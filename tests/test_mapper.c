#include <stdio.h>
#include <string.h>
#include "../src/tieredvol_types.h"
#include "test_common.h"

static TV_METADATA single_seg_meta(void) {
    TV_METADATA m;
    memset(&m, 0, sizeof(m));
    m.version = 1;
    m.chunk_size = 1024 * 1024;
    m.disk_count = 2;
    strcpy(m.disk_names[0], "nvme0n1");
    strcpy(m.disk_names[1], "sda");

    m.segment_count = 1;
    m.segments[0].logical_begin = 0;
    m.segments[0].logical_end = 10ULL * 1024 * 1024 * 1024;
    m.segments[0].disk_count = 2;
    m.segments[0].disk_index[0] = 0;
    m.segments[0].disk_index[1] = 1;
    m.segments[0].weight[0] = 3;
    m.segments[0].weight[1] = 1;
    m.segments[0].stripe_size = 4ULL * 1024 * 1024;
    return m;
}

static TV_METADATA dual_seg_meta(void) {
    TV_METADATA m;
    memset(&m, 0, sizeof(m));
    m.version = 1;
    m.chunk_size = 1024 * 1024;
    m.disk_count = 2;
    strcpy(m.disk_names[0], "nvme0n1");
    strcpy(m.disk_names[1], "sda");

    m.segment_count = 2;
    m.segments[0].logical_begin = 0;
    m.segments[0].logical_end = 5ULL * 1024 * 1024 * 1024;
    m.segments[0].disk_count = 2;
    m.segments[0].disk_index[0] = 0;
    m.segments[0].disk_index[1] = 1;
    m.segments[0].weight[0] = 3;
    m.segments[0].weight[1] = 1;
    m.segments[0].stripe_size = 4ULL * 1024 * 1024;

    m.segments[1].logical_begin = 5ULL * 1024 * 1024 * 1024;
    m.segments[1].logical_end = 10ULL * 1024 * 1024 * 1024;
    m.segments[1].disk_count = 2;
    m.segments[1].disk_index[0] = 1;
    m.segments[1].disk_index[1] = 0;
    m.segments[1].weight[0] = 2;
    m.segments[1].weight[1] = 2;
    m.segments[1].stripe_size = 4ULL * 1024 * 1024;
    return m;
}

static void test_map_single_seg_begin(void) {
    printf("\n[TEST] tv_map_logical single segment, offset 0\n");
    TV_METADATA m = single_seg_meta();
    TV_MAP map = tv_map_logical(0, &m);
    check(map.disk == 0, "maps to disk 0 (nvme0n1)");
    check(map.offset == 0, "offset 0");
    check(map.length == m.segments[0].weight[0] * TV_CHUNK_SIZE, "length = weight * chunk");
}

static void test_map_single_seg_mid(void) {
    printf("\n[TEST] tv_map_logical single segment, mid offset\n");
    TV_METADATA m = single_seg_meta();
    uint64_t stripe = m.segments[0].stripe_size;
    /* offset = 2 stripes + 1 byte → offset_in_stripe = 1 → disk 0 (weight[0]=3) */
    TV_MAP map = tv_map_logical(stripe * 2 + 1, &m);
    check(map.disk == 0, "offset 1 byte into stripe → disk 0 (weight[0]=3)");
    check(map.offset > 0, "non-zero offset");
    check(map.length > 0, "non-zero length");
}

static void test_map_single_seg_boundary(void) {
    printf("\n[TEST] tv_map_logical single segment, near end\n");
    TV_METADATA m = single_seg_meta();
    uint64_t end = m.segments[0].logical_end;
    /* end-1 is in the last stripe → determine expected disk from weight bounds */
    uint64_t offset_in_seg = (end - 1) - m.segments[0].logical_begin;
    uint64_t stripe_size = m.segments[0].stripe_size;
    uint64_t offset_in_stripe = offset_in_seg % stripe_size;
    int expected_disk = 0;
    uint64_t bound = 0;
    for (uint32_t i = 0; i < m.segments[0].disk_count; i++) {
        bound += (uint64_t)m.segments[0].weight[i] * m.chunk_size;
        if (offset_in_stripe < bound) { expected_disk = (int)i; break; }
    }
    TV_MAP map = tv_map_logical(end - 1, &m);
    check(map.disk == expected_disk, "disk matches weight boundary calculation");
    check(map.length > 0, "non-zero length");
}

static void test_map_dual_seg_first(void) {
    printf("\n[TEST] tv_map_logical dual segment, first segment\n");
    TV_METADATA m = dual_seg_meta();
    TV_MAP map = tv_map_logical(0, &m);
    check(map.disk == 0, "segment 0, offset 0 -> disk 0");
}

static void test_map_dual_seg_second(void) {
    printf("\n[TEST] tv_map_logical dual segment, second segment\n");
    TV_METADATA m = dual_seg_meta();
    uint64_t boundary = m.segments[0].logical_end;
    /* boundary = start of segment 1 → offset_in_seg_1 = 0 → disk = seg[1].disk_index[0] = 1 */
    TV_MAP map = tv_map_logical(boundary, &m);
    check(map.disk == 1, "segment 1 offset 0 → disk_index[0]=1 (sda)");
}

static void test_map_null_meta(void) {
    printf("\n[TEST] tv_map_logical NULL meta\n");
    TV_MAP map = tv_map_logical(0, NULL);
    check(map.disk == -1, "NULL meta returns error");
}

static void test_map_out_of_range(void) {
    printf("\n[TEST] tv_map_logical out-of-range offset\n");
    TV_METADATA m = single_seg_meta();
    TV_MAP map = tv_map_logical(m.segments[0].logical_end + 1, &m);
    check(map.disk == -1, "out-of-range offset returns error");
}

static void test_map_offset_eq_stripe(void) {
    printf("\n[TEST] tv_map_logical offset == stripe_size\n");
    TV_METADATA m = single_seg_meta();
    /* offset = stripe_size → start of second stripe → disk 0, physical = 1 * weight[0] * chunk */
    TV_MAP map = tv_map_logical(m.segments[0].stripe_size, &m);
    check(map.disk == 0, "offset == stripe_size maps to disk 0");
    uint64_t expected_off = (uint64_t)m.segments[0].weight[0] * m.chunk_size;
    check(map.offset == expected_off, "physical offset = weight * chunk for stripe 1");
}

int main(void) {
    printf("=== TieredVol Mapper Unit Tests ===\n");

    test_map_single_seg_begin();
    test_map_single_seg_mid();
    test_map_single_seg_boundary();
    test_map_dual_seg_first();
    test_map_dual_seg_second();
    test_map_null_meta();
    test_map_out_of_range();
    test_map_offset_eq_stripe();

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
