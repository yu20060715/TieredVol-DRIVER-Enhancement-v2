#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/device-mapper.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/percpu.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/kfifo.h>
#include <linux/sysfs.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/crc32c.h>
#include "tieredvol.h"

#define DM_MSG_PREFIX "tieredvol"

static void tv_sysfs_init(void);
static void tv_sysfs_exit(void);

static struct kobject *tv_kobj;
static struct tieredvol_ctx *tv_active_ctx;

/*
 * Per-disk load tracking via map()-only counters with time-decay.
 * We cannot reliably identify which disk a bio belongs to in end_io()
 * because DM strips bi_opf and restores bi_bdev. Instead, we track
 * "bytes recently sent" per disk and decay them over time.
 * This is an approximation good enough for load-balancing decisions.
 */

static struct workqueue_struct *tv_wq;

/* Forward declarations for pending-write tracking (1b) */
static void tv_pw_add(struct block_device *bdev, sector_t sector,
		      unsigned int size);
static void tv_pw_remove(struct block_device *bdev, sector_t sector,
			 unsigned int size);
static bool tv_pw_is_pending(struct block_device *bdev, sector_t sector,
			     unsigned int size);

static DEFINE_PER_CPU(u64, tv_map_count);
static DEFINE_PER_CPU(u64, tv_map_sectors);
static DEFINE_PER_CPU(u64, tv_map_bytes);

/* Configurable log buffer — default 512 entries, set via module_param */
static unsigned int log_size = TV_LOG_SIZE;
module_param(log_size, uint, 0644);
MODULE_PARM_DESC(log_size, "Ring buffer log entries (default 512, power of 2)");

static struct kfifo tv_log_fifo;
static DEFINE_SPINLOCK(tv_log_lock);
static u8 tv_log_level = TV_LOG_INFO;

static void tv_log(u8 level, u8 disk_idx, u8 event_type, const char *fmt, ...)
{
	struct tv_log_entry entry;
	va_list args;
	unsigned long flags;

	if (level > tv_log_level)
		return;

	entry.timestamp_ns = ktime_get_ns();
	entry.level = level;
	entry.disk_idx = disk_idx;
	entry.event_type = event_type;

	va_start(args, fmt);
	vsnprintf(entry.msg, sizeof(entry.msg), fmt, args);
	va_end(args);

	spin_lock_irqsave(&tv_log_lock, flags);
	kfifo_in(&tv_log_fifo, &entry, sizeof(entry));
	spin_unlock_irqrestore(&tv_log_lock, flags);
}

static void trigger_event(struct work_struct *work)
{
	struct tieredvol_ctx *ctx = container_of(work, struct tieredvol_ctx,
						 trigger_event);
	dm_table_event(ctx->ti->table);
}

static inline u64 tv_read_count(void)
{
	u64 total = 0;
	int cpu;
	for_each_possible_cpu(cpu)
		total += per_cpu(tv_map_count, cpu);
	return total;
}

static inline u64 tv_read_sectors(void)
{
	u64 total = 0;
	int cpu;
	for_each_possible_cpu(cpu)
		total += per_cpu(tv_map_sectors, cpu);
	return total;
}

static inline u64 tv_read_bytes(void)
{
	u64 total = 0;
	int cpu;
	for_each_possible_cpu(cpu)
		total += per_cpu(tv_map_bytes, cpu);
	return total;
}

#define TV_DECAY_INTERVAL (HZ)

static void tv_decay_timer_fn(struct timer_list *timer)
{
	struct tieredvol_ctx *ctx = from_timer(ctx, timer, decay_timer);
	u32 alpha_shift = ctx->ema_weight_shift;
	u64 alpha = (alpha_shift < 10) ? (1ULL << alpha_shift) : 1024;
	u64 one_minus_alpha = 1024 - alpha;
	u64 now = ktime_get_boottime_ns();
	int i;

	for (i = 0; i < ctx->ndisks; i++) {
		u64 snapshot = (u64)atomic_xchg(&ctx->in_flight_bytes[i], 0);

		ctx->ema_load[i] = (ctx->ema_load[i] * one_minus_alpha +
				    snapshot * alpha) >> 10;

		ctx->last_interval_bytes[i] = snapshot;

		if (ctx->stale_after_ns > 0 && snapshot > 0)
			ctx->last_finish_ns[i] = now;

		if (ctx->stale_after_ns > 0 &&
		    !ctx->stale[i] &&
		    ctx->last_finish_ns[i] > 0 &&
		    now > ctx->grace_until_ns[i] &&
		    (now - ctx->last_finish_ns[i]) > ctx->stale_after_ns) {
			ctx->stale[i] = true;
			ctx->stale_marked_ns[i] = now;
			pr_info("tieredvol: disk[%d] %s STALE (no I/O for %llu ms)\n",
				i, ctx->meta.disk_names[i],
				(now - ctx->last_finish_ns[i]) / 1000000ULL);
			tv_log(TV_LOG_WARN, i, TV_LOG_STALE,
			       "STALE %llums", (now - ctx->last_finish_ns[i]) / 1000000ULL);
		} else if (ctx->stale[i] && snapshot > 0) {
			ctx->stale[i] = false;
			ctx->grace_until_ns[i] = now + ctx->stale_after_ns;
			pr_info("tieredvol: disk[%d] %s RECOVERED (I/O resumed)\n",
				i, ctx->meta.disk_names[i]);
			tv_log(TV_LOG_INFO, i, TV_LOG_RECOVER, "RECOVERED io");
		} else if (ctx->stale[i] &&
			   (now - ctx->stale_marked_ns[i]) >
			   2 * ctx->stale_after_ns) {
			ctx->stale[i] = false;
			ctx->grace_until_ns[i] = now + ctx->stale_after_ns;
			pr_info("tieredvol: disk[%d] %s RECOVERED (cooldown)\n",
				i, ctx->meta.disk_names[i]);
			tv_log(TV_LOG_INFO, i, TV_LOG_RECOVER, "RECOVERED cooldown");
		}
	}

	mod_timer(&ctx->decay_timer, jiffies + TV_DECAY_INTERVAL);
}

static void tv_mirror_end_io(struct bio *bio)
{
	struct tieredvol_ctx *bio_ctx = bio->bi_private;

	if (bio->bi_status != BLK_STS_OK)
		bio_ctx->mirror_errors++;
	else
		bio_ctx->mirror_write_ops++;

	/* 1b: Remove from pending-write tracking — mirror write done */
	tv_pw_remove(bio->bi_bdev, bio->bi_iter.bi_sector, bio->bi_iter.bi_size);

	bio_put(bio);
}

/*
 * Pending-read tracking for mirror read fallback (1c).
 * In tieredvol_map(), for READ bios on mirrored segments, we record the
 * mapping so end_io can resubmit to the mirror on error.
 */
struct tv_pending_read {
	struct block_device *bdev;
	sector_t sector;
	unsigned int size;
	int mirror_disk;
};

#define TV_PENDING_MAX 64
static struct tv_pending_read tv_pending_reads[TV_PENDING_MAX];
static unsigned int tv_pending_head;
static unsigned int tv_pending_count;
static DEFINE_SPINLOCK(tv_pending_lock);

/*
 * Pending-write tracking for mirror write ordering (1b).
 * When a mirror write is submitted, we add an entry. When it completes
 * (tv_mirror_end_io), we remove it. Read retries check this before
 * retrying on the mirror — if a write is still pending, the retry
 * is rescheduled after a short delay.
 */
