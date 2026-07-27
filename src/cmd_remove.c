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
#include "cmd_remove.h"
#include "cmd_status.h"

int cmd_remove(int argc, char *argv[]) {
    char *name = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) name = argv[++i];
    }

    if (!name) {
        fprintf(stderr, "Usage: tiered_setup --remove --name NAME\n");
        return TV_ERR;
    }

    if (!tiered_is_valid_name(name)) {
        fprintf(stderr, "Error: invalid name '%s'\n", name);
        return TV_ERR;
    }

    printf("=== TieredVol: Removing '%s' ===\n", name);

    if (is_kernel_target(name)) {
        printf("  Detected kernel dm target (tieredvol)\n");

        printf("  Removing dm target...\n");
        {
            char *dm_argv[] = {"sudo", "dmsetup", "remove", name, NULL};
            int ret = tv_exec_sudo(dm_argv, 0);
            if (ret != 0) {
                fprintf(stderr, "  dmsetup remove failed (retrying)...\n");
                sleep(1);
                ret = tv_exec_sudo(dm_argv, 0);
            }
            if (ret == 0)
                printf("  Removed /dev/mapper/%s\n", name);
            else
                fprintf(stderr, "  Failed to remove /dev/mapper/%s\n", name);
        }

        printf("  Removing metadata...\n");
        {
            char conf_path[256];
            snprintf(conf_path, sizeof(conf_path), TV_CONFIG_DIR "%s.conf", name);
            char *rm_argv[] = {"sudo", "rm", "-f", conf_path, NULL};
            (void)tv_exec_sudo(rm_argv, 0);
        }
        {
            char sched_path[256];
            snprintf(sched_path, sizeof(sched_path), TV_CONFIG_DIR "%s.scheduler", name);
            char *rm_argv[] = {"sudo", "rm", "-f", sched_path, NULL};
            (void)tv_exec_sudo(rm_argv, 0);
        }
        {
            char *rmdir_argv[] = {"sudo", "rmdir", TV_CONFIG_DIR, NULL};
            (void)tv_exec_sudo(rmdir_argv, 0);
        }

        printf("\n=== Remove Complete ===\n");
        return TV_OK;
    }

    /* Check for old-style scheduler volume */
    char sched_path[256];
    snprintf(sched_path, sizeof(sched_path), TV_CONFIG_DIR "%s.scheduler", name);
    FILE *sf = fopen(sched_path, "r");
    if (sf) {
        fclose(sf);
        printf("  Detected legacy weighted I/O scheduler volume\n");

        char targets[TV_MAX_DISKS][64];
        int ntargets = 0;

        TV_METADATA sched_meta;
        memset(&sched_meta, 0, sizeof(sched_meta));
        if (tv_metadata_load(&sched_meta, sched_path) == 0) {
            for (uint32_t i = 0; i < sched_meta.disk_count && ntargets < TV_MAX_DISKS; i++) {
                char short_disk[64];
                const char *dn = sched_meta.disk_names[i];
                const char *slash = strrchr(dn, '/');
                strncpy(short_disk, slash ? slash + 1 : dn, 63);
                short_disk[63] = 0;
                make_target(targets[ntargets], sizeof(targets[0]), short_disk);
                ntargets++;
            }
        }

        if (ntargets == 0) {
            FILE *p = popen("sudo dmsetup ls 2>/dev/null", "r");
            if (p) {
                char line[256];
                while (fgets(line, sizeof(line), p) && ntargets < TV_MAX_DISKS) {
                    line[strcspn(line, "\n")] = 0;
                    char *tok = strtok(line, "\t ");
                    if (!tok) continue;
                    if (strncmp(tok, "tv_", 3) == 0 && strstr(tok, "_carve")) {
                        snprintf(targets[ntargets], sizeof(targets[0]), "%s", tok);
                        ntargets++;
                    }
                }
                pclose(p);
            }
        }
        for (int i = 0; i < ntargets; i++) {
            int removed = 0;
            for (int retry = 0; retry < 3; retry++) {
                char *const dm_argv[] = {"sudo", "dmsetup", "remove", targets[i], NULL};
                if (tv_exec_sudo(dm_argv, 0) == 0) { removed = 1; break; }
                if (retry < 2) { fprintf(stderr, "  %s busy, retrying...\n", targets[i]); sleep(1); }
            }
            if (removed) printf("  Removed %s\n", targets[i]);
            else fprintf(stderr, "  Failed to remove %s\n", targets[i]);
        }

        { char *rm_argv[] = {"sudo", "rm", "-f", sched_path, NULL}; (void)tv_exec_sudo(rm_argv, 0); }
        { char conf_path_cleanup[256]; snprintf(conf_path_cleanup, sizeof(conf_path_cleanup), TV_CONFIG_DIR "%s.conf", name); char *rm_argv[] = {"sudo", "rm", "-f", conf_path_cleanup, NULL}; (void)tv_exec_sudo(rm_argv, 0); }
        { char *rmdir_argv[] = {"sudo", "rmdir", TV_CONFIG_DIR, NULL}; (void)tv_exec_sudo(rmdir_argv, 0); }

        printf("\n=== Remove Complete ===\n");
        return TV_OK;
    }

    /* LVM volume path */
    char targets[TV_MAX_DISKS][64];
    int ntargets = 0;

    char conf_path[256];
    snprintf(conf_path, sizeof(conf_path), TV_CONFIG_DIR "%s.conf", name);
    FILE *cf = fopen(conf_path, "r");
    if (cf) {
        char line[256];
        while (fgets(line, sizeof(line), cf) && ntargets < TV_MAX_DISKS) {
            if (strncmp(line, "device=", 7) == 0) {
                char disk[32];
                sscanf(line + 7, "%31s", disk);
                make_target(targets[ntargets], sizeof(targets[0]), disk);
                ntargets++;
            }
        }
        fclose(cf);
        if (ntargets == 0) { fprintf(stderr, "Error: no devices in config\n"); return TV_ERR; }
    } else {
        fprintf(stderr, "Error: no config for '%s'\n", name);
        return TV_ERR;
    }

    printf("Checking mounts...\n");
    char lv_path[256];
    snprintf(lv_path, sizeof(lv_path), "/dev/mapper/tv_vg_%s-tv_lv_%s", name, name);
    { struct stat st; if (stat(lv_path, &st) == 0) { char mp[512]; find_mount_for_disk(lv_path, mp, sizeof(mp)); if (mp[0]) { char *ua[] = {"sudo", "umount", mp, NULL}; (void)tv_exec_sudo(ua, 0); printf("  Unmounted %s\n", mp); } } }

    printf("Removing LVM logical volume...\n");
    { char fl[256]; snprintf(fl, sizeof(fl), "tv_vg_%s/tv_lv_%s", name, name); char *la[] = {"lvremove", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-f", fl, NULL}; (void)tv_exec_run("lvremove", la); }

    printf("Removing volume group...\n");
    { char vn[128]; snprintf(vn, sizeof(vn), "tv_vg_%s", name); char *va[] = {"vgremove", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-f", vn, NULL}; (void)tv_exec_run("vgremove", va); }

    printf("Removing dm-linear carve targets...\n");
    for (int i = 0; i < ntargets; i++) {
        { char dp[128]; snprintf(dp, sizeof(dp), "/dev/mapper/%s", targets[i]); char *pa[] = {"pvremove", "--config", "devices{scan=[\"/dev/mapper\"] obtain_device_list_from_udev=0}", "-ff", "-y", dp, NULL}; (void)tv_exec_run("pvremove", pa); }
        { char *da[] = {"sudo", "dmsetup", "remove", targets[i], NULL}; (void)tv_exec_sudo(da, 0); }
    }

    printf("Removing config...\n");
    { char *ra[] = {"sudo", "rm", "-f", conf_path, NULL}; (void)tv_exec_sudo(ra, 0); char *rda[] = {"sudo", "rmdir", TV_CONFIG_DIR, NULL}; (void)tv_exec_sudo(rda, 0); }

    printf("\n=== Remove Complete ===\n");
    return TV_OK;
}
