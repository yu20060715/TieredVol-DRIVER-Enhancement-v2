#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "tieredvol_types.h"

/* CRC32C (Castagnoli) — matches kernel crc32c */
static uint32_t crc32c_table[256];
static int crc32c_initialized = 0;

static void crc32c_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0x82F63B78u & (-(crc & 1)));
        crc32c_table[i] = crc;
    }
    crc32c_initialized = 1;
}

static uint32_t crc32c_calc(const char *data, size_t len) {
    uint32_t crc = 0;  /* match kernel crc32c() initial value */
    if (!crc32c_initialized) crc32c_init();
    for (size_t i = 0; i < len; i++)
        crc = crc32c_table[(crc ^ (uint8_t)data[i]) & 0xFF] ^ (crc >> 8);
    return crc;  /* no finalization XOR — matches kernel crc32c() */
}

int tv_metadata_save(TV_METADATA *meta, const char *path) {
    if (!meta || !path) return TV_ERR;

    /* Build content to a temp buffer for CRC computation */
    char buf[16384];
    int off = 0;

    off += snprintf(buf + off, sizeof(buf) - off, "[weighted_striping]\n");
    off += snprintf(buf + off, sizeof(buf) - off, "version=%u\n", meta->version);
    off += snprintf(buf + off, sizeof(buf) - off, "chunk_size=%u\n", meta->chunk_size);
    off += snprintf(buf + off, sizeof(buf) - off, "segment_count=%u\n", meta->segment_count);
    off += snprintf(buf + off, sizeof(buf) - off, "disk_count=%u\n", meta->disk_count);

    for (uint32_t i = 0; i < meta->disk_count; i++) {
        off += snprintf(buf + off, sizeof(buf) - off, "disk%u_name=%s\n", i, meta->disk_names[i]);
    }

    for (uint32_t i = 0; i < meta->segment_count; i++) {
        TV_SEGMENT *seg = &meta->segments[i];
        off += snprintf(buf + off, sizeof(buf) - off, "seg%u_begin=%lu\n", i, (unsigned long)seg->logical_begin);
        off += snprintf(buf + off, sizeof(buf) - off, "seg%u_end=%lu\n", i, (unsigned long)seg->logical_end);
        off += snprintf(buf + off, sizeof(buf) - off, "seg%u_count=%u\n", i, seg->disk_count);

        off += snprintf(buf + off, sizeof(buf) - off, "seg%u_disks=", i);
        for (uint32_t j = 0; j < seg->disk_count; j++) {
            off += snprintf(buf + off, sizeof(buf) - off, "%s%u", j ? "," : "", seg->disk_index[j]);
        }
        off += snprintf(buf + off, sizeof(buf) - off, "\n");

        off += snprintf(buf + off, sizeof(buf) - off, "seg%u_weight=", i);
        for (uint32_t j = 0; j < seg->disk_count; j++) {
            off += snprintf(buf + off, sizeof(buf) - off, "%s%u", j ? "," : "", seg->weight[j]);
        }
        off += snprintf(buf + off, sizeof(buf) - off, "\n");

        off += snprintf(buf + off, sizeof(buf) - off, "seg%u_stripe=%lu\n", i, (unsigned long)seg->stripe_size);
    }

    /* Compute CRC32C */
    uint32_t crc = crc32c_calc(buf, off);

    /* Write backup of current file if it exists */
    char bak_path[256];
    snprintf(bak_path, sizeof(bak_path), "%s.bak", path);
    unlink(bak_path);
    rename(path, bak_path);

    /* Write new file with CRC */
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "metadata: cannot write to '%s'\n", path);
        return TV_ERR;
    }

    fwrite(buf, 1, off, f);
    fprintf(f, "crc32=%u\n", crc);
    fclose(f);

    fprintf(stderr, "metadata: saved with CRC32=0x%08X to '%s'\n", crc, path);
    return 0;
}

static int parse_line(char *line, char **key, char **val) {
    char *eq = strchr(line, '=');
    if (!eq) return TV_ERR;
    *eq = 0;
    *key = line;
    *val = eq + 1;
    /* Strip trailing newline */
    char *nl = strchr(*val, '\n');
    if (nl) *nl = 0;
    return 0;
}

