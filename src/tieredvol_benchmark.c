#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include "tieredvol_types.h"
#include "tieredvol_warmup.h"

volatile sig_atomic_t g_shutdown_requested = 0;

#define BENCH_RAW_SIZE      (32 * 1024 * 1024)
#define BENCH_RAW_RUNS      3
#define BENCH_RAW_BLOCK     512
#define WARMUP_DEFAULT_BYTES (2LL * 1024 * 1024 * 1024)

int tv_benchmark(const char *disk_path, uint64_t *speed_out, int warmup) {
    if (!disk_path || !speed_out) return TV_ERR;
    *speed_out = 0;

    int fd = open(disk_path, O_RDWR | O_DIRECT);
    if (fd < 0) {
        fprintf(stderr, "benchmark: cannot open '%s': %s\n", disk_path, strerror(errno));
        return TV_ERR;
    }

    void *buf = NULL;
    if (posix_memalign(&buf, BENCH_RAW_BLOCK, BENCH_RAW_SIZE) != 0) {
        close(fd);
        return TV_ERR;
    }

    memset(buf, 0xAB, BENCH_RAW_SIZE);

    /* Write near the end of the device to avoid overwriting data at offset 0 */
    off_t dev_size = lseek(fd, 0, SEEK_END);
    if (dev_size < 0) {
        fprintf(stderr, "benchmark: cannot get device size for '%s': %s\n", disk_path, strerror(errno));
        free(buf);
        close(fd);
        return TV_ERR;
    }
    off_t bench_offset = 0;
    if (dev_size > (off_t)BENCH_RAW_SIZE * BENCH_RAW_RUNS + 1024 * 1024) {
        bench_offset = dev_size - (off_t)BENCH_RAW_SIZE * BENCH_RAW_RUNS - 1024 * 1024;
        /* Align to 512 bytes */
        bench_offset = bench_offset & ~((off_t)BENCH_RAW_BLOCK - 1);
    }

    /* SLC cache warm-up: write 2GB to exhaust SLC cache before benchmarking */
    if (warmup) {
        fprintf(stderr, "  Warming up SLC cache (2GB)...\n");
        tv_warmup_device(disk_path, (uint64_t)WARMUP_DEFAULT_BYTES);
        fprintf(stderr, "  Warm-up complete, starting benchmark...\n");
    }

    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    for (int run = 0; run < BENCH_RAW_RUNS; run++) {
        uint64_t written = 0;
        while (written < BENCH_RAW_SIZE) {
            ssize_t n = pwrite(fd, (char *)buf + written, BENCH_RAW_SIZE - written, bench_offset + written);
            if (n < 0) {
                if (errno == EINTR) continue;
                fprintf(stderr, "benchmark: write error on '%s': %s\n", disk_path, strerror(errno));
                free(buf);
                close(fd);
                return TV_ERR;
            }
            written += n;
        }
        fsync(fd);
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_end);

    free(buf);
    close(fd);

    double elapsed = (ts_end.tv_sec - ts_start.tv_sec) +
                     (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;
    if (elapsed <= 0) elapsed = 0.001;

    uint64_t total = (uint64_t)BENCH_RAW_SIZE * BENCH_RAW_RUNS;
    *speed_out = (uint64_t)((double)total / elapsed / (1024.0 * 1024.0));

    return TV_OK;
}
