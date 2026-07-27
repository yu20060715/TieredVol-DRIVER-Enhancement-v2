#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include "tiered_common.h"
#include "tiered_types.h"
#include "exec_helper.h"
#include "cmd_status.h"

int is_kernel_target(const char *name) {
    char path[256];
    snprintf(path, sizeof(path), "/dev/mapper/%s", name);
    struct stat st;
    if (stat(path, &st) != 0) return 0;

    char output[4096];
    char *dm_argv[] = {"dmsetup", "table", (char *)name, NULL};
    if (tv_exec_capture("dmsetup", dm_argv, output, sizeof(output)) != 0)
        return 0;
    return strstr(output, "tieredvol") != NULL;
}

int cmd_status(void) {
    printf("=== TieredVol Status ===\n\n");

    printf("DM Targets:\n");
    {
        DIR *d = opendir("/dev/mapper");
        if (d) {
            struct dirent *ent;
            int found = 0;
            while ((ent = readdir(d))) {
                if (ent->d_name[0] == '.') continue;
                if (strncmp(ent->d_name, "tv_", 3) == 0 ||
                    strncmp(ent->d_name, "fastpool", 8) == 0) {
                    printf("  /dev/mapper/%s", ent->d_name);
                    if (is_kernel_target(ent->d_name))
                        printf(" [tieredvol]");
                    printf("\n");
                    found = 1;
                }
            }
            closedir(d);
            if (!found) printf("  None\n");
        }
    }

    printf("\nKernel Module:\n");
    {
        FILE *f = fopen("/proc/modules", "r");
        if (f) {
            char line[256];
            int found = 0;
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "tieredvol ", 10) == 0) {
                    printf("  tieredvol loaded\n");
                    found = 1;
                    break;
                }
            }
            fclose(f);
            if (!found) printf("  tieredvol not loaded\n");
        }
    }

    printf("\nSaved Configs:\n");
    {
        DIR *d = opendir(TV_CONFIG_DIR);
        if (d) {
            struct dirent *ent;
            int found = 0;
            while ((ent = readdir(d))) {
                if (strstr(ent->d_name, ".conf") || strstr(ent->d_name, ".scheduler")) {
                    printf("  " TV_CONFIG_DIR "%s\n", ent->d_name);
                    found = 1;
                }
            }
            closedir(d);
            if (!found) printf("  None\n");
        } else {
            printf("  None\n");
        }
    }

    return TV_OK;
}