struct tv_pending_write {
	struct block_device *bdev;
	sector_t sector;
	unsigned int size;
};

static struct tv_pending_write tv_pending_writes[TV_PENDING_MAX];
static unsigned int tv_pw_head;
static unsigned int tv_pw_count;
static DEFINE_SPINLOCK(tv_pw_lock);

static void tv_pending_add(struct block_device *bdev, sector_t sector,
			   unsigned int size, int mirror_disk)
{
	unsigned long flags;
	unsigned int idx;

	spin_lock_irqsave(&tv_pending_lock, flags);
	idx = (tv_pending_head + tv_pending_count) % TV_PENDING_MAX;
	if (tv_pending_count < TV_PENDING_MAX) {
		tv_pending_reads[idx].bdev = bdev;
		tv_pending_reads[idx].sector = sector;
		tv_pending_reads[idx].size = size;
		tv_pending_reads[idx].mirror_disk = mirror_disk;
		tv_pending_count++;
	}
	spin_unlock_irqrestore(&tv_pending_lock, flags);
}

static int tv_pending_find_and_remove(struct block_device *bdev, sector_t sector,
				     unsigned int size)
{
	unsigned long flags;
	int mirror_disk = -1;
	unsigned int i;

	spin_lock_irqsave(&tv_pending_lock, flags);
	for (i = 0; i < tv_pending_count; i++) {
		unsigned int idx = (tv_pending_head + i) % TV_PENDING_MAX;
		struct tv_pending_read *pr = &tv_pending_reads[idx];

		if (pr->bdev == bdev && pr->sector == sector && pr->size == size) {
			mirror_disk = pr->mirror_disk;
			/* Shift remaining entries down */
			unsigned int j;

			for (j = i; j + 1 < tv_pending_count; j++) {
				unsigned int next = (tv_pending_head + j + 1) % TV_PENDING_MAX;

				tv_pending_reads[(tv_pending_head + j) % TV_PENDING_MAX] =
					tv_pending_reads[next];
			}
			tv_pending_count--;
			break;
		}
	}
	spin_unlock_irqrestore(&tv_pending_lock, flags);
	return mirror_disk;
}

static void tv_pw_add(struct block_device *bdev, sector_t sector,
		      unsigned int size)
{
	unsigned long flags;
	unsigned int idx;

	spin_lock_irqsave(&tv_pw_lock, flags);
	idx = (tv_pw_head + tv_pw_count) % TV_PENDING_MAX;
	if (tv_pw_count < TV_PENDING_MAX) {
		tv_pending_writes[idx].bdev = bdev;
		tv_pending_writes[idx].sector = sector;
		tv_pending_writes[idx].size = size;
		tv_pw_count++;
	}
	spin_unlock_irqrestore(&tv_pw_lock, flags);
}

static void tv_pw_remove(struct block_device *bdev, sector_t sector,
			 unsigned int size)
{
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&tv_pw_lock, flags);
	for (i = 0; i < tv_pw_count; i++) {
		unsigned int idx = (tv_pw_head + i) % TV_PENDING_MAX;
		struct tv_pending_write *pw = &tv_pending_writes[idx];

		if (pw->bdev == bdev && pw->sector == sector && pw->size == size) {
			unsigned int j;

			for (j = i; j + 1 < tv_pw_count; j++) {
				unsigned int next = (tv_pw_head + j + 1) % TV_PENDING_MAX;

				tv_pending_writes[(tv_pw_head + j) % TV_PENDING_MAX] =
					tv_pending_writes[next];
			}
			tv_pw_count--;
			break;
		}
	}
	spin_unlock_irqrestore(&tv_pw_lock, flags);
}

static bool tv_pw_is_pending(struct block_device *bdev, sector_t sector,
			     unsigned int size)
{
	unsigned long flags;
	bool found = false;
	unsigned int i;

	spin_lock_irqsave(&tv_pw_lock, flags);
	for (i = 0; i < tv_pw_count; i++) {
		unsigned int idx = (tv_pw_head + i) % TV_PENDING_MAX;
		struct tv_pending_write *pw = &tv_pending_writes[idx];

		if (pw->bdev == bdev && pw->sector == sector && pw->size == size) {
			found = true;
			break;
		}
	}
	spin_unlock_irqrestore(&tv_pw_lock, flags);
	return found;
}

struct tv_retry_ctx {
	struct delayed_work dwork;
	struct tieredvol_ctx *ctx;
	sector_t sector;
	unsigned int size;
	int mirror_disk;
};

static void tv_read_retry_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct tv_retry_ctx *rc = container_of(dwork, struct tv_retry_ctx, dwork);
	struct bio *clone;

	/* 1b: If mirror write still in-flight, reschedule after 1ms */
	if (tv_pw_is_pending(rc->ctx->devs[rc->mirror_disk]->bdev,
			     rc->sector, rc->size)) {
		tv_log(TV_LOG_INFO, rc->mirror_disk, TV_LOG_MIRROR,
		       "retry delayed (mirror write pending) sec=%llu",
		       (u64)rc->sector);
		/* Reschedule with 1ms delay */
		schedule_delayed_work(&rc->dwork, msecs_to_jiffies(1));
		return;
	}

	clone = bio_alloc(rc->ctx->devs[rc->mirror_disk]->bdev,
			  1, REQ_OP_READ, GFP_NOIO);
	if (!clone) {
		tv_log(TV_LOG_ERR, rc->mirror_disk, TV_LOG_MIRROR,
		       "retry alloc failed sec=%llu", (u64)rc->sector);
		goto out;
	}

	clone->bi_iter.bi_sector = rc->sector;
	clone->bi_iter.bi_size = rc->size;
	clone->bi_end_io = tv_mirror_end_io;
	clone->bi_private = rc->ctx;

	submit_bio(clone);
	tv_log(TV_LOG_INFO, rc->mirror_disk, TV_LOG_MIRROR,
	       "retry read -> disk%d sec=%llu",
	       rc->mirror_disk, (u64)rc->sector);

out:
	kfree(rc);
}

