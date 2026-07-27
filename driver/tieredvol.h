#ifndef TIEREDVOL_H
#define TIEREDVOL_H

#include <linux/types.h>
#include <linux/device-mapper.h>
#include <linux/atomic.h>
#include <linux/rcupdate.h>
#include <linux/ktime.h>
#include <linux/timer.h>
#include <linux/bio.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include "tieredvol_meta_format.h"

/* Aliases: canonical constants live in tieredvol_meta_format.h */
#define TV_MAX_DISKS    TV_META_MAX_DISKS
#define TV_MAX_SEGS     TV_META_MAX_SEGS
#define TV_MAX_WEIGHT   TV_META_MAX_WEIGHT
#define TV_CHUNK_SIZE   TV_META_CHUNK_SIZE
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

/* Phase 2: Sub-structs for tieredvol_ctx */

struct tv_io_stats {
	atomic_t in_flight_bytes[TV_MAX_DISKS];
	atomic64_t total_write_bytes[TV_MAX_DISKS];
	atomic64_t total_read_bytes[TV_MAX_DISKS];
	atomic64_t total_write_ops[TV_MAX_DISKS];
	atomic64_t total_read_ops[TV_MAX_DISKS];
	/* Adaptive v2: latency + IOPS tracking */
	atomic64_t total_latency_ns[TV_MAX_DISKS];
	atomic64_t total_completions[TV_MAX_DISKS];
	atomic64_t interval_completions[TV_MAX_DISKS];
};

struct tv_adaptive_state {
	u32 ema_weight_shift;
	u64 ema_load[TV_MAX_DISKS];
	u64 stale_after_ns;
	bool stale[TV_MAX_DISKS];
	u64 stale_marked_ns[TV_MAX_DISKS];
	u64 grace_until_ns[TV_MAX_DISKS];
	u64 last_finish_ns[TV_MAX_DISKS];
	u64 last_interval_bytes[TV_MAX_DISKS];
	struct timer_list decay_timer;
	u32 wear_bias;
	enum tv_policy policy;
	/* Adaptive v2: multi-factor scoring */
	u64 ema_latency_ns[TV_MAX_DISKS];
	u64 ema_iops[TV_MAX_DISKS];
	u32 write_boost;
};

struct tv_mirror_stats {
	atomic64_t mirror_write_bytes;
	atomic64_t mirror_write_ops;
	atomic64_t mirror_read_ops;
	atomic64_t mirror_errors;
};

struct tv_rebuild_state {
	struct task_struct *thread;
	int seg_idx;
	u64 offset;
	u64 total;
	atomic_t running;
	struct completion done_r;
	struct completion done_w;
};

struct tv_degradation {
	atomic_t *error_count;
	u32 error_threshold;
	bool degraded[TV_MAX_DISKS];
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
	struct tv_io_stats io;
	struct tv_degradation deg;
	struct tv_adaptive_state adaptive;
	struct tv_mirror_stats mirror;
	struct tv_rebuild_state rebuild;
	bool mirror_enabled_any;
	struct work_struct trigger_event;
	mempool_t *mirror_pw_pool;
	mempool_t *retry_ctx_pool;
};

/* ---- tieredvol_map.c exports ---- */
struct tieredvol_map tv_map_logical(u64 logical,
				    struct tieredvol_metadata *meta,
				    u32 chunk_size);
struct tieredvol_map tv_map_logical_adaptive(u64 logical,
					    struct tieredvol_metadata *meta,
					    u64 *ema_load, bool *stale,
					    bool *degraded,
					    int ndisks,
					    atomic64_t *total_write_bytes,
					    u32 wear_bias,
					    u32 chunk_size);
struct tieredvol_map tv_map_logical_random(u64 logical,
					  struct tieredvol_metadata *meta,
					  u32 chunk_size);

/* ---- tieredvol_meta.c exports ---- */
int tv_metadata_load_kernel(struct tieredvol_metadata *meta,
			    const char *path);

/* ---- tieredvol_log.c exports ---- */
void tv_log(u8 level, u8 disk_idx, u8 event_type, const char *fmt, ...);
u64 tv_read_count(void);
u64 tv_read_sectors(void);
u64 tv_read_bytes(void);
void tv_reset_stats(void);

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

extern struct kfifo tv_log_fifo;
extern raw_spinlock_t tv_log_lock;
extern u8 tv_log_level;
extern unsigned int log_size;
extern struct workqueue_struct *tv_wq;

/* ---- Pending-read tracking (per-CPU, lockless) ---- */
struct tv_pending_read_entry {
	struct block_device *bdev;
	sector_t sector;
	sector_t mirror_sector;
	unsigned int size;
	int mirror_disk;
};

struct tv_pending_read_cpu {
	struct tv_pending_read_entry entries[64];
	unsigned int head;
	unsigned int count;
};

/* ---- Pending-write tracking (per-CPU, lockless) ---- */
struct tv_pending_write_entry {
	struct block_device *bdev;
	sector_t sector;
	unsigned int size;
};

struct tv_pending_write_cpu {
	struct tv_pending_write_entry entries[64];
	unsigned int head;
	unsigned int count;
};

/* ---- tieredvol_mirror.c exports ---- */
struct tv_mirror_pw_ctx {
	struct tieredvol_ctx *ctx;
	struct block_device *bdev;
	sector_t sector;
	unsigned int size;
};
void tv_pw_add(struct block_device *bdev, sector_t sector, unsigned int size);
void tv_pending_add(struct block_device *bdev, sector_t sector,
		    unsigned int size, int mirror_disk,
		    sector_t mirror_sector);
int tv_pending_find_and_remove(struct block_device *bdev, sector_t sector,
			       unsigned int size, sector_t *mirror_sector_out);
void tv_mirror_end_io(struct bio *bio);
int tieredvol_end_io(struct dm_target *ti, struct bio *bio,
		     blk_status_t *error);
void tv_decay_timer_fn(struct timer_list *timer);

struct tv_retry_ctx {
	struct delayed_work dwork;
	struct tieredvol_ctx *ctx;
	struct bio *orig_bio;
	sector_t sector;
	unsigned int size;
	int mirror_disk;
	int retries;
};

int tv_rebuild_thread(void *data);

/* ---- tieredvol_sysfs.c exports ---- */
void tv_sysfs_init(void);
void tv_sysfs_exit(void);

/* ---- tieredvol_message.c exports ---- */
int tieredvol_message(struct dm_target *ti, unsigned int argc,
		      char **argv, char *result, unsigned int maxlen);

/* ---- Global active context (RCU-protected) ---- */
extern struct tieredvol_ctx __rcu *tv_active_ctx;

#endif
