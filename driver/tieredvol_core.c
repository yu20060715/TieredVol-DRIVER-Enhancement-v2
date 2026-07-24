#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/device-mapper.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/percpu.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include "tieredvol.h"

#define DM_MSG_PREFIX "tieredvol"

/*
 * Per-disk load tracking via map()-only counters with time-decay.
 * We cannot reliably identify which disk a bio belongs to in end_io()
 * because DM strips bi_opf and restores bi_bdev. Instead, we track
 * "bytes recently sent" per disk and decay them over time.
 * This is an approximation good enough for load-balancing decisions.
 */

static struct workqueue_struct *tv_wq;

static DEFINE_PER_CPU(u64, tv_map_count);
static DEFINE_PER_CPU(u64, tv_map_sectors);
static DEFINE_PER_CPU(u64, tv_map_bytes);

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
		} else if (ctx->stale[i] && snapshot > 0) {
			ctx->stale[i] = false;
			ctx->grace_until_ns[i] = now + ctx->stale_after_ns;
			pr_info("tieredvol: disk[%d] %s RECOVERED (I/O resumed)\n",
				i, ctx->meta.disk_names[i]);
		} else if (ctx->stale[i] &&
			   (now - ctx->stale_marked_ns[i]) >
			   2 * ctx->stale_after_ns) {
			ctx->stale[i] = false;
			ctx->grace_until_ns[i] = now + ctx->stale_after_ns;
			pr_info("tieredvol: disk[%d] %s RECOVERED (cooldown)\n",
				i, ctx->meta.disk_names[i]);
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

	bio_put(bio);
}

static int tieredvol_map(struct dm_target *ti, struct bio *bio)
{
	struct tieredvol_ctx *ctx = ti->private;
	u64 logical;
	struct tieredvol_map cur;
	int ret;

	logical = (u64)bio->bi_iter.bi_sector << SECTOR_SHIFT;

	switch (ctx->policy) {
	case TV_POLICY_ADAPTIVE:
		cur = tv_map_logical_adaptive(logical, &ctx->meta,
					      ctx->ema_load, ctx->stale,
					      ctx->ndisks,
					      ctx->total_write_bytes,
					      ctx->wear_bias);
		break;
	case TV_POLICY_RANDOM:
		cur = tv_map_logical_random(logical, &ctx->meta);
		break;
	case TV_POLICY_STATIC:
	default:
		cur = tv_map_logical(logical, &ctx->meta);
		break;
	}

	if (cur.disk < 0 || cur.disk >= ctx->ndisks) {
		pr_err("tieredvol: map failed for sector %llu\n",
		       (unsigned long long)bio->bi_iter.bi_sector);
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
						bio, GFP_NOIO, NULL);
			if (clone) {
				clone->bi_iter.bi_sector = cur.offset >> SECTOR_SHIFT;
				clone->bi_private = ctx;
				clone->bi_end_io = tv_mirror_end_io;
				ctx->mirror_write_bytes += bio->bi_iter.bi_size;
				submit_bio(clone);
			} else {
				ctx->mirror_errors++;
			}
		}
	}

	return DM_MAPIO_REMAPPED;
}

static int tieredvol_end_io(struct dm_target *ti, struct bio *bio,
			    blk_status_t *error)
{
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
	ctx->ema_weight_shift = 3;
	ctx->stale_after_ns = 5000000000ULL;
	ctx->wear_bias = 0;
	ctx->policy = TV_POLICY_STATIC;
	ctx->mirror_write_bytes = 0;
	ctx->mirror_write_ops = 0;
	ctx->mirror_errors = 0;

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

	/* Compute min_chunk_sectors and stripe_sectors across all segments.
	 * min_chunk = min over all segments of (min weight * TV_CHUNK_SIZE).
	 * dm_set_target_max_io_len() ensures dm core splits bios so they
	 * never cross a disk boundary within a stripe.
	 */
	{
		sector_t global_min_chunk = (sector_t)-1;
		sector_t max_stripe = 0;
		u32 si, j;

		for (si = 0; si < ctx->meta.segment_count; si++) {
			struct tieredvol_segment *seg = &ctx->meta.segments[si];
			sector_t seg_min;

			if (seg->disk_count == 0)
				continue;

			seg_min = (sector_t)seg->weight[0] * (TV_CHUNK_SIZE >> SECTOR_SHIFT);
			for (j = 1; j < seg->disk_count; j++) {
				sector_t w = (sector_t)seg->weight[j] *
					     (TV_CHUNK_SIZE >> SECTOR_SHIFT);

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
			char status = 'A';

			if (ctx->error_count &&
			    atomic_read(&ctx->error_count[i]))
				status = 'D';

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
		return 0;
	}
	if (argc == 1 && strcmp(argv[0], "adaptive_off") == 0) {
		struct tieredvol_ctx *ctx = ti->private;

		ctx->policy = TV_POLICY_STATIC;
		pr_info("tieredvol: policy = static\n");
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
		return 0;
	}
	if (argc == 1 && strcmp(argv[0], "set_ema_shift") == 0) {
		struct tieredvol_ctx *ctx = ti->private;
		u32 shift;

		if (kstrtou32(argv[1], 10, &shift) || shift > 10)
			return -EINVAL;
		ctx->ema_weight_shift = shift;
		pr_info("tieredvol: ema_weight_shift=%u (alpha=%u/1024)\n",
			shift, 1 << shift);
		return 0;
	}
	if (argc == 2 && strcmp(argv[0], "set_stale_ms") == 0) {
		struct tieredvol_ctx *ctx = ti->private;
		u32 ms;

		if (kstrtou32(argv[1], 10, &ms))
			return -EINVAL;
		ctx->stale_after_ns = (u64)ms * 1000000ULL;
		pr_info("tieredvol: stale_after=%ums\n", ms);
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
		return 0;
	}
	if (argc == 1 && strcmp(argv[0], "reset_wear") == 0) {
		struct tieredvol_ctx *ctx = ti->private;
		int i;

		for (i = 0; i < ctx->ndisks; i++)
			ctx->total_write_bytes[i] = 0;
		pr_info("tieredvol: wear counters reset\n");
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
		return 0;
	}
	return -EINVAL;
}

static struct target_type tieredvol_target = {
	.name   = "tieredvol",
	.version = {2, 0, 0},
	.module = THIS_MODULE,
	.features = DM_TARGET_NOWAIT | DM_TARGET_PASSES_INTEGRITY |
		    DM_TARGET_ATOMIC_WRITES | DM_TARGET_PASSES_CRYPTO,
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

	ret = dm_register_target(&tieredvol_target);
	if (ret < 0) {
		pr_err("tieredvol: registration failed: %d\n", ret);
		return ret;
	}

	tv_wq = alloc_workqueue("tieredvol_wq", WQ_UNBOUND | WQ_HIGHPRI, 0);
	if (!tv_wq) {
		dm_unregister_target(&tieredvol_target);
		pr_err("tieredvol: workqueue alloc failed\n");
		return -ENOMEM;
	}

	pr_info("tieredvol: module loaded\n");
	return 0;
}

static void __exit tieredvol_exit(void)
{
	dm_unregister_target(&tieredvol_target);
	destroy_workqueue(tv_wq);
	pr_info("tieredvol: module unloaded\n");
}

module_init(tieredvol_init);
module_exit(tieredvol_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("TieredVol");
MODULE_DESCRIPTION("Weighted striped dm target for tiered storage");
MODULE_VERSION("4.6.0");