static int tieredvol_map(struct dm_target *ti, struct bio *bio)
{
	struct tieredvol_ctx *ctx = ti->private;
	u64 logical;
	struct tieredvol_map cur;

	logical = (u64)bio->bi_iter.bi_sector << SECTOR_SHIFT;

	switch (ctx->policy) {
	case TV_POLICY_ADAPTIVE:
		cur = tv_map_logical_adaptive(logical, &ctx->meta,
					      ctx->ema_load, ctx->stale,
					      ctx->degraded,
					      ctx->ndisks,
					      ctx->total_write_bytes,
					      ctx->wear_bias,
					      ctx->meta.chunk_size);
		break;
	case TV_POLICY_RANDOM:
		cur = tv_map_logical_random(logical, &ctx->meta,
					    ctx->meta.chunk_size);
		break;
	case TV_POLICY_STATIC:
	default:
		cur = tv_map_logical(logical, &ctx->meta,
				     ctx->meta.chunk_size);
		break;
	}

	if (cur.disk < 0 || cur.disk >= ctx->ndisks) {
		pr_err("tieredvol: map failed for sector %llu\n",
		       (unsigned long long)bio->bi_iter.bi_sector);
		tv_log(TV_LOG_ERR, 0, TV_LOG_IO,
		       "map fail sec=%llu", bio->bi_iter.bi_sector);
		bio_io_error(bio);
		return DM_MAPIO_SUBMITTED;
	}

	bio_set_dev(bio, ctx->devs[cur.disk]->bdev);
	bio->bi_iter.bi_sector = cur.offset >> SECTOR_SHIFT;
	atomic_add(bio->bi_iter.bi_size, &ctx->in_flight_bytes[cur.disk]);
	if (bio_data_dir(bio) == WRITE) {
		ctx->total_write_bytes[cur.disk] += bio->bi_iter.bi_size;
		ctx->total_write_ops[cur.disk]++;
	} else {
		ctx->total_read_bytes[cur.disk] += bio->bi_iter.bi_size;
		ctx->total_read_ops[cur.disk]++;
	}
	this_cpu_inc(tv_map_count);
	this_cpu_add(tv_map_sectors, bio_sectors(bio));
	this_cpu_add(tv_map_bytes, bio->bi_iter.bi_size);

	/* Mirror write: clone bio and submit to mirror disk */
	if (bio_data_dir(bio) == WRITE &&
	    cur.seg_idx >= 0 &&
	    cur.seg_idx < (int)ctx->meta.segment_count) {
		struct tieredvol_segment *seg =
			&ctx->meta.segments[cur.seg_idx];

		if (seg->mirror_enabled &&
		    seg->mirror_disk < (u32)ctx->ndisks &&
		    seg->mirror_disk != (u32)cur.disk) {
			struct bio *clone;

			clone = bio_alloc_clone(ctx->devs[seg->mirror_disk]->bdev,
						bio, GFP_NOIO, &fs_bio_set);
			if (clone) {
				clone->bi_iter.bi_sector = cur.offset >> SECTOR_SHIFT;
				clone->bi_private = ctx;
				clone->bi_end_io = tv_mirror_end_io;
				ctx->mirror_write_bytes += bio->bi_iter.bi_size;
				/* 1b: Track pending mirror write for ordering */
				tv_pw_add(ctx->devs[seg->mirror_disk]->bdev,
					  clone->bi_iter.bi_sector,
					  bio->bi_iter.bi_size);
				submit_bio(clone);
				tv_log(TV_LOG_INFO, cur.disk, TV_LOG_MIRROR,
				       "mirrored %uKB seg%d->disk%d",
				       bio->bi_iter.bi_size >> 10,
				       cur.seg_idx, seg->mirror_disk);
			} else {
				ctx->mirror_errors++;
				tv_log(TV_LOG_ERR, cur.disk, TV_LOG_MIRROR,
				       "mirror alloc fail seg%d", cur.seg_idx);
			}
		}
	}

	/* Mirror read fallback (1c): record pending read for retry on error */
	if (bio_data_dir(bio) == READ &&
	    cur.seg_idx >= 0 &&
	    cur.seg_idx < (int)ctx->meta.segment_count) {
		struct tieredvol_segment *seg =
			&ctx->meta.segments[cur.seg_idx];

		if (seg->mirror_enabled &&
		    seg->mirror_disk < (u32)ctx->ndisks &&
		    seg->mirror_disk != (u32)cur.disk) {
			tv_pending_add(ctx->devs[cur.disk]->bdev,
				       bio->bi_iter.bi_sector,
				       bio->bi_iter.bi_size,
				       (int)seg->mirror_disk);
		}
	}

	return DM_MAPIO_REMAPPED;
}

static int tieredvol_end_io(struct dm_target *ti, struct bio *bio,
			    blk_status_t *error)
{
	struct tieredvol_ctx *ctx = ti->private;

	if (bio->bi_status != BLK_STS_OK) {
		int i;

		for (i = 0; i < ctx->ndisks; i++) {
			if (bio->bi_bdev == ctx->devs[i]->bdev) {
				int errs = atomic_inc_return(&ctx->error_count[i]);

				tv_log(TV_LOG_ERR, i, TV_LOG_IO,
				       "I/O error on %s status=%d err=%d",
				       ctx->meta.disk_names[i],
				       bio->bi_status, errs);

				if (!ctx->degraded[i] &&
				    errs >= (int)ctx->error_threshold) {
					ctx->degraded[i] = true;
					pr_warn("tieredvol: disk[%d] %s DEGRADED (errors=%d >= threshold=%u)\n",
						i, ctx->meta.disk_names[i],
						errs, ctx->error_threshold);
					tv_log(TV_LOG_WARN, i, TV_LOG_IO,
					       "DEGRADED err=%d", errs);
					schedule_work(&ctx->trigger_event);
				}

				/* Mirror read fallback (1c) */
				if (bio_data_dir(bio) == READ) {
					int mirror = tv_pending_find_and_remove(
						bio->bi_bdev,
						bio->bi_iter.bi_sector,
						bio->bi_iter.bi_size);

					if (mirror >= 0 && mirror < ctx->ndisks) {
						struct tv_retry_ctx *rc;

					rc = kmalloc(sizeof(*rc), GFP_ATOMIC);
					if (rc) {
						INIT_DELAYED_WORK(&rc->dwork, tv_read_retry_work);
						rc->ctx = ctx;
						rc->sector = bio->bi_iter.bi_sector;
						rc->size = bio->bi_iter.bi_size;
						rc->mirror_disk = mirror;
						schedule_delayed_work(&rc->dwork, 0);
							tv_log(TV_LOG_INFO, i, TV_LOG_MIRROR,
							       "scheduling read retry -> disk%d",
							       mirror);
						}
					}
				}
				break;
			}
		}
	}

	return 0;
}

