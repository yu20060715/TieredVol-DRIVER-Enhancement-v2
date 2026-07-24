#ifndef TIEREDVOL_H
#define TIEREDVOL_H

#include <linux/types.h>
#include <linux/device-mapper.h>
#include <linux/atomic.h>

#define TV_MAX_DISKS    16
#define TV_MAX_SEGS     16
#define TV_MAX_WEIGHT   16
#define TV_CHUNK_SIZE   (1UL << 20)
#define TV_SECTOR_SHIFT 9
#define TV_SECTOR_SIZE  (1 << TV_SECTOR_SHIFT)
#define TV_IO_ERROR_THRESHOLD 15

struct tieredvol_segment {
	u64 logical_begin;
	u64 logical_end;
	u32 disk_count;
	u32 disk_index[TV_MAX_DISKS];
	u32 weight[TV_MAX_DISKS];
	u64 stripe_size;
};

struct tieredvol_metadata {
	u32 version;
	u32 chunk_size;
	u32 segment_count;
	u32 disk_count;
	char disk_names[TV_MAX_DISKS][64];
	struct tieredvol_segment segments[TV_MAX_SEGS];
};

struct tieredvol_map {
	int disk;
	u64 offset;
	u64 length;
};

struct tieredvol_ctx {
	struct dm_target *ti;
	struct tieredvol_metadata meta;
	struct dm_dev **devs;
	sector_t *disk_sectors;
	int ndisks;
	sector_t min_chunk_sectors;
	sector_t stripe_sectors;
	atomic_t *error_count;
	struct work_struct trigger_event;
};

struct tieredvol_map tv_map_logical(u64 logical,
				    struct tieredvol_metadata *meta);
int tv_metadata_load_kernel(struct tieredvol_metadata *meta,
			    const char *path);

#endif
