#include <stdint.h>
#include "tieredvol_types.h"

TV_MAP tv_map_logical(uint64_t logical, TV_METADATA *meta) {
    TV_MAP err = {-1, 0, 0};
    if (!meta || meta->segment_count == 0) return err;

    /* Find segment by linear scan (could be binary search for large seg counts) */
    int seg_idx = -1;
    for (int i = 0; i < (int)meta->segment_count; i++) {
        if (logical >= meta->segments[i].logical_begin &&
            logical <  meta->segments[i].logical_end) {
            seg_idx = i;
            break;
        }
    }

    /* If not found, return error */
    if (seg_idx < 0) return err;

    TV_SEGMENT *seg = &meta->segments[seg_idx];

    if (seg->disk_count == 0 || seg->disk_count > TV_MAX_DISKS) return err;

    uint64_t stripe_no = (logical - seg->logical_begin) / seg->stripe_size;
    uint64_t offset_in = (logical - seg->logical_begin) % seg->stripe_size;

    /* Build prefix sum boundary */
    uint64_t boundary[TV_MAX_DISKS + 1];
    boundary[0] = 0;
    for (int i = 0; i < (int)seg->disk_count; i++) {
        boundary[i + 1] = boundary[i] + (uint64_t)seg->weight[i] * TV_CHUNK_SIZE;
    }

    /* Find which disk this offset falls into */
    int disk_idx = -1;
    for (int i = 0; i < (int)seg->disk_count; i++) {
        if (offset_in >= boundary[i] && offset_in < boundary[i + 1]) {
            disk_idx = i;
            break;
        }
    }

    if (disk_idx < 0) return err;

    TV_MAP map;
    map.disk = (int)seg->disk_index[disk_idx];
    map.offset = stripe_no * (uint64_t)seg->weight[disk_idx] * TV_CHUNK_SIZE
               + (offset_in - boundary[disk_idx]);
    map.length = (uint64_t)seg->weight[disk_idx] * TV_CHUNK_SIZE;

    return map;
}