static int tieredvol_ctr(struct dm_target *ti, unsigned int argc,
			 char **argv)
{
	struct tieredvol_ctx *ctx;
	int ret, i;

	if (argc != 1) {
		ti->error = "tieredvol: expected 1 argument (config path)";
		return -EINVAL;
	}

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		ti->error = "tieredvol: out of memory";
		return -ENOMEM;
	}

	ctx->ti = ti;
	strscpy(ctx->config_path, argv[0], sizeof(ctx->config_path));

	ret = tv_metadata_load_kernel(&ctx->meta, argv[0]);
	if (ret) {
		ti->error = "tieredvol: failed to load metadata";
		goto free_ctx;
	}

	if (ctx->meta.disk_count == 0 || ctx->meta.disk_count > TV_MAX_DISKS) {
		ti->error = "tieredvol: invalid disk count";
		ret = -EINVAL;
		goto free_ctx;
	}

	ctx->ndisks = ctx->meta.disk_count;

	ctx->devs = kcalloc(ctx->ndisks, sizeof(*ctx->devs), GFP_KERNEL);
	ctx->disk_sectors = kcalloc(ctx->ndisks, sizeof(*ctx->disk_sectors),
				    GFP_KERNEL);
	if (!ctx->devs || !ctx->disk_sectors) {
		ti->error = "tieredvol: out of memory for devs";
		ret = -ENOMEM;
		goto free_devs;
	}

	for (i = 0; i < ctx->ndisks; i++) {
		ret = dm_get_device(ti, ctx->meta.disk_names[i],
				    dm_table_get_mode(ti->table),
				    &ctx->devs[i]);
		if (ret) {
			ti->error = "tieredvol: device lookup failed";
			goto put_devices;
		}
		ctx->disk_sectors[i] = bdev_nr_sectors(ctx->devs[i]->bdev);
	}

	ctx->error_count = kcalloc(ctx->ndisks, sizeof(atomic_t), GFP_KERNEL);
	if (!ctx->error_count) {
		ti->error = "tieredvol: out of memory for error_count";
		ret = -ENOMEM;
		goto put_devices;
	}

	INIT_WORK(&ctx->trigger_event, trigger_event);

	ctx->adaptive_enabled = false;
	/* Use runtime defaults from config if present, else hard defaults */
	ctx->policy = ctx->meta.runtime_policy;
	ctx->ema_weight_shift = ctx->meta.runtime_ema_shift ?
				ctx->meta.runtime_ema_shift : 3;
	ctx->stale_after_ns = ctx->meta.runtime_stale_ms ?
			      (u64)ctx->meta.runtime_stale_ms * 1000000ULL :
			      5000000000ULL;
	ctx->wear_bias = ctx->meta.runtime_wear_bias;
	ctx->mirror_write_bytes = 0;
	ctx->mirror_write_ops = 0;
	ctx->mirror_errors = 0;
	ctx->error_threshold = 10;

	timer_setup(&ctx->decay_timer, tv_decay_timer_fn, 0);
	mod_timer(&ctx->decay_timer, jiffies + TV_DECAY_INTERVAL);

	for (i = 0; i < ctx->ndisks; i++)
		pr_info("tieredvol: disk[%d] %s -> %pg (%llu sectors)\n",
			i, ctx->meta.disk_names[i], ctx->devs[i]->bdev,
			(unsigned long long)ctx->disk_sectors[i]);

	if (ctx->meta.segment_count == 0) {
		ti->error = "tieredvol: no segments";
		ret = -EINVAL;
		goto free_error_count;
	}

	/* Validate segments are sorted by logical_begin (required for binary search) */
	for (i = 1; i < (int)ctx->meta.segment_count; i++) {
		if (ctx->meta.segments[i].logical_begin <
		    ctx->meta.segments[i - 1].logical_begin) {
			ti->error = "tieredvol: segments not sorted by logical_begin";
			ret = -EINVAL;
			goto free_error_count;
		}
	}

	/* Compute min_chunk_sectors and stripe_sectors across all segments.
	 * min_chunk = min over all segments of (min weight * chunk_size).
	 * dm_set_target_max_io_len() ensures dm core splits bios so they
	 * never cross a disk boundary within a stripe.
	 */
	{
		sector_t global_min_chunk = (sector_t)-1;
		sector_t max_stripe = 0;
		u32 si, j;
		sector_t chunk_sectors = ctx->meta.chunk_size >> SECTOR_SHIFT;

		for (si = 0; si < ctx->meta.segment_count; si++) {
			struct tieredvol_segment *seg = &ctx->meta.segments[si];
			sector_t seg_min;

			if (seg->disk_count == 0)
				continue;

			seg_min = (sector_t)seg->weight[0] * chunk_sectors;
			for (j = 1; j < seg->disk_count; j++) {
				sector_t w = (sector_t)seg->weight[j] *
					     chunk_sectors;

				if (w < seg_min)
					seg_min = w;
			}
			if (seg_min < global_min_chunk)
				global_min_chunk = seg_min;
			if (seg->stripe_size > max_stripe)
				max_stripe = seg->stripe_size;
		}

		if (global_min_chunk == (sector_t)-1 || global_min_chunk == 0) {
			ti->error = "tieredvol: invalid chunk geometry";
			ret = -EINVAL;
			goto free_error_count;
		}

		ctx->min_chunk_sectors = global_min_chunk;
		ctx->stripe_sectors = max_stripe >> SECTOR_SHIFT;
	}

	for (i = 0; i < (int)ctx->meta.segment_count; i++)
		pr_info("tieredvol: segment[%d] [%llu, %llu) stripe=%llu disks=%u\n",
			i,
			(unsigned long long)ctx->meta.segments[i].logical_begin,
			(unsigned long long)ctx->meta.segments[i].logical_end,
			(unsigned long long)ctx->meta.segments[i].stripe_size,
			ctx->meta.segments[i].disk_count);

	pr_info("tieredvol: min_chunk=%llu sectors, stripe=%llu sectors\n",
		(unsigned long long)ctx->min_chunk_sectors,
		(unsigned long long)ctx->stripe_sectors);

	ret = dm_set_target_max_io_len(ti, ctx->min_chunk_sectors);
	if (ret) {
		ti->error = "tieredvol: dm_set_target_max_io_len failed";
		goto free_error_count;
	}

	ti->num_flush_bios = ctx->ndisks;
	ti->num_discard_bios = ctx->ndisks;
	ti->flush_bypasses_map = true;

	ti->private = ctx;
	tv_active_ctx = ctx;
	return 0;

free_error_count:
	kfree(ctx->error_count);
put_devices:
	for (i = i - 1; i >= 0; i--)
		dm_put_device(ti, ctx->devs[i]);
free_devs:
	kfree(ctx->devs);
	kfree(ctx->disk_sectors);
free_ctx:
	kfree(ctx);
	return ret;
}

static void tieredvol_dtr(struct dm_target *ti)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i;

	timer_delete_sync(&ctx->decay_timer);
	flush_work(&ctx->trigger_event);
	kfree(ctx->error_count);

	for (i = 0; i < ctx->ndisks; i++)
		dm_put_device(ti, ctx->devs[i]);

	kfree(ctx->devs);
	kfree(ctx->disk_sectors);
	if (tv_active_ctx == ctx)
		tv_active_ctx = NULL;
	kfree(ctx);
}

static int tieredvol_prepare_ioctl(struct dm_target *ti,
				   struct block_device **bdev)
{
	struct tieredvol_ctx *ctx = ti->private;

	if (ctx->ndisks > 0)
		*bdev = ctx->devs[0]->bdev;

	return 0;
}

static void tieredvol_io_hints(struct dm_target *ti,
			       struct queue_limits *limits)
{
	struct tieredvol_ctx *ctx = ti->private;

	limits->logical_block_size = 512;
	limits->physical_block_size = 512;
	limits->chunk_sectors = ctx->min_chunk_sectors;
	limits->io_min = ctx->min_chunk_sectors;
	limits->io_opt = ctx->stripe_sectors;
}

static int tieredvol_iterate_devices(struct dm_target *ti,
				     iterate_devices_callout_fn fn,
				     void *data)
{
	struct tieredvol_ctx *ctx = ti->private;
	int ret = 0;
	int i;

	for (i = 0; !ret && i < ctx->ndisks; i++)
		ret = fn(ti, ctx->devs[i], 0,
			 bdev_nr_sectors(ctx->devs[i]->bdev), data);

	return ret;
}

