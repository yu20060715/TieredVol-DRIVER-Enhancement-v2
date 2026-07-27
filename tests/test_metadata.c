#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../src/tieredvol_types.h"
#include "test_common.h"

static TV_METADATA make_test_meta(void) {
    TV_METADATA m;
    memset(&m, 0, sizeof(m));
    m.version = 1;
    m.chunk_size = 1024 * 1024;
    m.segment_count = 1;
    m.disk_count = 2;
    strcpy(m.disk_names[0], "nvme0n1");
    strcpy(m.disk_names[1], "sda");

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

static int meta_equal(const TV_METADATA *a, const TV_METADATA *b) {
    if (a->version != b->version) return 0;
    if (a->chunk_size != b->chunk_size) return 0;
    if (a->segment_count != b->segment_count) return 0;
    if (a->disk_count != b->disk_count) return 0;
    for (uint32_t i = 0; i < a->disk_count; i++) {
        if (strcmp(a->disk_names[i], b->disk_names[i]) != 0) return 0;
    }
    for (uint32_t i = 0; i < a->segment_count; i++) {
        if (a->segments[i].logical_begin != b->segments[i].logical_begin) return 0;
        if (a->segments[i].logical_end != b->segments[i].logical_end) return 0;
        if (a->segments[i].disk_count != b->segments[i].disk_count) return 0;
        if (a->segments[i].stripe_size != b->segments[i].stripe_size) return 0;
        for (uint32_t j = 0; j < a->segments[i].disk_count; j++) {
            if (a->segments[i].weight[j] != b->segments[i].weight[j]) return 0;
            if (a->segments[i].disk_index[j] != b->segments[i].disk_index[j]) return 0;
        }
    }
    return 1;
}

static void test_save_load_roundtrip(void) {
    printf("\n[TEST] tv_metadata_save / tv_metadata_load round-trip\n");
    const char *path = "/tmp/.tv_test_meta.bin";
    TV_METADATA orig = make_test_meta();

    int ret = tv_metadata_save(&orig, path);
    check(ret == 0, "save succeeded");

    TV_METADATA loaded;
    memset(&loaded, 0, sizeof(loaded));
    ret = tv_metadata_load(&loaded, path);
    check(ret == 0, "load succeeded");

    check(meta_equal(&orig, &loaded), "loaded matches original");
    unlink(path);
}

static void test_load_nonexistent(void) {
    printf("\n[TEST] tv_metadata_load nonexistent file\n");
    TV_METADATA m;
    memset(&m, 0, sizeof(m));
    int ret = tv_metadata_load(&m, "/tmp/.tv_no_such_file.bin");
    check(ret < 0, "fails for nonexistent file");
}

static void test_save_corner(void) {
    printf("\n[TEST] tv_metadata_save edge cases\n");
    TV_METADATA m;
    memset(&m, 0, sizeof(m));
    m.version = 1;
    m.chunk_size = 1024 * 1024;
    m.segment_count = 0;
    m.disk_count = 0;

    const char *path = "/tmp/.tv_test_meta_empty.bin";
    int ret = tv_metadata_save(&m, path);
    check(ret == 0, "empty metadata saves ok");

    TV_METADATA loaded;
    memset(&loaded, 0xFF, sizeof(loaded));
    ret = tv_metadata_load(&loaded, path);
    check(ret == 0, "empty metadata loads ok");
    check(loaded.segment_count == 0, "loaded has 0 segments");
    check(loaded.disk_count == 0, "loaded has 0 disks");
    unlink(path);
}

static void test_save_load_multi_seg(void) {
    printf("\n[TEST] tv_metadata_save/load with multiple segments\n");
    const char *path = "/tmp/.tv_test_meta_multi.bin";
    TV_METADATA orig;
    memset(&orig, 0, sizeof(orig));
    orig.version = 1;
    orig.chunk_size = 1024 * 1024;
    orig.segment_count = 2;
    orig.disk_count = 3;
    strcpy(orig.disk_names[0], "nvme0n1");
    strcpy(orig.disk_names[1], "sda");
    strcpy(orig.disk_names[2], "hda");

    orig.segments[0].logical_begin = 0;
    orig.segments[0].logical_end = 5ULL * 1024 * 1024 * 1024;
    orig.segments[0].disk_count = 3;

    orig.segments[1].logical_begin = 5ULL * 1024 * 1024 * 1024;
    orig.segments[1].logical_end = 10ULL * 1024 * 1024 * 1024;
    orig.segments[1].disk_count = 2;

    int ret = tv_metadata_save(&orig, path);
    check(ret == 0, "save multi-segment succeeded");

    TV_METADATA loaded;
    memset(&loaded, 0, sizeof(loaded));
    ret = tv_metadata_load(&loaded, path);
    check(ret == 0, "load multi-segment succeeded");
    check(meta_equal(&orig, &loaded), "multi-segment loaded matches original");
    unlink(path);
}

static void test_load_long_disk_name(void) {
    printf("\n[TEST] tv_metadata_save/load with long disk name\n");
    const char *path = "/tmp/.tv_test_meta_longname.bin";
    TV_METADATA orig;
    memset(&orig, 0, sizeof(orig));
    orig.version = 1;
    orig.chunk_size = 1024 * 1024;
    orig.disk_count = 1;
    /* Fill disk name with 63 chars (max is TV_MAX_NAME=64 including null) */
    memset(orig.disk_names[0], 'a', 63);
    orig.disk_names[0][63] = '\0';
    orig.segment_count = 1;
    orig.segments[0].logical_begin = 0;
    orig.segments[0].logical_end = 10ULL * 1024 * 1024 * 1024;
    orig.segments[0].disk_count = 1;
    orig.segments[0].weight[0] = 1;
    orig.segments[0].stripe_size = 1024 * 1024;

    int ret = tv_metadata_save(&orig, path);
    check(ret == 0, "save with long name succeeded");

    TV_METADATA loaded;
    memset(&loaded, 0, sizeof(loaded));
    ret = tv_metadata_load(&loaded, path);
    check(ret == 0, "load with long name succeeded");
    check(strcmp(loaded.disk_names[0], orig.disk_names[0]) == 0, "long name preserved");
    unlink(path);
}

int main(void) {
    printf("=== TieredVol Metadata Unit Tests ===\n");

    test_save_load_roundtrip();
    test_load_nonexistent();
    test_save_corner();
    test_save_load_multi_seg();
    test_load_long_disk_name();

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
