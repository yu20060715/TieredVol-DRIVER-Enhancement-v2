#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tieredvol_types.h"

uint32_t tv_compute_weight(uint64_t speed, uint64_t slowest) {
    if (slowest == 0) return 1;
    double w = (double)speed / (double)slowest;
    uint32_t result = (uint32_t)(w + 0.5);
    if (result > TV_MAX_WEIGHT) result = TV_MAX_WEIGHT;
    if (result < 1) result = 1;
    return result;
}

uint64_t tv_compute_stripe_size(uint32_t *weights, int nweights, uint32_t chunk_size) {
    uint32_t sum = 0;
    for (int i = 0; i < nweights; i++) sum += weights[i];
    return (uint64_t)sum * chunk_size;
}

int tv_build_segments(TV_DISK *disks, int ndisks, TV_SEGMENT *segs, int *nsegs) {
    if (ndisks <= 0 || !disks || !segs || !nsegs) return TV_ERR;
    *nsegs = 0;

    /* Find slowest speed */
    uint64_t slowest = disks[0].speed;
    for (int i = 1; i < ndisks; i++) {
        if (disks[i].speed < slowest) slowest = disks[i].speed;
    }
    if (slowest == 0) slowest = 1;

    /* Compute weights */
    for (int i = 0; i < ndisks; i++) {
        disks[i].weight = tv_compute_weight(disks[i].speed, slowest);
    }

    /* Build capacity-sorted boundary list */
    int sorted_idx[TV_MAX_DISKS];

    /* Simple insertion sort by free_size ascending */
    for (int i = 0; i < ndisks; i++) sorted_idx[i] = i;
    for (int i = 1; i < ndisks; i++) {
        int key = sorted_idx[i];
        int j = i - 1;
        while (j >= 0 && disks[sorted_idx[j]].free_size > disks[key].free_size) {
            sorted_idx[j + 1] = sorted_idx[j];
            j--;
        }
        sorted_idx[j + 1] = key;
    }

    /* Build segments at each capacity boundary */
    uint64_t prev_boundary = 0;
    int seg_count = 0;

    for (int i = 0; i < ndisks && seg_count < TV_MAX_SEGS; i++) {
        uint64_t boundary = disks[sorted_idx[i]].free_size;
        if (boundary <= prev_boundary) continue;

        TV_SEGMENT *seg = &segs[seg_count];
        memset(seg, 0, sizeof(TV_SEGMENT));
        seg->logical_begin = prev_boundary;
        seg->logical_end = boundary;

        /* All disks from i to ndisks-1 participate in this segment */
        seg->disk_count = ndisks - i;
        for (int j = i; j < ndisks; j++) {
            seg->disk_index[j - i] = (uint32_t)sorted_idx[j];
            seg->weight[j - i] = disks[sorted_idx[j]].weight;
        }

        seg->stripe_size = tv_compute_stripe_size(
            seg->weight, seg->disk_count, TV_CHUNK_SIZE);

        prev_boundary = boundary;
        seg_count++;
    }

    /* Warn if stripe sizes differ across segments (expected for unequal capacities) */
    if (seg_count > 1) {
        uint64_t ref_stripe = segs[0].stripe_size;
        for (int i = 1; i < seg_count; i++) {
            if (segs[i].stripe_size != ref_stripe) {
                fprintf(stderr, "tv: WARNING: stripe sizes differ across segments "
                        "(seg0=%lu, seg%d=%lu) — unequal capacities may cause "
                        "incorrect offset calculations in flush_submit_io\n",
                        (unsigned long)ref_stripe, i, (unsigned long)segs[i].stripe_size);
                break;
            }
        }
    }

    *nsegs = seg_count;
    return 0;
}