static void tieredvol_status(struct dm_target *ti, status_type_t type,
			     unsigned int status_flags, char *result,
			     unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;

	switch (type) {
	case STATUSTYPE_INFO: {
		int i, off = 0;

		off += snprintf(result + off, maxlen - off,
				"policy=%d mirror=%llu/%llu err=%llu",
				ctx->policy,
				ctx->mirror_write_ops,
				ctx->mirror_write_bytes,
				ctx->mirror_errors);

		for (i = 0; i < ctx->ndisks && off < (int)maxlen - 2; i++) {
			char status;

			if (ctx->degraded[i])
				status = 'D';
			else if (ctx->error_count &&
				 atomic_read(&ctx->error_count[i]))
				status = 'E';
			else
				status = 'A';

			off += snprintf(result + off, maxlen - off,
					" %c%s:rd=%llu/%llu wr=%llu/%llu",
					status,
					ctx->meta.disk_names[i],
					ctx->total_read_ops[i],
					ctx->total_read_bytes[i],
					ctx->total_write_ops[i],
					ctx->total_write_bytes[i]);
		}
		break;
	}
	case STATUSTYPE_TABLE: {
		int off = 0;
		int i;

		for (i = 0; i < ctx->ndisks && off < maxlen; i++) {
			int n = snprintf(result + off, maxlen - off,
					 "%s%s", i > 0 ? " " : "",
					 ctx->meta.disk_names[i]);
			if (n < 0)
				break;
			off += n;
		}
		break;
	}
	case STATUSTYPE_IMA:
		result[0] = '\0';
		break;
	}
}

/*
 * Metadata write-back (1d): persist runtime config changes to disk.
 * Uses kernel file I/O (filp_open/kernel_write) — acceptable for a
 * research module. Writes a backup (.bak) then the new file with CRC32.
 */
