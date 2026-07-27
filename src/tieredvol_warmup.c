#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "tieredvol_warmup.h"
#include "tieredvol_types.h"

int tv_warmup_device(const char *path, uint64_t target_bytes) {
    int fd = open(path, O_WRONLY | O_DIRECT);
    if (fd < 0) {
        fprintf(stderr, "Error: cannot open %s for warmup: %s\n", path, strerror(errno));
        return TV_ERR;
    }
    uint64_t block = 1024 * 1024;
    uint8_t *buf = NULL;
    if (posix_memalign((void **)&buf, 512, (size_t)block) != 0) {
        fprintf(stderr, "Error: cannot allocate warmup buffer\n");
        close(fd);
        return TV_ERR;
    }
    memset(buf, 0xAB, (size_t)block);
    fprintf(stderr, "  Warming up %s (%luMB)...\n", path,
            (unsigned long)(target_bytes / (1024 * 1024)));
    uint64_t written = 0;
    while (written < target_bytes) {
        if (g_shutdown_requested) { free(buf); close(fd); return TV_ERR; }
        uint64_t chunk = block;
        if (written + chunk > target_bytes) chunk = target_bytes - written;
        ssize_t n = pwrite(fd, buf, (size_t)chunk, (off_t)written);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "Error: warmup pwrite failed on %s: %s\n", path, strerror(errno));
            free(buf); close(fd);
            return TV_ERR;
        }
        written += (uint64_t)n;
    }
    fsync(fd);
    fprintf(stderr, "  Warm-up complete.\n");
    free(buf);
    close(fd);
    return TV_OK;
}
