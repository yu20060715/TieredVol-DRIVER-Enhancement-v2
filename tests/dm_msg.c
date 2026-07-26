#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/dm-ioctl.h>
#include <errno.h>

#define DM_VERSION_IOWR _IOWR(0xfd, 0, struct dm_ioctl)
#define DM_TARGET_MSG_IOWR _IOWR(0xfd, 8, struct dm_ioctl)

static int dm_message(const char *target, const char *msg, char *result, size_t result_len) {
    int fd = open("/dev/mapper/control", O_RDWR);
    if (fd < 0) { perror("open /dev/mapper/control"); return -1; }

    /* First: DM_VERSION to negotiate version */
    size_t vlen = sizeof(struct dm_ioctl) + 128;
    char *vbuf = calloc(1, vlen);
    struct dm_ioctl *vhdr = (struct dm_ioctl *)vbuf;
    vhdr->version[0] = 4;
    vhdr->version[1] = 0;
    vhdr->version[2] = 0;
    vhdr->data_size = vlen;
    if (ioctl(fd, DM_VERSION_IOWR, vbuf) < 0) {
        perror("ioctl DM_VERSION");
        free(vbuf);
        close(fd);
        return -1;
    }
    unsigned int drv_ver = vhdr->version[1];
    free(vbuf);

    /* Second: DM_TARGET_MSG */
    size_t tlen = strlen(target) + 1;
    size_t mlen = strlen(msg) + 1;
    size_t total = sizeof(struct dm_ioctl) + tlen + mlen;
    char *buf = calloc(1, total);
    struct dm_ioctl *hdr = (struct dm_ioctl *)buf;

    hdr->version[0] = 4;
    hdr->version[1] = drv_ver;
    hdr->version[2] = 0;
    hdr->data_size = total;
    hdr->data_start = sizeof(struct dm_ioctl);

    memcpy(buf + sizeof(struct dm_ioctl), target, tlen);
    memcpy(buf + sizeof(struct dm_ioctl) + tlen, msg, mlen);

    if (ioctl(fd, DM_TARGET_MSG_IOWR, buf) < 0) {
        fprintf(stderr, "ioctl DM_TARGET_MSG: %s\n", strerror(errno));
        free(buf);
        close(fd);
        return -1;
    }

    hdr = (struct dm_ioctl *)buf;
    if (hdr->data_size > sizeof(struct dm_ioctl)) {
        char *data = buf + hdr->data_start;
        size_t dlen = hdr->data_size - sizeof(struct dm_ioctl);
        if (dlen >= result_len) dlen = result_len - 1;
        memcpy(result, data, dlen);
        result[dlen] = '\0';
    } else {
        result[0] = '\0';
    }

    free(buf);
    close(fd);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <target> <msg> [args...]\n", argv[0]);
        return 1;
    }

    char msg[4096] = "";
    for (int i = 2; i < argc; i++) {
        if (i > 2) strcat(msg, " ");
        strcat(msg, argv[i]);
    }

    char result[4096] = "";
    if (dm_message(argv[1], msg, result, sizeof(result)) == 0) {
        if (result[0])
            printf("%s\n", result);
        return 0;
    }
    return 1;
}
