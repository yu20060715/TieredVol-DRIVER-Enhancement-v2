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
	/* Runtime defaults persisted in [runtime] section */
	int runtime_policy;
	u32 runtime_stale_ms;
	u32 runtime_ema_shift;
	u32 runtime_wear_bias;
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
	char config_path[256];
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
	bool degraded[TV_MAX_DISKS];
	u32 error_threshold;
	/* Mirror rebuild state (2c) */
	struct task_struct *rebuild_thread;
	int rebuild_seg_idx;
	u64 rebuild_offset;
	u64 rebuild_total;
	atomic_t rebuild_running;
	struct work_struct trigger_event;
};

struct tieredvol_map tv_map_logical(u64 logical,
				    struct tieredvol_metadata *meta,
				    u32 chunk_size);
struct tieredvol_map tv_map_logical_adaptive(u64 logical,
					    struct tieredvol_metadata *meta,
					    u64 *ema_load, bool *stale,
					    bool *degraded,
					    int ndisks,
					    u64 *total_write_bytes,
					    u32 wear_bias,
					    u32 chunk_size);
struct tieredvol_map tv_map_logical_random(u64 logical,
					  struct tieredvol_metadata *meta,
					  u32 chunk_size);
int tv_metadata_load_kernel(struct tieredvol_metadata *meta,
			    const char *path);

#define TV_LOG_SIZE 512

struct tv_log_entry {
	u64  timestamp_ns;
	u8   level;
	u8   disk_idx;
	u8   event_type;
	char msg[48];
};

enum tv_log_level {
	TV_LOG_OFF  = 0,
	TV_LOG_ERR  = 1,
	TV_LOG_WARN = 2,
	TV_LOG_INFO = 3,
};

enum tv_log_event {
	TV_LOG_IO      = 0,
	TV_LOG_STALE   = 1,
	TV_LOG_RECOVER = 2,
	TV_LOG_MIRROR  = 3,
	TV_LOG_CONFIG  = 4,
};

#endif
