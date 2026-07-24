#ifndef TIEREDVOL_H
#define TIEREDVOL_H

#include <linux/types.h>
#include <linux/device-mapper.h>
#include <linux/atomic.h>
#include <linux/ktime.h>
#include <linux/timer.h>
#include <linux/bio.h>

#define TV_MAX_DISKS    16
#define TV_MAX_SEGS     16
#define TV_MAX_WEIGHT   16
#define TV_CHUNK_SIZE   (1UL << 20)
#define TV_SECTOR_SHIFT 9
#define TV_SECTOR_SIZE  (1 << TV_SECTOR_SHIFT)

struct tieredvol_segment {
	u64 logical_begin;
	u64 logical_end;
	u32 disk_count;
	u32 disk_index[TV_MAX_DISKS];
	u32 weight[TV_MAX_DISKS];
	u64 stripe_size;
	bool mirror_enabled;
	u32 mirror_disk;
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
	int seg_idx;
	u64 offset;
	u64 length;
};

enum tv_policy {
	TV_POLICY_STATIC = 0,
	TV_POLICY_ADAPTIVE = 1,
	TV_POLICY_RANDOM = 2,
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
	atomic_t in_flight_bytes[TV_MAX_DISKS];
	u64 last_finish_ns[TV_MAX_DISKS];
	bool adaptive_enabled;
	u32 ema_weight_shift;
	u64 ema_load[TV_MAX_DISKS];
	u64 stale_after_ns;
	bool stale[TV_MAX_DISKS];
	u64 stale_marked_ns[TV_MAX_DISKS];
	u64 grace_until_ns[TV_MAX_DISKS];
	struct timer_list decay_timer;
	u64 last_interval_bytes[TV_MAX_DISKS];
	u64 total_write_bytes[TV_MAX_DISKS];
	u64 total_read_bytes[TV_MAX_DISKS];
	u64 total_write_ops[TV_MAX_DISKS];
	u64 total_read_ops[TV_MAX_DISKS];
	u32 wear_bias;
	enum tv_policy policy;
	u64 mirror_write_bytes;
	u64 mirror_write_ops;
	u64 mirror_errors;
	struct work_struct trigger_event;
};

struct tieredvol_map tv_map_logical(u64 logical,
				    struct tieredvol_metadata *meta);
struct tieredvol_map tv_map_logical_adaptive(u64 logical,
					    struct tieredvol_metadata *meta,
					    u64 *ema_load, bool *stale,
					    int ndisks,
					    u64 *total_write_bytes,
					    u32 wear_bias);
struct tieredvol_map tv_map_logical_random(u64 logical,
					  struct tieredvol_metadata *meta);
int tv_metadata_load_kernel(struct tieredvol_metadata *meta,
			    const char *path);

#endif
