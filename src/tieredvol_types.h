#ifndef TIEREDVOL_TYPES_H
#define TIEREDVOL_TYPES_H

#include <stdint.h>
#include <signal.h>
#include "tieredvol_meta_format.h"

extern volatile sig_atomic_t g_shutdown_requested;

/* Aliases: canonical constants live in tieredvol_meta_format.h */
#define TV_MAX_DISKS    TV_META_MAX_DISKS
#define TV_MAX_SEGS     TV_META_MAX_SEGS
#define TV_MAX_WEIGHT   TV_META_MAX_WEIGHT
#define TV_CHUNK_SIZE   TV_META_CHUNK_SIZE
#define TV_OK       0
#define TV_ERR      (-1)

#define DEFAULT_STRIPE_SIZE_KB  512
#define TV_ALLOC_ALIGNMENT      4096
#define TV_CONFIG_DIR           "/etc/tieredvol/"
#define TV_PROGRESS_INTERVAL    (64 * 1024 * 1024)

typedef struct {
    int      id;
    int      fd;
    uint64_t total_size;
    uint64_t free_size;
    uint64_t speed;
    uint32_t weight;
    uint64_t physical_offset;
    char     name[64];
} TV_DISK;

typedef struct {
    uint64_t logical_begin;
    uint64_t logical_end;
    uint32_t disk_count;
    uint32_t disk_index[TV_MAX_DISKS];
    uint32_t weight[TV_MAX_DISKS];
    uint64_t stripe_size;
} TV_SEGMENT;

typedef struct {
    uint32_t version;
    uint32_t chunk_size;
    uint32_t segment_count;
    uint32_t disk_count;
    char     disk_names[TV_MAX_DISKS][64];
    TV_SEGMENT segments[TV_MAX_SEGS];
} TV_METADATA;

typedef struct {
    int      disk;
    uint64_t offset;
    uint64_t length;
} TV_MAP;

uint32_t tv_compute_weight(uint64_t speed, uint64_t slowest);
int      tv_build_segments(TV_DISK *disks, int ndisks, TV_SEGMENT *segs, int *nsegs);
uint64_t tv_compute_stripe_size(uint32_t *weights, int nweights, uint32_t chunk_size);

TV_MAP   tv_map_logical(uint64_t logical, TV_METADATA *meta);

int  tv_metadata_save(TV_METADATA *meta, const char *path);
int  tv_metadata_load(TV_METADATA *meta, const char *path);

int  tv_benchmark(const char *disk_path, uint64_t *speed_out, int warmup);

#endif
