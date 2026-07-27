#ifndef TIEREDVOL_DISCOVER_H
#define TIEREDVOL_DISCOVER_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    char disk[32];
    char model[128];
    char tran[16];
    long long size_gb;
    double speed_write;
    double speed_read;
    long long carve_gb;
    int is_root;
    int is_mounted;
} disk_t;

long long sysfs_size_gb(const char *disk);
void sysfs_model(const char *disk, char *out, size_t len);
int load_all_disk_info(disk_t *out, int max);
void find_mount_for_disk(const char *disk, char *mp, size_t mp_size);
int cmd_list(void);

#endif