static int tv_metadata_save_kernel(struct tieredvol_ctx *ctx)
{
	struct file *f;
	char buf[4096];
	int off = 0;
	int ret;
	u32 crc;
	loff_t pos = 0;
	char bak_path[260];

	if (!ctx->config_path[0])
		return -ENOENT;

	off += scnprintf(buf + off, sizeof(buf) - off, "[weighted_striping]\n");
	off += scnprintf(buf + off, sizeof(buf) - off, "version=%u\n", ctx->meta.version);
	off += scnprintf(buf + off, sizeof(buf) - off, "chunk_size=%u\n", ctx->meta.chunk_size);
	off += scnprintf(buf + off, sizeof(buf) - off, "segment_count=%u\n", ctx->meta.segment_count);
	off += scnprintf(buf + off, sizeof(buf) - off, "disk_count=%u\n", ctx->meta.disk_count);

	for (u32 i = 0; i < ctx->meta.disk_count; i++)
		off += scnprintf(buf + off, sizeof(buf) - off, "disk%u_name=%s\n",
				 i, ctx->meta.disk_names[i]);

	for (u32 i = 0; i < ctx->meta.segment_count; i++) {
		struct tieredvol_segment *seg = &ctx->meta.segments[i];

		off += scnprintf(buf + off, sizeof(buf) - off, "seg%u_begin=%llu\n",
				 i, seg->logical_begin);
		off += scnprintf(buf + off, sizeof(buf) - off, "seg%u_end=%llu\n",
				 i, seg->logical_end);
		off += scnprintf(buf + off, sizeof(buf) - off, "seg%u_count=%u\n",
				 i, seg->disk_count);

		off += scnprintf(buf + off, sizeof(buf) - off, "seg%u_disks=", i);
		for (u32 j = 0; j < seg->disk_count; j++)
			off += scnprintf(buf + off, sizeof(buf) - off, "%s%u",
					 j ? "," : "", seg->disk_index[j]);
		off += scnprintf(buf + off, sizeof(buf) - off, "\n");

		off += scnprintf(buf + off, sizeof(buf) - off, "seg%u_weight=", i);
		for (u32 j = 0; j < seg->disk_count; j++)
			off += scnprintf(buf + off, sizeof(buf) - off, "%s%u",
					 j ? "," : "", seg->weight[j]);
		off += scnprintf(buf + off, sizeof(buf) - off, "\n");

		off += scnprintf(buf + off, sizeof(buf) - off, "seg%u_stripe=%llu\n",
				 i, seg->stripe_size);
		if (seg->mirror_enabled)
			off += scnprintf(buf + off, sizeof(buf) - off,
					 "seg%u_mirror=%u\n", i, seg->mirror_disk);
	}

	/* Runtime section */
	off += scnprintf(buf + off, sizeof(buf) - off, "[runtime]\n");
	off += scnprintf(buf + off, sizeof(buf) - off, "policy=%d\n", ctx->policy);
	off += scnprintf(buf + off, sizeof(buf) - off, "stale_ms=%llu\n",
			 ctx->stale_after_ns / 1000000ULL);
	off += scnprintf(buf + off, sizeof(buf) - off, "ema_shift=%u\n",
			 ctx->ema_weight_shift);
	off += scnprintf(buf + off, sizeof(buf) - off, "wear_bias=%u\n",
			 ctx->wear_bias);

	crc = crc32c(0, buf, off);
	off += scnprintf(buf + off, sizeof(buf) - off, "crc32=%u\n", crc);

	/* Write backup */
	scnprintf(bak_path, sizeof(bak_path), "%s.bak", ctx->config_path);

	/* Create backup of current file */
	f = filp_open(ctx->config_path, O_RDONLY, 0);
	if (!IS_ERR(f)) {
		struct file *bak;

		bak = filp_open(bak_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (!IS_ERR(bak)) {
			char kbuf[256];
			loff_t rpos = 0, wpos = 0;
			ssize_t nrd;

			while ((nrd = kernel_read(f, kbuf, sizeof(kbuf), &rpos)) > 0)
				kernel_write(bak, kbuf, nrd, &wpos);
			filp_close(bak, NULL);
		}
		filp_close(f, NULL);
	}

	/* Write new file */
	f = filp_open(ctx->config_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (IS_ERR(f)) {
		pr_err("tieredvol: save failed to open %s: %ld\n",
		       ctx->config_path, PTR_ERR(f));
		return PTR_ERR(f);
	}

	pos = 0;
	ret = kernel_write(f, buf, off, &pos);
	filp_close(f, NULL);

	if (ret != off) {
		pr_err("tieredvol: save write error %d (wrote %lld of %d)\n",
		       ret, pos, off);
		return ret < 0 ? ret : -EIO;
	}

	tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG, "metadata saved crc=0x%08x", crc);
	pr_info("tieredvol: metadata saved crc=0x%08x to %s\n", crc, ctx->config_path);
	return 0;
}

static int tieredvol_message(struct dm_target *ti, unsigned int argc,
			     char **argv, char *result, unsigned int maxlen)
{
	if (argc == 1 && strcmp(argv[0], "reset_stats") == 0) {
		int cpu;
		for_each_possible_cpu(cpu) {
			per_cpu(tv_map_count, cpu) = 0;
			per_cpu(tv_map_sectors, cpu) = 0;
			per_cpu(tv_map_bytes, cpu) = 0;
		}
		snprintf(result, maxlen, "stats reset");
		return 0;
	}
	if (argc == 1 && strcmp(argv[0], "show_stats") == 0) {
		u64 cnt = tv_read_count();
		u64 bytes = tv_read_bytes();
		u64 avg = cnt ? bytes / cnt : 0;
		pr_info("tieredvol: maps=%llu avg_bytes=%llu total_bytes=%llu",
			cnt, avg, bytes);
		snprintf(result, maxlen, "maps=%llu avg_bytes=%llu",
			 cnt, avg);
		return 0;
	}
	if (argc == 1 && strcmp(argv[0], "status") == 0) {
		struct tieredvol_ctx *ctx = ti->private;
		int i, off = 0;

		for (i = 0; i < ctx->ndisks && off < (int)maxlen - 1; i++) {
			u32 w = 0;
			int si;

			for (si = 0; si < (int)ctx->meta.segment_count; si++) {
				struct tieredvol_segment *seg = &ctx->meta.segments[si];
				int j;

				for (j = 0; j < (int)seg->disk_count; j++) {
					if (seg->disk_index[j] == (u32)i) {
						w = seg->weight[j];
						goto found;
					}
				}
			}
found:
			off += snprintf(result + off, maxlen - off,
					"disk[%d]=%s(w=%u) ",
					i, ctx->meta.disk_names[i], w);
		}
		return 0;
	}
	if (argc == 1 && strcmp(argv[0], "show_inflight") == 0) {
		struct tieredvol_ctx *ctx = ti->private;
		int i, off = 0;

		for (i = 0; i < ctx->ndisks && off < (int)maxlen - 1; i++) {
			off += snprintf(result + off, maxlen - off,
					"%s%s=%u", i > 0 ? " " : "",
					ctx->meta.disk_names[i],
					atomic_read(&ctx->in_flight_bytes[i]));
		}
		pr_info("tieredvol: %s\n", result);
		return 0;
	}
	if (argc == 1 && strcmp(argv[0], "adaptive_on") == 0) {
		struct tieredvol_ctx *ctx = ti->private;

		ctx->policy = TV_POLICY_ADAPTIVE;
		pr_info("tieredvol: policy = adaptive\n");
		tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG, "policy=adaptive");
		tv_metadata_save_kernel(ctx);
		return 0;
	}
	if (argc == 1 && strcmp(argv[0], "adaptive_off") == 0) {
		struct tieredvol_ctx *ctx = ti->private;

		ctx->policy = TV_POLICY_STATIC;
		pr_info("tieredvol: policy = static\n");
		tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG, "policy=static");
		tv_metadata_save_kernel(ctx);
		return 0;
	}
	if (argc == 2 && strcmp(argv[0], "set_policy") == 0) {
		struct tieredvol_ctx *ctx = ti->private;

		if (strcmp(argv[1], "static") == 0)
			ctx->policy = TV_POLICY_STATIC;
		else if (strcmp(argv[1], "adaptive") == 0)
			ctx->policy = TV_POLICY_ADAPTIVE;
		else if (strcmp(argv[1], "random") == 0)
			ctx->policy = TV_POLICY_RANDOM;
		else
			return -EINVAL;
		pr_info("tieredvol: policy = %s\n", argv[1]);
		tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG, "policy=%s", argv[1]);
		tv_metadata_save_kernel(ctx);
		return 0;
	}
	if (argc == 2 && strcmp(argv[0], "set_ema_shift") == 0) {
		struct tieredvol_ctx *ctx = ti->private;
		u32 shift;

		if (kstrtou32(argv[1], 10, &shift) || shift > 10)
			return -EINVAL;
		ctx->ema_weight_shift = shift;
		pr_info("tieredvol: ema_weight_shift=%u (alpha=%u/1024)\n",
			shift, 1 << shift);
		tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG, "ema_shift=%u", shift);
		tv_metadata_save_kernel(ctx);
		return 0;
	}
	if (argc == 2 && strcmp(argv[0], "set_stale_ms") == 0) {
		struct tieredvol_ctx *ctx = ti->private;
		u32 ms;

		if (kstrtou32(argv[1], 10, &ms))
			return -EINVAL;
		ctx->stale_after_ns = (u64)ms * 1000000ULL;
		pr_info("tieredvol: stale_after=%ums\n", ms);
		tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG, "stale_ms=%u", ms);
		tv_metadata_save_kernel(ctx);
		return 0;
	}
	if (argc == 1 && strcmp(argv[0], "show_adaptive") == 0) {
		struct tieredvol_ctx *ctx = ti->private;
		int i, off = 0;

		off += snprintf(result + off, maxlen - off,
				"policy=%d ema_shift=%u stale_ms=%llu wear_bias=%u",
				ctx->policy,
				ctx->ema_weight_shift,
				ctx->stale_after_ns / 1000000ULL,
				ctx->wear_bias);
		for (i = 0; i < ctx->ndisks && off < (int)maxlen - 1; i++) {
			off += snprintf(result + off, maxlen - off,
					" %s:load=%llu writes=%llu stale=%d",
					ctx->meta.disk_names[i],
					ctx->ema_load[i],
					ctx->total_write_bytes[i],
					ctx->stale[i]);
		}
		pr_info("tieredvol: %s\n", result);
		return 0;
	}
	if (argc == 1 && strcmp(argv[0], "show_wear") == 0) {
		struct tieredvol_ctx *ctx = ti->private;
		int i, off = 0;

		off += snprintf(result + off, maxlen - off,
				"wear_bias=%u", ctx->wear_bias);
		for (i = 0; i < ctx->ndisks && off < (int)maxlen - 2; i++) {
			off += snprintf(result + off, maxlen - off,
					" %s=%llu",
					ctx->meta.disk_names[i],
					ctx->total_write_bytes[i]);
		}
		pr_info("tieredvol: %s\n", result);
		return 0;
	}
	if (argc == 1 && strcmp(argv[0], "show_io_stats") == 0) {
		struct tieredvol_ctx *ctx = ti->private;
		int i, off = 0;

		for (i = 0; i < ctx->ndisks && off < (int)maxlen - 2; i++) {
			off += snprintf(result + off, maxlen - off,
					"%s%s:rd=%llu/%llu wr=%llu/%llu",
					i > 0 ? " " : "",
					ctx->meta.disk_names[i],
					ctx->total_read_ops[i],
					ctx->total_read_bytes[i],
					ctx->total_write_ops[i],
					ctx->total_write_bytes[i]);
		}
		pr_info("tieredvol: %s\n", result);
		return 0;
	}
	if (argc == 1 && strcmp(argv[0], "reset_io_stats") == 0) {
		struct tieredvol_ctx *ctx = ti->private;
		int i;

		for (i = 0; i < ctx->ndisks; i++) {
			ctx->total_read_bytes[i] = 0;
			ctx->total_write_bytes[i] = 0;
			ctx->total_read_ops[i] = 0;
			ctx->total_write_ops[i] = 0;
		}
		pr_info("tieredvol: IO stats reset\n");
		return 0;
	}
	if (argc == 2 && strcmp(argv[0], "set_wear_bias") == 0) {
		struct tieredvol_ctx *ctx = ti->private;
		u32 bias;

		if (kstrtou32(argv[1], 10, &bias) || bias > 1024)
			return -EINVAL;
		ctx->wear_bias = bias;
		pr_info("tieredvol: wear_bias=%u\n", bias);
		tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG, "wear_bias=%u", bias);
		tv_metadata_save_kernel(ctx);
		return 0;
	}
	if (argc == 1 && strcmp(argv[0], "reset_wear") == 0) {
		struct tieredvol_ctx *ctx = ti->private;
		int i;

		for (i = 0; i < ctx->ndisks; i++)
			ctx->total_write_bytes[i] = 0;
		pr_info("tieredvol: wear counters reset\n");
		tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG, "wear reset");
		return 0;
	}
	if (argc == 1 && strcmp(argv[0], "show_mirror") == 0) {
		struct tieredvol_ctx *ctx = ti->private;
		int i, off = 0;

		off += snprintf(result + off, maxlen - off,
				"mirror_wr=%llu/%llu mirror_err=%llu",
				ctx->mirror_write_ops,
				ctx->mirror_write_bytes,
				ctx->mirror_errors);
		for (i = 0; i < (int)ctx->meta.segment_count &&
		     off < (int)maxlen - 2; i++) {
			struct tieredvol_segment *seg =
				&ctx->meta.segments[i];

			off += snprintf(result + off, maxlen - off,
					" seg%d:mirror=%s%d",
					i,
					seg->mirror_enabled ? "" : "off",
					seg->mirror_enabled ?
					(int)seg->mirror_disk : 0);
		}
		pr_info("tieredvol: %s\n", result);
		return 0;
	}
	if (argc == 3 && strcmp(argv[0], "set_mirror") == 0) {
		struct tieredvol_ctx *ctx = ti->private;
		u32 seg_idx, disk_idx;

		if (kstrtou32(argv[1], 10, &seg_idx) ||
		    kstrtou32(argv[2], 10, &disk_idx) ||
		    seg_idx >= ctx->meta.segment_count ||
		    disk_idx >= (u32)ctx->ndisks)
			return -EINVAL;
		ctx->meta.segments[seg_idx].mirror_enabled = true;
		ctx->meta.segments[seg_idx].mirror_disk = disk_idx;
		pr_info("tieredvol: seg%u mirror -> disk%u (%s)\n",
			seg_idx, disk_idx, ctx->meta.disk_names[disk_idx]);
		tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG,
		       "mirror seg%u->disk%u", seg_idx, disk_idx);
		tv_metadata_save_kernel(ctx);
		return 0;
	}
	if (argc == 1 && strcmp(argv[0], "show_log") == 0) {
		struct tv_log_entry entry;
		unsigned long flags;
		int cnt = 0;

		spin_lock_irqsave(&tv_log_lock, flags);
		while (kfifo_out(&tv_log_fifo, &entry, sizeof(entry))) {
			pr_info("tieredvol: LOG %s %s: %s\n",
				entry.level == TV_LOG_ERR ? "ERR" :
				entry.level == TV_LOG_WARN ? "WRN" : "INF",
				entry.event_type == TV_LOG_STALE ? "STALE" :
				entry.event_type == TV_LOG_RECOVER ? "RCVR" :
				entry.event_type == TV_LOG_MIRROR ? "MIRR" :
				entry.event_type == TV_LOG_CONFIG ? "CONF" :
				entry.event_type == TV_LOG_IO ? "I/O" : "???",
				entry.msg);
			cnt++;
		}
		spin_unlock_irqrestore(&tv_log_lock, flags);

		if (cnt == 0)
			pr_info("tieredvol: LOG EMPTY\n");
		else
			pr_info("tieredvol: LOG DUMPED %d entries\n", cnt);
		return 0;
	}
	if (argc == 1 && strcmp(argv[0], "clear_log") == 0) {
		unsigned long flags;

		spin_lock_irqsave(&tv_log_lock, flags);
		kfifo_reset(&tv_log_fifo);
		spin_unlock_irqrestore(&tv_log_lock, flags);
		pr_info("tieredvol: log cleared\n");
		return 0;
	}
	if (argc == 2 && strcmp(argv[0], "set_loglevel") == 0) {
		u32 lvl;

		if (kstrtou32(argv[1], 10, &lvl) || lvl > TV_LOG_INFO)
			return -EINVAL;
		tv_log_level = lvl;
		pr_info("tieredvol: loglevel = %u\n", tv_log_level);
		return 0;
	}
	if (argc == 1 && strcmp(argv[0], "show_errors") == 0) {
		struct tieredvol_ctx *ctx = ti->private;
		int i, off = 0;

		for (i = 0; i < ctx->ndisks && off < (int)maxlen - 2; i++) {
			off += snprintf(result + off, maxlen - off,
					"%s%s=%d", i > 0 ? " " : "",
					ctx->meta.disk_names[i],
					atomic_read(&ctx->error_count[i]));
		}
		return 0;
	}
	if (argc == 1 && strcmp(argv[0], "reset_errors") == 0) {
		struct tieredvol_ctx *ctx = ti->private;
		int i;

		for (i = 0; i < ctx->ndisks; i++)
			atomic_set(&ctx->error_count[i], 0);
		pr_info("tieredvol: error counts reset\n");
		tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG, "errors reset");
		return 0;
	}
	if (argc == 2 && strcmp(argv[0], "set_error_threshold") == 0) {
		struct tieredvol_ctx *ctx = ti->private;
		u32 thresh;

		if (kstrtou32(argv[1], 10, &thresh) || thresh == 0)
			return -EINVAL;
		ctx->error_threshold = thresh;
		pr_info("tieredvol: error_threshold=%u\n", thresh);
		tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG, "err_thresh=%u", thresh);
		return 0;
	}
	if (argc == 1 && strcmp(argv[0], "show_degraded") == 0) {
		struct tieredvol_ctx *ctx = ti->private;
		int i, off = 0;

		for (i = 0; i < ctx->ndisks && off < (int)maxlen - 2; i++) {
			off += snprintf(result + off, maxlen - off,
					"%s%s=%c(err=%d)",
					i > 0 ? " " : "",
					ctx->meta.disk_names[i],
					ctx->degraded[i] ? 'D' : 'A',
					atomic_read(&ctx->error_count[i]));
		}
		return 0;
	}
	if (argc == 1 && strcmp(argv[0], "clear_degraded") == 0) {
		struct tieredvol_ctx *ctx = ti->private;
		int i, cleared = 0;

		for (i = 0; i < ctx->ndisks; i++) {
			if (ctx->degraded[i]) {
				ctx->degraded[i] = false;
				atomic_set(&ctx->error_count[i], 0);
				cleared++;
				pr_info("tieredvol: disk[%d] %s cleared from DEGRADED\n",
					i, ctx->meta.disk_names[i]);
				tv_log(TV_LOG_INFO, i, TV_LOG_IO, "CLEARED degraded");
			}
		}
		snprintf(result, maxlen, "%d disk(s) cleared", cleared);
		return 0;
	}
	return -EINVAL;
}