int tv_metadata_load(TV_METADATA *meta, const char *path) {
    if (!meta || !path) return TV_ERR;

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "metadata: cannot read '%s'\n", path);
        return TV_ERR;
    }

    memset(meta, 0, sizeof(TV_METADATA));

    char line[1024];

    while (fgets(line, sizeof(line), f)) {
        char *key, *val;
        if (parse_line(line, &key, &val) < 0) continue;

        if (strcmp(key, "version") == 0) {
            meta->version = (uint32_t)strtoul(val, NULL, 10);
        } else if (strcmp(key, "chunk_size") == 0) {
            meta->chunk_size = (uint32_t)strtoul(val, NULL, 10);
        } else if (strcmp(key, "segment_count") == 0) {
            meta->segment_count = (uint32_t)strtoul(val, NULL, 10);
            if (meta->segment_count > TV_MAX_SEGS) {
                fprintf(stderr, "metadata: segment_count %u exceeds max %d\n",
                        meta->segment_count, TV_MAX_SEGS);
                fclose(f); return TV_ERR;
            }
        } else if (strcmp(key, "disk_count") == 0) {
            meta->disk_count = (uint32_t)strtoul(val, NULL, 10);
            if (meta->disk_count > TV_MAX_DISKS) {
                fprintf(stderr, "metadata: disk_count %u exceeds max %d\n",
                        meta->disk_count, TV_MAX_DISKS);
                fclose(f); return TV_ERR;
            }
        } else if (strncmp(key, "disk", 4) == 0) {
            /* disk0_name=... */
            char *endp;
            unsigned long idx = strtoul(key + 4, &endp, 10);
            if (endp && strcmp(endp, "_name") == 0 && idx < TV_MAX_DISKS &&
                idx < meta->disk_count) {
                strncpy(meta->disk_names[idx], val, 63);
                meta->disk_names[idx][63] = 0;
            }
        } else if (strncmp(key, "seg", 3) == 0) {
            char *endp;
            unsigned long idx = strtoul(key + 3, &endp, 10);
            if (idx >= TV_MAX_SEGS) continue;

            TV_SEGMENT *seg = &meta->segments[idx];

            if (strcmp(endp, "_begin") == 0) {
                seg->logical_begin = strtoull(val, NULL, 10);
            } else if (strcmp(endp, "_end") == 0) {
                seg->logical_end = strtoull(val, NULL, 10);
            } else if (strcmp(endp, "_count") == 0) {
                seg->disk_count = (uint32_t)strtoul(val, NULL, 10);
            } else if (strcmp(endp, "_stripe") == 0) {
                seg->stripe_size = strtoull(val, NULL, 10);
            } else if (strcmp(endp, "_disks") == 0) {
                char *saveptr;
                char *t = strtok_r(val, ",", &saveptr);
                int j = 0;
                while (t && j < TV_MAX_DISKS) {
                    seg->disk_index[j++] = (uint32_t)strtoul(t, NULL, 10);
                    t = strtok_r(NULL, ",", &saveptr);
                }
            } else if (strcmp(endp, "_weight") == 0) {
                char *saveptr;
                char *t = strtok_r(val, ",", &saveptr);
                int j = 0;
                while (t && j < TV_MAX_DISKS) {
                    seg->weight[j++] = (uint32_t)strtoul(t, NULL, 10);
                    t = strtok_r(NULL, ",", &saveptr);
                }
            }
        }
    }

    fclose(f);

    /* Validate disk indices after all lines are parsed (allows any parse order) */
    for (unsigned long si = 0; si < meta->segment_count; si++) {
        TV_SEGMENT *seg = &meta->segments[si];
        for (uint32_t j = 0; j < seg->disk_count; j++) {
            if (seg->disk_index[j] >= meta->disk_count) {
                fprintf(stderr, "metadata: seg%lu disk index %u >= disk_count %u\n",
                        si, seg->disk_index[j], meta->disk_count);
                return TV_ERR;
            }
        }
    }

    return 0;
}
