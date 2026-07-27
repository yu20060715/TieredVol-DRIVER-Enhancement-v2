#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "tieredvol_common.h"
#include "tieredvol_types.h"
#include "version.h"
#include "tieredvol_discover.h"
#include "tieredvol_bench.h"
#include "tieredvol_exec.h"
#include "cmd_create.h"
#include "cmd_scheduler.h"

static int ensure_module_loaded(void) {
    FILE *f = fopen("/proc/modules", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "tieredvol ", 10) == 0) {
                fclose(f);
                return 0;
            }
        }
        fclose(f);
    }
    printf("  Loading tieredvol kernel module...\n");
    char *modprobe_argv[] = {"modprobe", "tieredvol", NULL};
    if (tv_exec_run("modprobe", modprobe_argv) == 0) return 0;
    fprintf(stderr, "  modprobe failed, trying insmod...\n");
    char insmod_path[512];
    ssize_t len = readlink("/proc/self/exe", insmod_path, sizeof(insmod_path) - 32);
    if (len > 0) {
        insmod_path[len] = '\0';
        char *slash = strrchr(insmod_path, '/');
        if (slash) {
            snprintf(slash + 1, sizeof(insmod_path) - (slash + 1 - insmod_path), "driver/tieredvol.ko");
            char *insmod_argv[] = {"insmod", insmod_path, NULL};
            if (tv_exec_run("insmod", insmod_argv) == 0) return 0;
        }
    }
    fprintf(stderr, "Error: cannot load tieredvol module.\n"
            "  Run: sudo make module_install\n");
    return TV_ERR;
}

static void cleanup_scheduler(const char *name, disk_t *valid, int valid_disks) {
    fprintf(stderr, "  Rolling back...\n");
    char target[256];
    snprintf(target, sizeof(target), "%s", name);
    char *dm_argv[] = {"sudo", "dmsetup", "remove", target, NULL};
    (void)tv_exec_sudo(dm_argv, 0);
    char conf_path[256];
    snprintf(conf_path, sizeof(conf_path), TV_CONFIG_DIR "%s.conf", name);
    char *rm_argv[] = {"sudo", "rm", "-f", conf_path, NULL};
    (void)tv_exec_sudo(rm_argv, 0);
    (void)valid; (void)valid_disks;
    fprintf(stderr, "  Rollback complete.\n");
}