static struct target_type tieredvol_target = {
	.name   = "tieredvol",
	.version = {2, 0, 0},
	.module = THIS_MODULE,
	.features = DM_TARGET_NOWAIT | DM_TARGET_PASSES_CRYPTO,
	.ctr    = tieredvol_ctr,
	.dtr    = tieredvol_dtr,
	.map    = tieredvol_map,
	.end_io = tieredvol_end_io,
	.status = tieredvol_status,
	.message = tieredvol_message,
	.prepare_ioctl = tieredvol_prepare_ioctl,
	.io_hints = tieredvol_io_hints,
	.iterate_devices = tieredvol_iterate_devices,
};

static int __init tieredvol_init(void)
{
	int ret;

	/* Allocate log ring buffer (round up to power of 2) */
	if (log_size == 0)
		log_size = TV_LOG_SIZE;
	if (!is_power_of_2(log_size))
		log_size = roundup_pow_of_two(log_size);

	ret = kfifo_alloc(&tv_log_fifo,
			  log_size * sizeof(struct tv_log_entry),
			  GFP_KERNEL);
	if (ret) {
		pr_err("tieredvol: kfifo alloc failed (%u entries)\n", log_size);
		return ret;
	}

	ret = dm_register_target(&tieredvol_target);
	if (ret < 0) {
		pr_err("tieredvol: registration failed: %d\n", ret);
		kfifo_free(&tv_log_fifo);
		return ret;
	}

	tv_wq = alloc_workqueue("tieredvol_wq", WQ_UNBOUND | WQ_HIGHPRI, 0);
	if (!tv_wq) {
		dm_unregister_target(&tieredvol_target);
		kfifo_free(&tv_log_fifo);
		pr_err("tieredvol: workqueue alloc failed\n");
		return -ENOMEM;
	}

	tv_sysfs_init();
	pr_info("tieredvol: module loaded (log_size=%u)\n", log_size);
	return 0;
}

