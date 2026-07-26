// SPDX-License-Identifier: GPL-2.0-only
/*
 * tieredvol_core.c — DM lifecycle: ctr/dtr/map/status/io_hints/ioctl/iterate,
 * trigger_event, module init/exit.
 *
 * Trimmed from the original 1843-line monolith in Phase 1 refactoring.
 * Log, mirror, sysfs, message handlers moved to separate files.
 */
#include <linux/module.h>
#include <linux/device-mapper.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/kthread.h>
#include <linux/kfifo.h>
#include "tieredvol.h"

#define DM_MSG_PREFIX "tieredvol"

struct tieredvol_ctx *tv_active_ctx;
EXPORT_SYMBOL_GPL(tv_active_ctx);

struct workqueue_struct *tv_wq;
EXPORT_SYMBOL_GPL(tv_wq);

static void trigger_event(struct work_struct *work)
{
	struct tieredvol_ctx *ctx = container_of(work, struct tieredvol_ctx,
						 trigger_event);
	dm_table_event(ctx->ti->table);
}

static int tieredvol_map(struct dm_target *ti, struct bio *bio)
{
	struct tieredvol_ctx *ctx = ti->private;
	u64 logical;
	struct tieredvol_map cur;

	logical = (u64)bio->bi_iter.bi_sector << TV_SECTOR_SHIFT;

	switch (ctx->adaptive.policy) {
	case TV_POLICY_ADAPTIVE:
		cur = tv_map_logical_adaptive(logical, &ctx->meta,
					      ctx->adaptive.ema_load,
					      ctx->adaptive.stale,
					      ctx->deg.degraded,
					      ctx->ndisks,
					      ctx->io.total_write_bytes,
					      ctx->adaptive.wear_bias,
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
	bio->bi_iter.bi_sector = cur.offset >> TV_SECTOR_SHIFT;
	atomic_add(bio->bi_iter.bi_size, &ctx->io.in_flight_bytes[cur.disk]);
	if (bio_data_dir(bio) == WRITE) {
		ctx->io.total_write_bytes[cur.disk] += bio->bi_iter.bi_size;
		ctx->io.total_write_ops[cur.disk]++;
	} else {
		ctx->io.total_read_bytes[cur.disk] += bio->bi_iter.bi_size;
		ctx->io.total_read_ops[cur.disk]++;
	}

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

			clone = bio_alloc_clone(
				ctx->devs[seg->mirror_disk]->bdev, bio,
				GFP_NOIO, &fs_bio_set);
			if (clone) {
				clone->bi_iter.bi_sector =
					(logical - seg->logical_begin) >>
					TV_SECTOR_SHIFT;
				clone->bi_private = ctx;
				clone->bi_end_io = tv_mirror_end_io;
				ctx->mirror.mirror_write_bytes +=
					bio->bi_iter.bi_size;
				tv_pw_add(ctx->devs[seg->mirror_disk]->bdev,
					  clone->bi_iter.bi_sector,
					  bio->bi_iter.bi_size);
				submit_bio(clone);
				tv_log(TV_LOG_INFO, cur.disk, TV_LOG_MIRROR,
				       "mirrored %uKB seg%d->disk%d",
				       bio->bi_iter.bi_size >> 10,
				       cur.seg_idx, seg->mirror_disk);
			} else {
				ctx->mirror.mirror_errors++;
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

static int tieredvol_ctr(struct dm_target *ti, unsigned int argc, char **argv)
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

	ctx->deg.error_count = kcalloc(ctx->ndisks, sizeof(atomic_t),
					GFP_KERNEL);
	if (!ctx->deg.error_count) {
		ti->error = "tieredvol: out of memory for error_count";
		ret = -ENOMEM;
		goto put_devices;
	}

	INIT_WORK(&ctx->trigger_event, trigger_event);

	ctx->adaptive_enabled = false;
	ctx->adaptive.policy = ctx->meta.runtime_policy;
	ctx->adaptive.ema_weight_shift = ctx->meta.runtime_ema_shift ?
						 ctx->meta.runtime_ema_shift :
						 3;
	ctx->adaptive.stale_after_ns = ctx->meta.runtime_stale_ms ?
					      (u64)ctx->meta.runtime_stale_ms *
						      1000000ULL :
					      5000000000ULL;
	ctx->adaptive.wear_bias = ctx->meta.runtime_wear_bias;
	ctx->mirror.mirror_write_bytes = 0;
	ctx->mirror.mirror_write_ops = 0;
	ctx->mirror.mirror_errors = 0;
	ctx->deg.error_threshold = 10;
	ctx->rebuild.thread = NULL;
	ctx->rebuild.seg_idx = -1;
	ctx->rebuild.offset = 0;
	ctx->rebuild.total = 0;
	atomic_set(&ctx->rebuild.running, 0);
	init_completion(&ctx->rebuild.done_r);
	init_completion(&ctx->rebuild.done_w);

	timer_setup(&ctx->adaptive.decay_timer, tv_decay_timer_fn, 0);
	mod_timer(&ctx->adaptive.decay_timer, jiffies + HZ);

	for (i = 0; i < ctx->ndisks; i++)
		pr_info("tieredvol: disk[%d] %s -> %pg (%llu sectors)\n",
			i, ctx->meta.disk_names[i], ctx->devs[i]->bdev,
			(unsigned long long)ctx->disk_sectors[i]);

	if (ctx->meta.segment_count == 0) {
		ti->error = "tieredvol: no segments";
		ret = -EINVAL;
		goto free_error_count;
	}

	/* Validate segments are sorted by logical_begin */
	for (i = 1; i < (int)ctx->meta.segment_count; i++) {
		if (ctx->meta.segments[i].logical_begin <
		    ctx->meta.segments[i - 1].logical_begin) {
			ti->error =
				"tieredvol: segments not sorted by logical_begin";
			ret = -EINVAL;
			goto free_error_count;
		}
	}

	/* Compute min_chunk_sectors and stripe_sectors */
	{
		sector_t global_min_chunk = (sector_t)-1;
		sector_t max_stripe = 0;
		u32 si, j;
		sector_t chunk_sectors = ctx->meta.chunk_size >> TV_SECTOR_SHIFT;

		for (si = 0; si < ctx->meta.segment_count; si++) {
			struct tieredvol_segment *seg =
				&ctx->meta.segments[si];
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

		if (global_min_chunk == (sector_t)-1 ||
		    global_min_chunk == 0) {
			ti->error = "tieredvol: invalid chunk geometry";
			ret = -EINVAL;
			goto free_error_count;
		}

		ctx->min_chunk_sectors = global_min_chunk;
		ctx->stripe_sectors = max_stripe >> TV_SECTOR_SHIFT;
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
	timer_delete_sync(&ctx->adaptive.decay_timer);
	kfree(ctx->deg.error_count);
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

	timer_delete_sync(&ctx->adaptive.decay_timer);
	flush_work(&ctx->trigger_event);

	if (atomic_read(&ctx->rebuild.running)) {
		atomic_set(&ctx->rebuild.running, 0);
		complete(&ctx->rebuild.done_r);
		complete(&ctx->rebuild.done_w);
		if (!IS_ERR_OR_NULL(ctx->rebuild.thread))
			kthread_stop(ctx->rebuild.thread);
	}

	kfree(ctx->deg.error_count);

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

static void tieredvol_io_hints(struct dm_target *ti, struct queue_limits *limits)
{
	struct tieredvol_ctx *ctx = ti->private;

	limits->logical_block_size = 512;
	limits->physical_block_size = 512;
	limits->chunk_sectors = ctx->min_chunk_sectors;
	limits->io_min = ctx->min_chunk_sectors;
	limits->io_opt = ctx->stripe_sectors;
}

static int tieredvol_iterate_devices(struct dm_target *ti,
				     iterate_devices_callout_fn fn, void *data)
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
				ctx->adaptive.policy,
				ctx->mirror.mirror_write_ops,
				ctx->mirror.mirror_write_bytes,
				ctx->mirror.mirror_errors);

		for (i = 0; i < ctx->ndisks && off < (int)maxlen - 2; i++) {
			char status;

			if (ctx->deg.degraded[i])
				status = 'D';
			else if (ctx->deg.error_count &&
				 atomic_read(&ctx->deg.error_count[i]))
				status = 'E';
			else
				status = 'A';

			off += snprintf(result + off, maxlen - off,
					" %c%s:rd=%llu/%llu wr=%llu/%llu",
					status, ctx->meta.disk_names[i],
					ctx->io.total_read_ops[i],
					ctx->io.total_read_bytes[i],
					ctx->io.total_write_ops[i],
					ctx->io.total_write_bytes[i]);
		}
		if (atomic_read(&ctx->rebuild.running))
			off += snprintf(result + off, maxlen - off,
					" rebuild=%d/%d",
					ctx->rebuild.seg_idx,
					(int)(ctx->rebuild.offset /
					      ctx->meta.chunk_size));
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
