#include <stdio.h>
#include <string.h>
#include "../src/tieredvol_common.h"
#include "test_common.h"

static void test_valid_name(void) {
    printf("\n[TEST] tiered_is_valid_name\n");
    check(tiered_is_valid_name("fastpool") == 1, "fastpool ok");
    check(tiered_is_valid_name("pool-v2") == 1, "pool-v2 ok");
    check(tiered_is_valid_name("pool.v2") == 1, "pool.v2 ok");
    check(tiered_is_valid_name("Pool_01") == 1, "Pool_01 ok");
    check(tiered_is_valid_name("a") == 1, "single char ok");
    check(tiered_is_valid_name("") == 0, "empty name rejected");
    check(tiered_is_valid_name(NULL) == 0, "NULL rejected");
    check(tiered_is_valid_name("pool name") == 0, "space rejected");
    check(tiered_is_valid_name("pool;rm -rf /") == 0, "semicolon injection rejected");
    check(tiered_is_valid_name("pool|cat /etc/passwd") == 0, "pipe injection rejected");
    check(tiered_is_valid_name("pool$(whoami)") == 0, "dollar injection rejected");
    check(tiered_is_valid_name("pool`id`") == 0, "backtick injection rejected");
}

static void test_valid_fs(void) {
    printf("\n[TEST] tiered_is_valid_fs\n");
    check(tiered_is_valid_fs("ext4") == 1, "ext4 ok");
    check(tiered_is_valid_fs("ext3") == 1, "ext3 ok");
    check(tiered_is_valid_fs("xfs") == 1, "xfs ok");
    check(tiered_is_valid_fs("btrfs") == 1, "btrfs ok");
    check(tiered_is_valid_fs("none") == 1, "none ok");
    check(tiered_is_valid_fs("ntfs") == 0, "ntfs rejected");
    check(tiered_is_valid_fs("fat32") == 0, "fat32 rejected");
    check(tiered_is_valid_fs("ext4;rm -rf /") == 0, "injection rejected");
    check(tiered_is_valid_fs("") == 0, "empty rejected");
    check(tiered_is_valid_fs(NULL) == 0, "NULL rejected");
}

static void test_valid_mount(void) {
    printf("\n[TEST] tiered_is_valid_mount\n");
    check(tiered_is_valid_mount("/mnt/fast") == 1, "/mnt/fast ok");
    check(tiered_is_valid_mount("/home") == 1, "/home ok");
    check(tiered_is_valid_mount("/") == 1, "/ ok");
    check(tiered_is_valid_mount("relative") == 0, "relative rejected");
    check(tiered_is_valid_mount("") == 0, "empty rejected");
    check(tiered_is_valid_mount(NULL) == 0, "NULL rejected");
    check(tiered_is_valid_mount("/mnt/../etc") == 0, "/mnt/../etc rejected");
    check(tiered_is_valid_mount("/..") == 0, "/.. rejected");
    check(tiered_is_valid_mount("/mnt/data/..") == 0, "/mnt/data/.. rejected");
    check(tiered_is_valid_mount("/a") == 1, "/a short path ok");
    check(tiered_is_valid_mount("/ab") == 1, "/ab short path ok");
    check(tiered_is_valid_mount("/abc") == 1, "/abc ok");
}

int main(void) {
    printf("=== TieredVol Common Unit Tests ===\n");

    test_valid_name();
    test_valid_fs();
    test_valid_mount();

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