/* ===== sysfs attributes (4a) ===== */

static ssize_t policy_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	struct tieredvol_ctx *ctx = tv_active_ctx;

	if (!ctx)
		return -ENODEV;
	return sysfs_emit(buf, "%s\n",
			  ctx->policy == TV_POLICY_ADAPTIVE ? "adaptive" :
			  ctx->policy == TV_POLICY_RANDOM ? "random" : "static");
}

static ssize_t stale_ms_show(struct kobject *kobj, struct kobj_attribute *attr,
			     char *buf)
{
	struct tieredvol_ctx *ctx = tv_active_ctx;

	if (!ctx)
		return -ENODEV;
	return sysfs_emit(buf, "%llu\n", ctx->stale_after_ns / 1000000ULL);
}

static ssize_t stale_ms_store(struct kobject *kobj, struct kobj_attribute *attr,
			      const char *buf, size_t count)
{
	struct tieredvol_ctx *ctx = tv_active_ctx;
	u32 ms;

	if (!ctx)
		return -ENODEV;
	if (kstrtou32(buf, 10, &ms))
		return -EINVAL;
	ctx->stale_after_ns = (u64)ms * 1000000ULL;
	return count;
}

static ssize_t wear_bias_show(struct kobject *kobj, struct kobj_attribute *attr,
			      char *buf)
{
	struct tieredvol_ctx *ctx = tv_active_ctx;

	if (!ctx)
		return -ENODEV;
	return sysfs_emit(buf, "%u\n", ctx->wear_bias);
}

static ssize_t wear_bias_store(struct kobject *kobj, struct kobj_attribute *attr,
			       const char *buf, size_t count)
{
	struct tieredvol_ctx *ctx = tv_active_ctx;
	u32 bias;

	if (!ctx)
		return -ENODEV;
	if (kstrtou32(buf, 10, &bias) || bias > 1024)
		return -EINVAL;
	ctx->wear_bias = bias;
	return count;
}

static ssize_t ema_shift_show(struct kobject *kobj, struct kobj_attribute *attr,
			      char *buf)
{
	struct tieredvol_ctx *ctx = tv_active_ctx;

	if (!ctx)
		return -ENODEV;
	return sysfs_emit(buf, "%u\n", ctx->ema_weight_shift);
}

static ssize_t ema_shift_store(struct kobject *kobj, struct kobj_attribute *attr,
			       const char *buf, size_t count)
{
	struct tieredvol_ctx *ctx = tv_active_ctx;
	u32 shift;

	if (!ctx)
		return -ENODEV;
	if (kstrtou32(buf, 10, &shift) || shift > 10)
		return -EINVAL;
	ctx->ema_weight_shift = shift;
	return count;
}

static ssize_t loglevel_show(struct kobject *kobj, struct kobj_attribute *attr,
			     char *buf)
{
	return sysfs_emit(buf, "%u\n", tv_log_level);
}

static ssize_t loglevel_store(struct kobject *kobj, struct kobj_attribute *attr,
			      const char *buf, size_t count)
{
	u32 lvl;

	if (kstrtou32(buf, 10, &lvl) || lvl > TV_LOG_INFO)
		return -EINVAL;
	tv_log_level = lvl;
	return count;
}

static ssize_t disk_count_show(struct kobject *kobj, struct kobj_attribute *attr,
			       char *buf)
{
	struct tieredvol_ctx *ctx = tv_active_ctx;

	if (!ctx)
		return -ENODEV;
	return sysfs_emit(buf, "%d\n", ctx->ndisks);
}

static ssize_t status_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	struct tieredvol_ctx *ctx = tv_active_ctx;
	int i, off = 0;

	if (!ctx)
		return -ENODEV;

	off += sysfs_emit_at(buf, off,
			     "policy=%d mirror=%llu/%llu err=%llu\n",
			     ctx->policy,
			     ctx->mirror_write_ops,
			     ctx->mirror_write_bytes,
			     ctx->mirror_errors);

	for (i = 0; i < ctx->ndisks; i++) {
		off += sysfs_emit_at(buf, off,
				     "%s: err=%d %s rd=%llu/%llu wr=%llu/%llu stale=%d ema=%llu\n",
				     ctx->meta.disk_names[i],
				     atomic_read(&ctx->error_count[i]),
				     ctx->degraded[i] ? "DEGRADED" : "active",
				     ctx->total_read_ops[i],
				     ctx->total_read_bytes[i],
				     ctx->total_write_ops[i],
				     ctx->total_write_bytes[i],
				     ctx->stale[i],
				     ctx->ema_load[i]);
	}
	return off;
}

static struct kobj_attribute policy_attr = __ATTR_RO(policy);
static struct kobj_attribute stale_ms_attr = __ATTR_RW(stale_ms);
static struct kobj_attribute wear_bias_attr = __ATTR_RW(wear_bias);
static struct kobj_attribute ema_shift_attr = __ATTR_RW(ema_shift);
static struct kobj_attribute loglevel_attr = __ATTR_RW(loglevel);
static struct kobj_attribute disk_count_attr = __ATTR_RO(disk_count);
static struct kobj_attribute status_attr = __ATTR_RO(status);

static struct attribute *tv_attrs[] = {
	&policy_attr.attr,
	&stale_ms_attr.attr,
	&wear_bias_attr.attr,
	&ema_shift_attr.attr,
	&loglevel_attr.attr,
	&disk_count_attr.attr,
	&status_attr.attr,
	NULL,
};

static struct attribute_group tv_attr_group = {
	.attrs = tv_attrs,
};

static void tv_sysfs_init(void)
{
	tv_kobj = kobject_create_and_add("tieredvol", kernel_kobj);
	if (!tv_kobj) {
		pr_err("tieredvol: sysfs init failed\n");
		return;
	}
	if (sysfs_create_group(tv_kobj, &tv_attr_group)) {
		pr_err("tieredvol: sysfs group create failed\n");
		kobject_put(tv_kobj);
		tv_kobj = NULL;
	}
}

static void tv_sysfs_exit(void)
{
	if (tv_kobj) {
		sysfs_remove_group(tv_kobj, &tv_attr_group);
		kobject_put(tv_kobj);
		tv_kobj = NULL;
	}
}

static void __exit tieredvol_exit(void)
{
	tv_sysfs_exit();
	dm_unregister_target(&tieredvol_target);
	destroy_workqueue(tv_wq);
	kfifo_free(&tv_log_fifo);
	pr_info("tieredvol: module unloaded\n");
}

module_init(tieredvol_init);
module_exit(tieredvol_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("TieredVol");
MODULE_DESCRIPTION("Weighted striped dm target for tiered storage");
MODULE_VERSION("4.6.0");
