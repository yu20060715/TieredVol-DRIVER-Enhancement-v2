#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <math.h>
#include <limits.h>
#include <errno.h>
#include <linux/fs.h>
#include "tieredvol_common.h"
#include "tieredvol_types.h"
#include "tieredvol_discover.h"
#include "tieredvol_bench.h"
#include "tieredvol_exec.h"
#include "tieredvol_warmup.h"

static volatile sig_atomic_t bench_interrupted = 0;

static void bench_signal_handler(int sig) {
    (void)sig;
    bench_interrupted = 1;
}

#define BENCH_FILE_ITERATIONS   5
#define BENCH_FILE_SIZE          (512LL * 1024 * 1024)
#define BENCH_FILE_BLOCK         (1024 * 1024)

typedef struct { int ret; double w; double r; } bench_result_t;

static int bench_disk(const char *disk, double *write_spd, double *read_spd, int warmup) {
    *write_spd = *read_spd = 0;
    char mp[512] = {0};
    int need_unmount = 0;
    int result = -1;
    double *ws = NULL;
    double *rs = NULL;
    char cleanup_path[600] = "";

    struct sigaction sa_new, sa_old_int, sa_old_term;
    memset(&sa_new, 0, sizeof(sa_new));
    sa_new.sa_handler = bench_signal_handler;
    sigemptyset(&sa_new.sa_mask);
    sa_new.sa_flags = 0;
    sigaction(SIGINT, &sa_new, &sa_old_int);
    sigaction(SIGTERM, &sa_new, &sa_old_term);
    bench_interrupted = 0;

    find_mount_for_disk(disk, mp, sizeof(mp));

    if (mp[0] == 0) {
        fprintf(stderr, "    /dev/%s is not mounted, attempting to mount...\n", disk);
        snprintf(mp, sizeof(mp), "/tmp/db_%s", disk);
        mkdir(mp, 0755);
        char devpath_b[64];
        snprintf(devpath_b, sizeof(devpath_b), "/dev/%s", disk);
        char *mount_argv[] = {"mount", devpath_b, mp, NULL};
        int mret = tv_exec_sudo(mount_argv, 1);
        if (mret == 0) {
            char *chmod_argv[] = {"chmod", "0755", mp, NULL};
            (void)tv_exec_sudo(chmod_argv, 1);
        }
        if (mret != 0) {
            fprintf(stderr, "    mount failed (exit=%d), trying udisksctl...\n", mret);
            char *udisks_argv[] = {"udisksctl", "mount", "-b", devpath_b, NULL};
            char uout[512] = "";
            tv_exec_capture("udisksctl", udisks_argv, uout, sizeof(uout));
            char *at = strstr(uout, "at ");
            if (at) {
                at += 3;
                at[strcspn(at, "\n")] = 0;
                strncpy(mp, at, sizeof(mp) - 1);
                mp[sizeof(mp) - 1] = 0;
            }
        }
        if (mret != 0 && strncmp(mp, "/tmp/db_", 8) == 0) {
            rmdir(mp);
            mp[0] = 0;
            fprintf(stderr, "    /dev/%s has no filesystem, using raw device benchmark\n", disk);
            char devpath_raw[64];
            snprintf(devpath_raw, sizeof(devpath_raw), "/dev/%s", disk);
            uint64_t raw_speed = 0;
            if (tv_benchmark(devpath_raw, &raw_speed, warmup) == 0) {
                *write_spd = (double)raw_speed;
                *read_spd = (double)raw_speed;
                result = 0;
            }
            goto cleanup;
        }
        need_unmount = 1;
    }

    if (mp[0] == 0) {
        fprintf(stderr, "Warning: cannot mount /dev/%s for benchmark\n", disk);
        goto cleanup;
    }

    {
        int iterations = BENCH_FILE_ITERATIONS;
        long long file_size = BENCH_FILE_SIZE;
        int block_size = BENCH_FILE_BLOCK;
        int o_direct_warned = 0;

        ws = calloc(iterations, sizeof(double));
        rs = calloc(iterations, sizeof(double));
        if (!ws || !rs) goto cleanup;

        if (warmup) {
            char warmup_path[PATH_MAX];
            snprintf(warmup_path, sizeof(warmup_path), "%s/.bench_warmup_%s", mp, disk);
            tv_warmup_device(warmup_path, 2LL * 1024 * 1024 * 1024);
            unlink(warmup_path);
        }

        int completed = 0;
        for (int iter = 0; iter < iterations; iter++) {
            if (bench_interrupted) break;

            char testpath[PATH_MAX];
            snprintf(testpath, sizeof(testpath), "%s/.bench_%s", mp, disk);
            snprintf(cleanup_path, sizeof(cleanup_path), "%s", testpath);

            int fd = open(testpath, O_RDWR | O_CREAT | O_DIRECT, 0644);
            if (fd < 0) {
                if (!o_direct_warned) {
                    fprintf(stderr, "    WARNING: O_DIRECT not supported, using buffered I/O (speeds may be inflated)\n");
                    o_direct_warned = 1;
                }
                fd = open(testpath, O_RDWR | O_CREAT, 0644);
            }
            if (fd < 0) { fprintf(stderr, "    [debug] open(%s) failed\n", testpath); continue; }
            if (ftruncate(fd, 0) != 0) { close(fd); continue; }
            void *buf = NULL;
            if (posix_memalign(&buf, 4096, block_size) != 0 || !buf) { close(fd); continue; }
            memset(buf, 'A', block_size);

            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            long long written = 0;
            while (written < file_size) {
                ssize_t n = pwrite(fd, buf, block_size, written);
                if (n <= 0) break;
                written += n;
            }
            fdatasync(fd);

            char devpath[64];
            snprintf(devpath, sizeof(devpath), "/dev/%s", disk);
            int fd_disk = open(devpath, O_RDONLY);
            if (fd_disk >= 0) { ioctl(fd_disk, BLKFLSBUF, NULL); close(fd_disk); }

            clock_gettime(CLOCK_MONOTONIC, &t1);
            double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
            if (elapsed <= 0.0) elapsed = 0.001;
            ws[completed] = (double)written / (1024.0 * 1024.0) / elapsed;

            sync();
            int dcfd = open("/proc/sys/vm/drop_caches", O_WRONLY);
            if (dcfd >= 0) { (void)!write(dcfd, "3", 1); close(dcfd); }

            lseek(fd, 0, SEEK_SET);
            clock_gettime(CLOCK_MONOTONIC, &t0);
            long long readtotal = 0;
            while (readtotal < file_size) {
                ssize_t n = pread(fd, buf, block_size, readtotal);
                if (n <= 0) break;
                readtotal += n;
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
            if (elapsed <= 0.0) elapsed = 0.001;
            rs[completed] = (double)readtotal / (1024.0 * 1024.0) / elapsed;
            completed++;

            free(buf);
            close(fd);
            unlink(testpath);
            cleanup_path[0] = 0;
        }

        for (int i = 0; i < completed; i++) {
            if (isnan(ws[i])) ws[i] = 0;
            if (isnan(rs[i])) rs[i] = 0;
        }

        if (completed >= 3) {
            for (int i = 0; i < completed - 1; i++)
                for (int j = i + 1; j < completed; j++)
                    if (ws[j] > ws[i]) { double t = ws[i]; ws[i] = ws[j]; ws[j] = t; }
            for (int i = 0; i < completed - 1; i++)
                for (int j = i + 1; j < completed; j++)
                    if (rs[j] > rs[i]) { double t = rs[i]; rs[i] = rs[j]; rs[j] = t; }
            double sw = 0, sr = 0;
            int cnt = 0;
            for (int i = 1; i < completed - 1; i++) { sw += ws[i]; sr += rs[i]; cnt++; }
            if (cnt > 0) { *write_spd = sw / cnt; *read_spd = sr / cnt; }
        } else if (completed > 0) {
            double sw = 0, sr = 0;
            for (int i = 0; i < completed; i++) { sw += ws[i]; sr += rs[i]; }
            *write_spd = sw / completed;
            *read_spd = sr / completed;
        }
    }

    if (need_unmount && mp[0]) {
        char *umount_argv[] = {"umount", mp, NULL};
        (void)tv_exec_sudo(umount_argv, 0);
        rmdir(mp);
        mp[0] = 0;
    }

    result = (*write_spd > 0 || *read_spd > 0) ? 0 : -1;

cleanup:
    if (cleanup_path[0]) unlink(cleanup_path);
    if (need_unmount && mp[0] && result != 0) {
        char *umount_argv[] = {"umount", mp, NULL};
        (void)tv_exec_sudo(umount_argv, 0);
        rmdir(mp);
    }
    sigaction(SIGINT, &sa_old_int, NULL);
    sigaction(SIGTERM, &sa_old_term, NULL);
    free(ws);
    free(rs);
    return result;
}

static double cmp_score(const disk_t *d) {
    return d->speed_write * 0.6 + d->speed_read * 0.4;
}

int cmp_speed(const void *a, const void *b) {
    double sa = cmp_score((const disk_t *)a);
    double sb = cmp_score((const disk_t *)b);
    if (isnan(sa) && isnan(sb)) return 0;
    if (isnan(sa)) return 1;
    if (isnan(sb)) return TV_ERR;
    return (sb > sa) - (sb < sa);
}

typedef struct { pid_t pid; int pipe_fd; int idx; } bench_child_t;

int run_parallel_bench(disk_t *disks, int ndisks, int warmup,
    bench_interrupt_fn on_interrupt, void *interrupt_ctx) {
    bench_child_t children[TV_MAX_DISKS];
    int nchildren = 0;
    for (int i = 0; i < ndisks; i++) {
        int pipefd[2];
        if (pipe(pipefd) < 0) { fprintf(stderr, "pipe failed\n"); continue; }
        pid_t pid = fork();
        if (pid < 0) { close(pipefd[0]); close(pipefd[1]); continue; }
        if (pid == 0) {
            close(pipefd[0]);
            double w = 0, r = 0;
            int ret = bench_disk(disks[i].disk, &w, &r, warmup);
            bench_result_t result = { ret, w, r };
            const char *wp = (const char *)&result;
            size_t remaining = sizeof(result);
            while (remaining > 0) {
                ssize_t n = write(pipefd[1], wp, remaining);
                if (n <= 0) break;
                wp += n;
                remaining -= n;
            }
            close(pipefd[1]);
            _exit(0);
        }
        close(pipefd[1]);
        children[nchildren].pid = pid;
        children[nchildren].pipe_fd = pipefd[0];
        children[nchildren].idx = i;
        nchildren++;
        printf("  Started benchmark for /dev/%s\n", disks[i].disk);
    }
    int done = 0;
    while (done < nchildren) {
        if (bench_interrupted) {
            for (int c = 0; c < nchildren; c++) {
                if (children[c].pid > 0) kill(children[c].pid, SIGTERM);
            }
            for (int c = 0; c < nchildren; c++) {
                if (children[c].pid > 0) {
                    waitpid(children[c].pid, NULL, 0);
                    close(children[c].pipe_fd);
                }
            }
            fprintf(stderr, "\nBenchmark interrupted.\n");
            if (on_interrupt) on_interrupt(interrupt_ctx);
            return TV_ERR;
        }
        int status;
        for (int c = 0; c < nchildren; c++) {
            if (children[c].pid <= 0) continue;
            pid_t ret = waitpid(children[c].pid, &status, WNOHANG);
            if (ret > 0) {
                bench_result_t result = { -1, 0, 0 };
                ssize_t nread = read(children[c].pipe_fd, &result, sizeof(result));
                close(children[c].pipe_fd);
                int idx = children[c].idx;
                if (nread == sizeof(result)) {
                    disks[idx].speed_write = result.w;
                    disks[idx].speed_read = result.r;
                    if (result.ret == 0)
                        printf("  /dev/%s: Write %.0f MB/s  Read %.0f MB/s\n",
                               disks[idx].disk, result.w, result.r);
                    else
                        printf("  /dev/%s: FAILED\n", disks[idx].disk);
                } else {
                    printf("  /dev/%s: pipe read error\n", disks[idx].disk);
                }
                children[c].pid = 0;
                done++;
            }
        }
        if (done < nchildren) usleep(100000);
    }
    return TV_OK;
}

int cmd_bench(int argc, char *argv[]) {
    char *disk_list = NULL;
    int sequential = 0, warmup = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--disks") == 0 && i + 1 < argc) {
            disk_list = argv[++i];
        } else if (strcmp(argv[i], "--sequential") == 0) {
            sequential = 1;
        } else if (strcmp(argv[i], "--warmup") == 0) {
            warmup = 1;
        }
    }

    if (!disk_list) {
        fprintf(stderr, "Usage: tiered_setup --bench --disks sda,sdb,sdc [--sequential] [--warmup]\n");
        return TV_ERR;
    }

    char disks[TV_MAX_DISKS][32];
    int nd = 0;
    char buf[1024];
    strncpy(buf, disk_list, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    char *tok = strtok(buf, ",");
    while (tok && nd < TV_MAX_DISKS) {
        if (!tiered_is_valid_name(tok)) {
            fprintf(stderr, "Error: invalid disk name '%s'\n", tok);
            return TV_ERR;
        }
        strncpy(disks[nd], tok, 31);
        disks[nd][31] = 0;
        nd++;
        tok = strtok(NULL, ",");
    }

    if (nd == 0) {
        fprintf(stderr, "Error: no valid disks\n");
        return TV_ERR;
    }

    disk_t info[TV_MAX_DISKS];
    for (int i = 0; i < nd; i++) {
        memset(&info[i], 0, sizeof(disk_t));
        strncpy(info[i].disk, disks[i], 31);
        sysfs_model(disks[i], info[i].model, sizeof(info[i].model));
        info[i].size_gb = sysfs_size_gb(disks[i]);
    }

    disk_t dinfo[TV_MAX_DISKS];
    int ninfo = load_all_disk_info(dinfo, TV_MAX_DISKS);
    for (int i = 0; i < nd; i++) {
        for (int j = 0; j < ninfo; j++) {
            if (strcmp(disks[i], dinfo[j].disk) == 0) {
                strncpy(info[i].tran, dinfo[j].tran, sizeof(info[i].tran) - 1);
                break;
            }
        }
    }

    if (sequential || nd <= 1) {
        printf("Benchmarking %d disk(s) sequentially...\n", nd);
        for (int i = 0; i < nd; i++) {
            printf("  Testing /dev/%s ... ", info[i].disk);
            fflush(stdout);
            if (bench_disk(info[i].disk, &info[i].speed_write, &info[i].speed_read, warmup) == 0) {
                printf("Write: %.0f MB/s  Read: %.0f MB/s\n", info[i].speed_write, info[i].speed_read);
            } else {
                printf("FAILED\n");
            }
        }
    } else {
        printf("Benchmarking %d disks in parallel...\n", nd);

        if (run_parallel_bench(info, nd, warmup, NULL, NULL) != 0)
            return TV_ERR;
    }

    double total_w = 0;
    printf("\n%-28s %-12s %-8s %-8s %-8s\n", "ID", "Type", "Size", "Write", "Read");
    printf("%-28s %-12s %-8s %-8s %-8s\n", "----------------------------", "------------", "--------", "--------", "--------");
    for (int i = 0; i < nd; i++) {
        char id[256];
        snprintf(id, sizeof(id), "%s(%s)", info[i].model, info[i].disk);

        char size_str[32];
        if (info[i].size_gb >= 1024) snprintf(size_str, sizeof(size_str), "%.1fT", info[i].size_gb / 1024.0);
        else snprintf(size_str, sizeof(size_str), "%lldG", info[i].size_gb);

        char w[16] = "-", r[16] = "-";
        if (info[i].speed_write > 0) { snprintf(w, sizeof(w), "%.0f", info[i].speed_write); total_w += info[i].speed_write; }
        if (info[i].speed_read > 0) snprintf(r, sizeof(r), "%.0f", info[i].speed_read);
        printf("%-28s %-12s %-8s %-8s %-8s\n", id, info[i].tran, size_str, w, r);
    }
    printf("\nTotal theoretical write speed: %.0f MB/s\n", total_w);

    return TV_OK;
}
