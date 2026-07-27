#include <stdio.h>
#include <string.h>
#include "../src/tieredvol_types.h"
#include "test_common.h"

static void test_weight_fast(void) {
    printf("\n[TEST] tv_compute_weight fast disk\n");
    uint32_t w = tv_compute_weight(3000, 500);
    check(w > 1, "fast disk gets weight > 1");
    check(w <= TV_MAX_WEIGHT, "weight capped at 16");
}

static void test_weight_slow(void) {
    printf("\n[TEST] tv_compute_weight slow disk\n");
    uint32_t w = tv_compute_weight(500, 3000);
    check(w >= 1, "slow disk gets weight >= 1");
}

static void test_weight_equal(void) {
    printf("\n[TEST] tv_compute_weight equal speeds\n");
    uint32_t w = tv_compute_weight(1000, 1000);
    check(w >= 1, "equal speeds give valid weight");
}

static void test_weight_fastest_is_self(void) {
    printf("\n[TEST] tv_compute_weight same speed\n");
    uint32_t w = tv_compute_weight(2000, 2000);
    check(w == 1, "same speed gives weight 1");
}

static void test_stripe_size_two_disks(void) {
    printf("\n[TEST] tv_compute_stripe_size two disks\n");
    uint32_t weights[] = { 3, 1 };
    uint64_t s = tv_compute_stripe_size(weights, 2, 1024 * 1024);
    check(s >= 1024 * 1024, "stripe >= chunk size");
    check(s % (1024 * 1024) == 0, "stripe is multiple of chunk");
}

static void test_stripe_size_four_disks(void) {
    printf("\n[TEST] tv_compute_stripe_size four disks\n");
    uint32_t weights[] = { 3, 2, 1, 1 };
    uint64_t s = tv_compute_stripe_size(weights, 4, 1024 * 1024);
    check(s > 0, "non-zero stripe size");
    check(s % (1024 * 1024) == 0, "stripe is multiple of chunk");
}

static void test_stripe_size_single(void) {
    printf("\n[TEST] tv_compute_stripe_size single disk\n");
    uint32_t weights[] = { 1 };
    uint64_t s = tv_compute_stripe_size(weights, 1, 1024 * 1024);
    check(s == 1024 * 1024, "single disk stripe = chunk");
}

static void test_build_segments_two_disks(void) {
    printf("\n[TEST] tv_build_segments two disks, equal speed\n");
    TV_DISK disks[2];
    memset(&disks, 0, sizeof(disks));
    disks[0].id = 0; strcpy(disks[0].name, "nvme0n1"); disks[0].free_size = 10ULL * 1024 * 1024 * 1024; disks[0].speed = 3000;
    disks[1].id = 1; strcpy(disks[1].name, "sda");      disks[1].free_size = 10ULL * 1024 * 1024 * 1024; disks[1].speed = 3000;

    TV_SEGMENT segs[TV_MAX_SEGS];
    int nsegs = 0;
    int ret = tv_build_segments(disks, 2, segs, &nsegs);
    check(ret == 0, "build succeeded");
    check(nsegs == 1, "single segment for equal-speed disks");
    check(segs[0].disk_count == 2, "segment covers both disks");
}

static void test_build_segments_three_tiers(void) {
    printf("\n[TEST] tv_build_segments three speed tiers\n");
    TV_DISK disks[3];
    memset(&disks, 0, sizeof(disks));
    disks[0].id = 0; strcpy(disks[0].name, "nvme0n1"); disks[0].free_size = 5ULL * 1024 * 1024 * 1024;  disks[0].speed = 3000;
    disks[1].id = 1; strcpy(disks[1].name, "sda");      disks[1].free_size = 20ULL * 1024 * 1024 * 1024; disks[1].speed = 500;
    disks[2].id = 2; strcpy(disks[2].name, "hda");      disks[2].free_size = 10ULL * 1024 * 1024 * 1024; disks[2].speed = 200;

    TV_SEGMENT segs[TV_MAX_SEGS];
    int nsegs = 0;
    int ret = tv_build_segments(disks, 3, segs, &nsegs);
    check(ret == 0, "build succeeded");
    check(nsegs == 3, "three segments for three tiers");
    check(segs[0].disk_count == 3, "first segment covers all disks");
}

static void test_build_segments_single(void) {
    printf("\n[TEST] tv_build_segments single disk\n");
    TV_DISK disks[1];
    memset(&disks, 0, sizeof(disks));
    disks[0].id = 0; strcpy(disks[0].name, "nvme0n1"); disks[0].free_size = 10ULL * 1024 * 1024 * 1024; disks[0].speed = 3000;

    TV_SEGMENT segs[TV_MAX_SEGS];
    int nsegs = 0;
    int ret = tv_build_segments(disks, 1, segs, &nsegs);
    check(ret == 0, "single disk build succeeded");
    check(nsegs == 1, "single segment");
    check(segs[0].disk_count == 1, "segment covers one disk");
}

int main(void) {
    printf("=== TieredVol Partition Unit Tests ===\n");

    test_weight_fast();
    test_weight_slow();
    test_weight_equal();
    test_weight_fastest_is_self();

    test_stripe_size_two_disks();
    test_stripe_size_four_disks();
    test_stripe_size_single();

    test_build_segments_two_disks();
    test_build_segments_three_tiers();
    test_build_segments_single();

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
