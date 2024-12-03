/**
 * @file mochi-plumber.c
 *
 * (C) The University of Chicago
 *
 * See COPYRIGHT in top-level directory.
 */

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/types.h>
#include <rdma/fabric.h>
#include <rdma/fi_errno.h>
#include <hwloc.h>

#include "mochi-plumber-private.h"

struct bucket {
    int num_nics;
    char** nics;
};

static int select_nic(hwloc_topology_t* topology, const char* bucket_policy, const char* nic_policy, int nbuckets, struct bucket* buckets, const char** out_nic);
static int select_nic_roundrobin(int bucket_idx, struct bucket* bucket, const char** out_nic);
static int select_nic_random(int bucket_idx, struct bucket* bucket, const char** out_nic);

int mochi_plumber_resolve_nic(const char* in_address, const char* bucket_policy, const char* nic_policy, char** out_address) {

    int nbuckets = 0;
    hwloc_topology_t     topology;
    hwloc_const_bitmap_t nset_all;
    struct bucket *buckets = NULL;
    struct fi_info* info;
    struct fi_info* hints;
    struct fi_info* cur;
    int             ret;
    hwloc_obj_t      pci_dev;
    hwloc_obj_t      non_io_ancestor;
    int bucket_idx = 0;
    int i;
    const char* selected_nic;

    /* for now we only manipulate CXI addresses */
    if(strncmp(in_address, "cxi", strlen("cxi")) != 0 &&
        strncmp(in_address, "ofi+cxi", strlen("ofi+cxi")) != 0)
    {
        /* don't know what this is; just pass it through */
        *out_address = strdup(in_address);
        return(0);
    }

    /* check to make sure the input address is not specific already */
    if(in_address[strlen(in_address)-1] != '/' ||
        in_address[strlen(in_address)-2] != '/')
    {
        /* the address is already resolved to some degree; don't touch it */
        *out_address = strdup(in_address);
        return(0);
    }

    /* get topology */
    hwloc_topology_init(&topology);
    hwloc_topology_set_io_types_filter(topology, HWLOC_TYPE_FILTER_KEEP_IMPORTANT);
    hwloc_topology_load(topology);

    /* figure out how many buckets there will be */
    if(strcmp(bucket_policy, "all") == 0) {
        /* just one big bucket */
        nbuckets = 1;
    }
    else if(strcmp(bucket_policy, "numa") == 0) {
        /* we need to query number of numa domains and make a bucket for
         * each
         */
        nset_all = hwloc_topology_get_complete_nodeset(topology);
        nbuckets = hwloc_bitmap_weight(nset_all);
    }
    else {
        fprintf(stderr, "mochi_plumber_resolve_nic: unknown bucket policy \"%s\"\n", bucket_policy);
        hwloc_topology_destroy(topology);
        return(-1);
    }

    buckets = calloc(nbuckets, sizeof(*buckets));
    if(!buckets) {
        hwloc_topology_destroy(topology);
        return(-1);
    }

    /* query libfabric for interfaces */
    hints = fi_allocinfo();
    assert(hints);
    /* These are required as input if we want to filter the results; they
     * indicate functionality that the caller is prepared to provide.  This
     * is just a query, so we want wildcard options except that we must disable
     * deprecated memory registration modes.
     */
    hints->mode                 = ~0;
    hints->domain_attr->mode    = ~0;
    hints->domain_attr->mr_mode = ~3;
    /* only supporting cxi for now */
    hints->fabric_attr->prov_name = strdup("cxi");
    hints->ep_attr->protocol = FI_PROTO_CXI;
    ret = fi_getinfo(FI_VERSION(FI_MAJOR_VERSION, FI_MINOR_VERSION), NULL, NULL,
                     0, hints, &info);
    if (ret != 0) {
        fprintf(stderr, "fi_getinfo: %d (%s)\n", ret, fi_strerror(-ret));
        fi_freeinfo(hints);
        free(buckets);
        hwloc_topology_destroy(topology);
        return (ret);
    }
    fi_freeinfo(hints);

    /* iterate through interfaces and assign to buckets */
    for (cur = info; cur; cur = cur->next) {
        if (cur->nic && cur->nic->bus_attr
            && cur->nic->bus_attr->bus_type == FI_BUS_PCI) {

            /* look for this device in hwloc topology */
            struct fi_pci_attr pci = cur->nic->bus_attr->attr.pci;
            pci_dev = hwloc_get_pcidev_by_busid(topology, pci.domain_id,
                                            pci.bus_id, pci.device_id,
                                            pci.function_id);
            if(!pci_dev) {
                fprintf(stderr, "Error: can't find %s in hwloc topology.\n",
                    cur->domain_attr->name);
                fi_freeinfo(info);
                free(buckets);
                hwloc_topology_destroy(topology);
                return(-1);
            }
            if(nbuckets == 1) {
                /* add to the global bucket */
                bucket_idx = 0;
            }
            else {
                /* figure out what numa domain this maps to and put it in
                 * that bucket
                 */
                non_io_ancestor = hwloc_get_non_io_ancestor_obj(topology, pci_dev);
                bucket_idx = hwloc_bitmap_first(non_io_ancestor->nodeset);
            }

            buckets[bucket_idx].num_nics++;
            /* TODO: err handling */
            buckets[bucket_idx].nics = realloc(buckets[bucket_idx].nics, buckets[bucket_idx].num_nics*sizeof(*buckets[bucket_idx].nics));
            buckets[bucket_idx].nics[buckets[bucket_idx].num_nics-1] = strdup(cur->domain_attr->name);
        }
    }
    fi_freeinfo(info);

    /* sanity check: every bucket must have at least one NIC */
    for(i=0; i<nbuckets; i++) {
        if(buckets[i].num_nics < 1) {
            fprintf(stderr, "Error: bucket %d has no NICs\n", i);
            /* TODO: bucket cleanup */
            free(buckets);
            hwloc_topology_destroy(topology);
            return(-1);
        }
    }

    ret = select_nic(&topology, bucket_policy, nic_policy, nbuckets, buckets, &selected_nic);
    if(ret < 0) {
        fprintf(stderr, "Error: failed to select NIC.\n");
        /* TODO: bucket cleanup */
        free(buckets);
        hwloc_topology_destroy(topology);
        return(-1);
    }

    /* TODO: bucket cleanup */
    free(buckets);
    hwloc_topology_destroy(topology);

    /* generate new address! */
    *out_address = malloc(strlen(in_address) + strlen(selected_nic) + 1);
    sprintf(*out_address, "%s%s", in_address, selected_nic);

    return(0);
}

