#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include "tieredvol_common.h"
#include "tieredvol_types.h"
#include "version.h"
#include "tieredvol_discover.h"
#include "tieredvol_bench.h"
#include "tieredvol_exec.h"
#include "cmd_create.h"
#include "cmd_scheduler.h"

void make_target(char *out, size_t sz, const char *disk) {
    snprintf(out, sz, "tv_%s_carve", disk);
}

void cleanup_create(const char *name, disk_t *valid, int valid_disks) {
    fprintf(stderr, "  Rolling back...\n");
    char umount_lv[256];
    snprintf(umount_lv, sizeof(umount_lv), "/dev/mapper/tv_vg_%s-tv_lv_%s", name, name);
    char *umount_argv[] = {"umount", umount_lv, NULL};
    (void)tv_exec_quiet("sudo", umount_argv);
    char full_lv[256];
    snprintf(full_lv, sizeof(full_lv), "tv_vg_%s/tv_lv_%s", name, name);
    char *const lv_argv[] = {"lvremove", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-f", full_lv, NULL};
    (void)tv_exec_run("lvremove", lv_argv);
    char vg_name[128];
    snprintf(vg_name, sizeof(vg_name), "tv_vg_%s", name);
    char *const vg_argv2[] = {"vgremove", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-f", vg_name, NULL};
    (void)tv_exec_run("vgremove", vg_argv2);
    for (int i = 0; i < valid_disks; i++) {
        char target[64];
        make_target(target, sizeof(target), valid[i].disk);
        char devpath[128];
        snprintf(devpath, sizeof(devpath), "/dev/mapper/%s", target);
        char *const pv_argv[] = {"pvremove", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-ff", "-y", devpath, NULL};
        (void)tv_exec_run("pvremove", pv_argv);
        char *const dm_argv[] = {"sudo", "dmsetup", "remove", target, NULL};
        (void)tv_exec_sudo(dm_argv, 0);
    }
    fprintf(stderr, "  Rollback complete.\n");
}

int cmd_create(int argc, char *argv[]) {
    char *name = NULL;
    char *disk_spec = NULL;
    char *fs = "ext4";
    char *mount_point = NULL;
    int stripe_size_kb = DEFAULT_STRIPE_SIZE_KB;
    int user_stripesize = 0;
    int use_scheduler = 0;
    int auto_confirm = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) name = argv[++i];
        else if (strcmp(argv[i], "--disks") == 0 && i + 1 < argc) disk_spec = argv[++i];
        else if (strcmp(argv[i], "--fs") == 0 && i + 1 < argc) fs = argv[++i];
        else if (strcmp(argv[i], "--mount") == 0 && i + 1 < argc) mount_point = argv[++i];
        else if (strcmp(argv[i], "--scheduler") == 0) use_scheduler = 1;
        else if (strcmp(argv[i], "--yes") == 0) auto_confirm = 1;
        else if (strcmp(argv[i], "--stripesize") == 0 && i + 1 < argc) {
            char *endptr;
            long val = strtol(argv[++i], &endptr, 10);
            if (*endptr != '\0' || endptr == argv[i] || val <= 0 || val > 1048576) {
                fprintf(stderr, "Error: invalid stripesize '%s'\n", argv[i]);
                return TV_ERR;
            }
            stripe_size_kb = (int)val;
            user_stripesize = 1;
        }
    }

    if (!name || !disk_spec) {
        fprintf(stderr, "Usage: tiered_setup --create --name NAME --disks sdb,sdc [--scheduler] [--fs ext4] [--mount /mnt/fast]\n");
        return TV_ERR;
    }

    if (!tiered_is_valid_name(name)) {
        fprintf(stderr, "Error: invalid name '%s'\n", name);
        return TV_ERR;
    }

    if (use_scheduler) {
        return create_scheduler(argc, argv, name, disk_spec, auto_confirm);
    }

    if (!tiered_is_valid_fs(fs)) {
        fprintf(stderr, "Error: invalid filesystem '%s'\n", fs);
        return TV_ERR;
    }
    if (mount_point && !tiered_is_valid_mount(mount_point)) {
        fprintf(stderr, "Error: mount point invalid\n");
        return TV_ERR;
    }

    disk_t disks_arr[TV_MAX_DISKS];
    int nd = 0;
    char buf[1024];
    strncpy(buf, disk_spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    char *tok = strtok(buf, ",");
    while (tok && nd < TV_MAX_DISKS) {
        memset(&disks_arr[nd], 0, sizeof(disk_t));
        char *colon = strchr(tok, ':');
        if (colon) {
            *colon = 0;
            strncpy(disks_arr[nd].disk, tok, 31);
            disks_arr[nd].disk[31] = 0;
            char *endptr;
            long long val = strtoll(colon + 1, &endptr, 10);
            if (*endptr != '\0' || endptr == colon + 1 || val <= 0) {
                fprintf(stderr, "Error: invalid carve size '%s'\n", colon + 1);
                return TV_ERR;
            }
            disks_arr[nd].carve_gb = val;
        } else {
            strncpy(disks_arr[nd].disk, tok, 31);
            disks_arr[nd].disk[31] = 0;
            disks_arr[nd].carve_gb = 0;
        }
        nd++;
        tok = strtok(NULL, ",");
    }

    if (nd == 0) {
        fprintf(stderr, "Error: no disks specified\n");
        return TV_ERR;
    }

    printf("=== TieredVol: Creating LVM volume '%s' ===\n", name);

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
        if (disks_arr[i].is_root) { printf("  WARNING: /dev/%s is system disk, skipping\n", disks_arr[i].disk); continue; }
        if (disks_arr[i].is_mounted) { printf("  WARNING: /dev/%s is mounted, skipping\n", disks_arr[i].disk); continue; }
        {
            char target[64];
            make_target(target, sizeof(target), disks_arr[i].disk);
            char devpath[128];
            snprintf(devpath, sizeof(devpath), "/dev/mapper/%s", target);
            struct stat st;
            if (stat(devpath, &st) == 0) {
                fprintf(stderr, "Error: /dev/%s is already carved as %s\n", disks_arr[i].disk, target);
                return TV_ERR;
            }
        }
        if (disks_arr[i].size_gb <= 1) { fprintf(stderr, "Error: /dev/%s too small\n", disks_arr[i].disk); return TV_ERR; }
        if (disks_arr[i].carve_gb <= 0) disks_arr[i].carve_gb = disks_arr[i].size_gb - 1;
        if (disks_arr[i].carve_gb > disks_arr[i].size_gb - 1) {
            fprintf(stderr, "Error: /dev/%s has %lldGB, cannot carve %lldGB\n", disks_arr[i].disk, disks_arr[i].size_gb, disks_arr[i].carve_gb);
            return TV_ERR;
        }
        valid[valid_disks++] = disks_arr[i];
    }

    if (valid_disks == 0) { fprintf(stderr, "Error: no usable disks\n"); return TV_ERR; }

    printf("  Benchmarking %d disks in parallel...\n", valid_disks);
    if (run_parallel_bench(valid, valid_disks, 1, NULL, NULL) != 0) { cleanup_create(name, valid, valid_disks); return TV_ERR; }
    qsort(valid, valid_disks, sizeof(disk_t), cmp_speed);

    long long total_gb = 0;
    for (int i = 0; i < valid_disks; i++) total_gb += valid[i].carve_gb;
    if (!user_stripesize) { int has_sata = 0; for (int i = 0; i < valid_disks; i++) { if (strcmp(valid[i].tran, "sata") == 0 || strcmp(valid[i].tran, "sas") == 0) { has_sata = 1; break; } } stripe_size_kb = has_sata ? 512 : 64; }

    printf("\nConfiguration:\n  Name: %s\n  Disks: %d\n  Total: %lldGB\n  FS: %s\n  Stripesize: %dKB\n\n", name, valid_disks, total_gb, fs, stripe_size_kb);
    double total_speed = 0;
    for (int i = 0; i < valid_disks; i++) total_speed += valid[i].speed_write;
    for (int i = 0; i < valid_disks; i++) printf("  %-12s %8lldGB %8.0f MB/s  %s\n", valid[i].disk, valid[i].carve_gb, valid[i].speed_write, (i == 0) ? "FAST" : (i == valid_disks - 1) ? "SLOW" : "MED");
    printf("  Total: %.0f MB/s\n\n", total_speed);

    printf("  WARNING: Disks will be overwritten. Data will be PERMANENTLY LOST.\n\n");
    if (!auto_confirm) { printf("  Type YES to confirm: "); fflush(stdout); char confirm[16] = ""; if (!fgets(confirm, sizeof(confirm), stdin) || strncmp(confirm, "YES", 3) != 0) { fprintf(stderr, "\nAborted.\n"); return TV_ERR; } }

    printf("Step 1: Cleaning up old targets...\n");
    for (int i = 0; i < valid_disks; i++) {
        char mp[512]; find_mount_for_disk(valid[i].disk, mp, sizeof(mp));
        if (mp[0]) { char *ua[] = {"sudo", "umount", mp, NULL}; (void)tv_exec_sudo(ua, 0); }
        char target[64]; make_target(target, sizeof(target), valid[i].disk);
        { char *da[] = {"sudo", "dmsetup", "remove", target, NULL}; (void)tv_exec_sudo(da, 1); }
    }
    { char fl[256]; snprintf(fl, sizeof(fl), "tv_vg_%s/tv_lv_%s", name, name); char *la[] = {"lvremove", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-f", fl, NULL}; (void)tv_exec_quiet("lvremove", la); }
    { char vn[128]; snprintf(vn, sizeof(vn), "tv_vg_%s", name); char *va[] = {"vgremove", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-f", vn, NULL}; (void)tv_exec_quiet("vgremove", va); }
    for (int i = 0; i < valid_disks; i++) {
        char target[64]; make_target(target, sizeof(target), valid[i].disk);
        char dp[128]; snprintf(dp, sizeof(dp), "/dev/mapper/%s", target);
        char *pa[] = {"pvremove", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-ff", "-y", dp, NULL}; (void)tv_exec_quiet("pvremove", pa);
    }

    printf("Step 2: Creating carved targets...\n");
    for (int i = 0; i < valid_disks; i++) {
        long long sectors = valid[i].carve_gb * 1024LL * 1024 * 1024 / 512;
        char target[64]; make_target(target, sizeof(target), valid[i].disk);
        char table[128]; snprintf(table, sizeof(table), "0 %lld linear /dev/%s 0\n", sectors, valid[i].disk);
        char *dm_argv[] = {"sudo", "dmsetup", "create", target, NULL};
        if (tv_exec_with_stdin("sudo", dm_argv, table) != 0) { fprintf(stderr, "Error: dmsetup create failed for %s\n", valid[i].disk); cleanup_create(name, valid, i); return TV_ERR; }
        printf("  Created %s (%lldGB)\n", target, valid[i].carve_gb);
    }

    printf("Step 3: Creating LVM physical volumes...\n");
    for (int i = 0; i < valid_disks; i++) {
        char target[64]; make_target(target, sizeof(target), valid[i].disk);
        char dp[128]; snprintf(dp, sizeof(dp), "/dev/mapper/%s", target);
        char *pa[] = {"pvcreate", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-f", dp, NULL};
        if (tv_exec_run("pvcreate", pa) != 0) { fprintf(stderr, "Error: pvcreate failed\n"); cleanup_create(name, valid, valid_disks); return TV_ERR; }
        printf("  PV: %s\n", dp);
    }

    printf("Step 4: Creating volume group...\n");
    { char vn[128]; snprintf(vn, sizeof(vn), "tv_vg_%s", name); int ac = 0; char *args[16]; args[ac++] = "vgcreate"; args[ac++] = "--config"; args[ac++] = "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}"; args[ac++] = "-f"; args[ac++] = vn;
      for (int i = 0; i < valid_disks; i++) { char target[64]; make_target(target, sizeof(target), valid[i].disk); char dp[128]; snprintf(dp, sizeof(dp), "/dev/mapper/%s", target); args[ac] = strdup(dp); if (!args[ac]) { for (int j = 5; j < ac; j++) free(args[j]); return TV_ERR; } ac++; }
      args[ac] = NULL; int ret = tv_exec_run("vgcreate", args); for (int i = 5; i < ac; i++) free(args[i]); if (ret != 0) { fprintf(stderr, "Error: vgcreate failed\n"); cleanup_create(name, valid, valid_disks); return TV_ERR; } }
    printf("  VG: tv_vg_%s\n", name);

    printf("Step 5: Creating striped logical volume...\n");
    { char vn[128], ln[128], ss[16], st2[16]; snprintf(vn, sizeof(vn), "tv_vg_%s", name); snprintf(ln, sizeof(ln), "tv_lv_%s", name); snprintf(ss, sizeof(ss), "%d", valid_disks); snprintf(st2, sizeof(st2), "%dk", stripe_size_kb);
      char fa[] = "100%FREE"; char *la[] = {"lvcreate", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-l", fa, "-i", ss, "-I", st2, "-n", ln, vn, NULL};
      if (tv_exec_run("lvcreate", la) != 0) { fprintf(stderr, "Error: lvcreate failed\n"); cleanup_create(name, valid, valid_disks); return TV_ERR; } }
    printf("  LV: /dev/mapper/tv_vg_%s-tv_lv_%s\n", name, name);

    char lv_path[256]; snprintf(lv_path, sizeof(lv_path), "/dev/mapper/tv_vg_%s-tv_lv_%s", name, name);

    if (strcmp(fs, "none") != 0) {
        printf("Step 6: Formatting as %s...\n", fs);
        char mkfs_name[64]; snprintf(mkfs_name, sizeof(mkfs_name), "mkfs.%s", fs);
        char *ma[] = {mkfs_name, lv_path, NULL};
        if (tv_exec_run(mkfs_name, ma) != 0) { fprintf(stderr, "Error: mkfs failed\n"); cleanup_create(name, valid, valid_disks); return TV_ERR; }
    } else { printf("Step 6: Skipped formatting\n"); }

    if (mount_point && strcmp(fs, "none") != 0) {
        printf("Step 7: Mounting...\n");
        char *mka[] = {"sudo", "mkdir", "-p", mount_point, NULL}; (void)tv_exec_sudo(mka, 0);
        char *mta[] = {"sudo", "mount", lv_path, mount_point, NULL};
        if (tv_exec_sudo(mta, 0) != 0) { fprintf(stderr, "Error: mount failed\n"); cleanup_create(name, valid, valid_disks); return TV_ERR; }
        printf("  Mounted at %s\n", mount_point);
    }

    printf("\n=== Complete! ===\nDevice: %s\nSize: %lldGB\n", lv_path, total_gb);
    return TV_OK;
}