int create_scheduler(int argc, char *argv[], char *name, char *disk_spec,
                     int auto_confirm) {
    (void)argc; (void)argv;
    disk_t disks_arr[TV_MAX_DISKS];
    int nd = 0;
    char buf[1024];
    strncpy(buf, disk_spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    char *tok = strtok(buf, ",");
    while (tok && nd < TV_MAX_DISKS) {
        memset(&disks_arr[nd], 0, sizeof(disk_t));
        strncpy(disks_arr[nd].disk, tok, 31);
        disks_arr[nd].disk[31] = 0;
        disks_arr[nd].carve_gb = 0;
        nd++;
        tok = strtok(NULL, ",");
    }

    if (nd == 0) {
        fprintf(stderr, "Error: no disks specified\n");
        return TV_ERR;
    }

    printf("=== TieredVol: Creating kernel dm target '%s' ===\n", name);

    disk_t dinfo[TV_MAX_DISKS];
    int ninfo = load_all_disk_info(dinfo, TV_MAX_DISKS);

    int valid_disks = 0;
    disk_t valid[TV_MAX_DISKS];

    for (int i = 0; i < nd; i++) {
        sysfs_model(disks_arr[i].disk, disks_arr[i].model, sizeof(disks_arr[i].model));
        disks_arr[i].size_gb = sysfs_size_gb(disks_arr[i].disk);
        disks_arr[i].is_root = 0;
        disks_arr[i].is_mounted = 0;
        for (int j = 0; j < ninfo; j++) {
            if (strcmp(disks_arr[i].disk, dinfo[j].disk) == 0) {
                strncpy(disks_arr[i].tran, dinfo[j].tran, sizeof(disks_arr[i].tran) - 1);
                disks_arr[i].is_root = dinfo[j].is_root;
                disks_arr[i].is_mounted = dinfo[j].is_mounted;
                break;
            }
        }

        if (disks_arr[i].is_root) {
            printf("  WARNING: /dev/%s is system disk, skipping\n", disks_arr[i].disk);
            continue;
        }
        if (disks_arr[i].is_mounted) {
            printf("  WARNING: /dev/%s is mounted, skipping\n", disks_arr[i].disk);
            continue;
        }
        if (disks_arr[i].size_gb <= 1) {
            fprintf(stderr, "Error: /dev/%s size not detected or too small\n", disks_arr[i].disk);
            return TV_ERR;
        }
        valid[valid_disks++] = disks_arr[i];
    }

    if (valid_disks == 0) {
        fprintf(stderr, "Error: no usable disks\n");
        return TV_ERR;
    }

    printf("  Benchmarking %d disks...\n", valid_disks);
    if (run_parallel_bench(valid, valid_disks, 1, NULL, NULL) != 0) {
        return TV_ERR;
    }
    qsort(valid, valid_disks, sizeof(disk_t), cmp_speed);

    printf("\n  %-12s %-10s %-10s %-8s %-10s\n", "DEVICE", "SIZE", "AVAIL", "SPEED", "TIER");
    printf("  %-12s %-10s %-10s %-8s %-10s\n", "------------", "----------", "----------", "--------", "----------");
    for (int i = 0; i < valid_disks; i++) {
        long long avail = valid[i].size_gb - 1;
        printf("  %-12s %-8lldGB %-8lldGB %-8.0f %-10s\n",
               valid[i].disk, valid[i].size_gb, avail, valid[i].speed_write,
               (i == 0) ? "FAST" : (i == valid_disks - 1) ? "SLOW" : "MED");
    }
    printf("\n");

    if (!auto_confirm) {
        printf("  Type YES to confirm: ");
        fflush(stdout);
        char confirm[16] = "";
        if (!fgets(confirm, sizeof(confirm), stdin) || strncmp(confirm, "YES", 3) != 0) {
            fprintf(stderr, "\nAborted by user.\n");
            return TV_ERR;
        }
    }
    printf("\n");

    if (ensure_module_loaded() != 0) return TV_ERR;

    printf("Step 1: Building weighted segments...\n");
    TV_DISK tv_disks[TV_MAX_DISKS];
    for (int i = 0; i < valid_disks; i++) {
        memset(&tv_disks[i], 0, sizeof(TV_DISK));
        tv_disks[i].id = i;
        snprintf(tv_disks[i].name, 63, "/dev/%s", valid[i].disk);
        tv_disks[i].free_size = (uint64_t)(valid[i].size_gb - 1) * 1024ULL * 1024 * 1024;
        tv_disks[i].speed = (uint64_t)valid[i].speed_write;
    }

    TV_METADATA meta;
    memset(&meta, 0, sizeof(meta));
    meta.version = 1;
    meta.chunk_size = TV_CHUNK_SIZE;
    meta.disk_count = (uint32_t)valid_disks;
    for (int i = 0; i < valid_disks; i++) {
        strncpy(meta.disk_names[i], tv_disks[i].name, 63);
    }

    TV_SEGMENT segs[TV_MAX_SEGS];
    int nsegs = 0;
    if (tv_build_segments(tv_disks, valid_disks, segs, &nsegs) < 0) {
        fprintf(stderr, "Error: failed to build segments\n");
        return TV_ERR;
    }

    meta.segment_count = (uint32_t)nsegs;
    memcpy(meta.segments, segs, sizeof(TV_SEGMENT) * nsegs);

    printf("  Segments: %d\n", nsegs);
    for (int i = 0; i < nsegs; i++) {
        printf("  Segment %d: [%lu, %lu) %u disks, stripe=%luKB\n",
               i, (unsigned long)segs[i].logical_begin,
               (unsigned long)segs[i].logical_end,
               segs[i].disk_count,
               (unsigned long)(segs[i].stripe_size / 1024));
    }

    printf("Step 2: Saving metadata...\n");
    mkdir(TV_CONFIG_DIR, 0755);
    char config_path[256];
    snprintf(config_path, sizeof(config_path), TV_CONFIG_DIR "%s.conf", name);

    if (tv_metadata_save(&meta, config_path) < 0) {
        fprintf(stderr, "Error: failed to save metadata to %s\n", config_path);
        return TV_ERR;
    }
    printf("  Saved to %s\n", config_path);

    printf("Step 3: Creating dm target...\n");
    {
        uint64_t total_bytes = segs[nsegs - 1].logical_end;
        long long total_sectors = (long long)(total_bytes / 512);
        char table[512];
        snprintf(table, sizeof(table), "0 %lld tieredvol %s", total_sectors, config_path);
        char *dm_argv[] = {"dmsetup", "create", name, NULL};
        int dm_ret = tv_exec_with_stdin("dmsetup", dm_argv, table);
        if (dm_ret != 0) {
            fprintf(stderr, "Error: dmsetup create failed (exit=%d)\n", dm_ret);
            cleanup_scheduler(name, valid, valid_disks);
            return TV_ERR;
        }
        printf("  Created /dev/mapper/%s (%lu GB)\n", name,
               (unsigned long)(total_bytes / (1024ULL * 1024 * 1024)));
    }

    printf("\n=== Kernel dm target '%s' created ===\n", name);
    printf("  Device: /dev/mapper/%s\n", name);
    printf("  Module: tieredvol %s\n", VERSION);
    printf("\nUse with:\n");
    printf("  sudo mkfs.ext4 /dev/mapper/%s\n", name);
    printf("  sudo mount /dev/mapper/%s /mnt/fast\n", name);

    return TV_OK;
}