static int select_nic(hwloc_topology_t* topology, const char* bucket_policy, const char* nic_policy, int nbuckets, struct bucket* buckets, const char** out_nic) {
    int bucket_idx = 0;
    int ret;
    hwloc_cpuset_t       last_cpu;
    hwloc_nodeset_t      last_numa;

    /* figure out which bucket to draw from */
    if(nbuckets == 1)
        bucket_idx = 0;
    else {
        if(strcmp(bucket_policy, "numa") == 0) {
            last_cpu = hwloc_bitmap_alloc();
            last_numa = hwloc_bitmap_alloc();
            assert(last_cpu && last_numa);

            /* select a bucket based on the numa domain that this process is
             * executing in
             */
            ret = hwloc_get_last_cpu_location(*topology, last_cpu, HWLOC_CPUBIND_THREAD);
            if(ret < 0) {
                hwloc_bitmap_free(last_cpu);
                hwloc_bitmap_free(last_numa);
                fprintf(stderr, "hwloc_get_last_cpu_location() failure.\n");
                return(-1);
            }
            hwloc_cpuset_to_nodeset(*topology, last_cpu, last_numa);
            bucket_idx = hwloc_bitmap_first(last_numa);
            assert(bucket_idx < nbuckets);

            hwloc_bitmap_free(last_cpu);
            hwloc_bitmap_free(last_numa);
        } else {
            fprintf(stderr, "Error: inconsistent bucket policy %s.\n", bucket_policy);
            return(-1);
        }
    }

    /* select a NIC from within the chosen bucket */
    if(buckets[bucket_idx].num_nics == 1) {
        *out_nic = buckets[bucket_idx].nics[0];
        return(0);
    }

    if(strcmp(nic_policy, "roundrobin") == 0) {
        ret = select_nic_roundrobin(bucket_idx, &buckets[bucket_idx], out_nic);
    }
    else if(strcmp(nic_policy, "random") == 0) {
        ret = select_nic_random(bucket_idx, &buckets[bucket_idx], out_nic);
    }
    else {
        fprintf(stderr, "Error: unknown nic_policy \"%s\"\n", nic_policy);
        ret = -1;
    }

    return(ret);
}

static int select_nic_roundrobin(int bucket_idx, struct bucket* bucket, const char** out_nic) {
    int ret;
    char tokenpath[256] = {0};
    int fd;
    int nic_idx = -1;

    snprintf(tokenpath, 256, "/tmp/%s-mochi-plumber", getlogin());
    ret = mkdir(tokenpath, 0700);
    if(ret !=0 && errno != EEXIST) {
        perror("mkdir");
        fprintf(stderr, "Error: failed to create %s\n", tokenpath);
        return(-1);
    }

    snprintf(tokenpath, 256, "/tmp/%s-mochi-plumber/%d", getlogin(), bucket_idx);
    fd = open(tokenpath, O_RDWR|O_CREAT|O_SYNC, 0600);
    if(fd < 0) {
        perror("open");
        fprintf(stderr, "Error: failed to open %s\n", tokenpath);
    }

    /* exlusive lock file */
    flock(fd, LOCK_EX);

    /* read most recently used nic index */
    /* note: if value hasn't been set yet (pread returns 0), nic_idx was
     * initialized to -1
     */
    ret = pread(fd, &nic_idx, sizeof(nic_idx), 0);
    if(ret < 0) {
        perror("pread");
        fprintf(stderr, "Error: failed to read %s\n", tokenpath);
        flock(fd, LOCK_UN);
        return(-1);
    }
    /* select next nic */
    nic_idx = (nic_idx+1)%(bucket->num_nics);
    /* write selection back to file */
    ret = pwrite(fd, &nic_idx, sizeof(nic_idx), 0);
    if(ret < 0) {
        perror("pwrite");
        fprintf(stderr, "Error: failed to write %s\n", tokenpath);
        flock(fd, LOCK_UN);
        return(-1);
    }
    flock(fd, LOCK_UN);

    *out_nic = bucket->nics[nic_idx];
    return(0);
}

static int select_nic_random(int bucket_idx, struct bucket* bucket, const char** out_nic) {
    int nic_idx = -1;

    /* we only need to worry about unique seeding within a single node, so
     * its sufficient to just use the pid
     */
    srand(getpid());
    nic_idx = rand() % bucket->num_nics;

    *out_nic = bucket->nics[nic_idx];
    return(0);
}
