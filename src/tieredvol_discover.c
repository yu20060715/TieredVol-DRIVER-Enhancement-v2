#include <stdio.h>
#include <string.h>
#include "tieredvol_common.h"
#include "tieredvol_types.h"
#include "tieredvol_discover.h"

long long sysfs_size_gb(const char *disk) {
    char path[256];
    int n = snprintf(path, sizeof(path), "/sys/block/%s/size", disk);
    if (n < 0 || n >= (int)sizeof(path)) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    long long sec = 0;
    if (fscanf(f, "%lld", &sec) != 1) sec = 0;
    fclose(f);
    return sec * 512 / (1024LL * 1024 * 1024);
}

void sysfs_model(const char *disk, char *out, size_t len) {
    char path[256];
    int n = snprintf(path, sizeof(path), "/sys/block/%s/device/model", disk);
    if (n < 0 || n >= (int)sizeof(path)) { strncpy(out, disk, len - 1); out[len - 1] = 0; return; }
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(out, len, f)) {
            out[strcspn(out, "\n")] = 0;
            if (out[0] == 0) { strncpy(out, disk, len - 1); out[len - 1] = 0; fclose(f); return; }
            char *p = out + strlen(out) - 1;
            while (p > out && *p == ' ') *p-- = 0;
        }
        fclose(f);
    } else {
        strncpy(out, disk, len - 1);
        out[len - 1] = 0;
    }
}

static int is_virtual_disk(const char *name) {
    return strncmp(name, "loop", 4) == 0 || strncmp(name, "ram", 3) == 0 || strncmp(name, "dm-", 3) == 0;
}

int load_all_disk_info(disk_t *out, int max) {
    int n = 0;
    FILE *p = popen("lsblk -d -o NAME,TRAN,MOUNTPOINT 2>/dev/null", "r");
    if (p) {
        char line[256];
        if (!fgets(line, sizeof(line), p)) { pclose(p); return n; }
        while (fgets(line, sizeof(line), p) && n < max) {
            line[strcspn(line, "\n")] = 0;
            char *name = strtok(line, " ");
            char *tran = strtok(NULL, " ");
            char *mnt = strtok(NULL, " ");
            if (!name) continue;
            if (is_virtual_disk(name)) continue;
            strncpy(out[n].disk, name, sizeof(out[n].disk) - 1);
            out[n].disk[sizeof(out[n].disk) - 1] = 0;
            strncpy(out[n].tran, tran ? tran : "unknown", sizeof(out[n].tran) - 1);
            out[n].tran[sizeof(out[n].tran) - 1] = 0;
            out[n].is_root = 0;
            out[n].is_mounted = 0;
            if (mnt && strcmp(mnt, "/") == 0)
                out[n].is_root = 1;
            else if (mnt && mnt[0] != 0)
                out[n].is_mounted = 1;
            n++;
        }
        pclose(p);
    }
    return n;
}

void find_mount_for_disk(const char *disk, char *mp, size_t mp_size) {
    mp[0] = 0;
    char dev_pattern[64];
    snprintf(dev_pattern, sizeof(dev_pattern), "/dev/%s ", disk);
    FILE *fp = fopen("/proc/mounts", "r");
    if (fp) {
        char mnt_line[512];
        while (fgets(mnt_line, sizeof(mnt_line), fp)) {
            if (strncmp(mnt_line, dev_pattern, strlen(dev_pattern)) == 0) {
                char fmt[32];
                snprintf(fmt, sizeof(fmt), "%%*s %%%zus", mp_size - 1);
                sscanf(mnt_line, fmt, mp);
                break;
            }
        }
        fclose(fp);
    }
}

int cmd_list(void) {
    disk_t info[TV_MAX_DISKS];
    int ninfo = load_all_disk_info(info, TV_MAX_DISKS);

    printf("%-12s %-8s %-28s %-12s %-8s %-8s\n",
           "DEVICE", "TYPE", "MODEL", "TRAN", "SIZE", "SPEED");
    printf("%-12s %-8s %-28s %-12s %-8s %-8s\n",
           "------------", "--------", "----------------------------", "------------", "--------", "--------");

    for (int i = 0; i < ninfo; i++) {
        char model[128];
        sysfs_model(info[i].disk, model, sizeof(model));
        long long gb = sysfs_size_gb(info[i].disk);

        char size_str[32];
        if (gb >= 1024) snprintf(size_str, sizeof(size_str), "%.1fT", gb / 1024.0);
        else snprintf(size_str, sizeof(size_str), "%lldG", gb);

        printf("%-12s %-8s %-28s %-12s %-8s %-8s%s%s\n",
               info[i].disk, "disk", model, info[i].tran, size_str, "-",
               info[i].is_root ? " [ROOT]" : "",
               info[i].is_mounted ? " [MOUNTED]" : "");
    }
    printf("\n[ROOT] = System disk, cannot be carved with dm-linear\n");
    printf("[MOUNTED] = Mounted partition, cannot be carved with dm-linear\n");
    return TV_OK;
}
