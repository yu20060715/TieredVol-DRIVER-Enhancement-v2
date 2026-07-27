#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "tieredvol_common.h"
#include "tieredvol_types.h"
#include "version.h"
#include "tieredvol_discover.h"
#include "tieredvol_bench.h"
#include "tieredvol_exec.h"
#include "cmd_create.h"
#include "cmd_remove.h"
#include "cmd_status.h"

static void print_usage(const char *prog) {
    printf("TieredVol — Tiered Storage Volume Manager v%s\n\n", VERSION);
    printf("Usage:\n");
    printf("  %s --list                              List all disks\n", prog);
    printf("  %s --bench --disks sda,sdb,sdc         Benchmark disks (parallel)\n", prog);
    printf("  %s --bench --disks sda,sdb --sequential Benchmark disks (sequential)\n", prog);
    printf("  %s --bench --disks sda,sdb --warmup    Benchmark with SLC cache warm-up\n", prog);
    printf("  %s --create --name NAME --disks ...    Create tiered volume\n", prog);
    printf("  %s --create --name NAME --disks ... --scheduler  Create with weighted I/O scheduler\n", prog);
    printf("  %s --remove --name NAME                Remove tiered volume\n", prog);
    printf("  %s --destroy --name NAME               Remove tiered volume\n", prog);
    printf("  %s --status                            Show status\n", prog);
    printf("  %s --version                           Show version\n", prog);
    printf("  %s --help / -h                        Show this help\n", prog);
    printf("\nExamples:\n");
    printf("  sudo %s --create --name fastpool --disks sdb:300,sdc:200 --fs ext4 --mount /mnt/fast\n", prog);
    printf("  sudo %s --remove --name fastpool\n", prog);
}

static int check_deps(void) {
    const char *deps[] = {"dmsetup", "vgcreate", "pvcreate", NULL};
    for (int i = 0; deps[i]; i++) {
        char path_buf[256];
        const char *path_env = getenv("PATH");
        if (!path_env) path_env = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
        int found = 0;
        char *path_copy = strdup(path_env);
        if (path_copy) {
            char *saveptr = NULL;
            char *dir = strtok_r(path_copy, ":", &saveptr);
            while (dir) {
                snprintf(path_buf, sizeof(path_buf), "%s/%s", dir, deps[i]);
                if (access(path_buf, X_OK) == 0) { found = 1; break; }
                dir = strtok_r(NULL, ":", &saveptr);
            }
            free(path_copy);
        }
        if (!found) {
            fprintf(stderr, "Error: '%s' not found. Install lvm2: apt install lvm2\n", deps[i]);
            return 1;
        }
    }
    return 0;
}

int main(int argc, char *argv[]) {
    if (geteuid() != 0) {
        fprintf(stderr, "Error: tiered_setup requires root privileges.\n");
        fprintf(stderr, "Please run: sudo %s\n", argv[0]);
        return 1;
    }

    if (check_deps() != 0) return 1;

    if (argc < 2) {
        print_usage(argv[0]);
        return 0;
    }

    if (strcmp(argv[1], "--list") == 0) return cmd_list();
    if (strcmp(argv[1], "--bench") == 0) return cmd_bench(argc, argv);
    if (strcmp(argv[1], "--create") == 0) return cmd_create(argc, argv);
    if (strcmp(argv[1], "--remove") == 0 || strcmp(argv[1], "--destroy") == 0) return cmd_remove(argc, argv);
    if (strcmp(argv[1], "--status") == 0) return cmd_status();
    if (strcmp(argv[1], "--version") == 0) { printf("TieredVol %s\n", VERSION); return 0; }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    fprintf(stderr, "Unknown command: %s\n", argv[1]);
    return 1;
}