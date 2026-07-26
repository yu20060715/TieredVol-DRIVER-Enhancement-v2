// SPDX-License-Identifier: GPL-2.0-only
/*
 * tieredvol_mirror.c — Mirror I/O, pending read/write tracking,
 * read retry, rebuild thread, DM end_io handler.
 *
 * Extracted from tieredvol_core.c in Phase 1 refactoring.
 */
#include <linux/module.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/device-mapper.h>
#include "tieredvol.h"

/* ---- Pending-read tracking (1c) ---- */

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

void tv_pending_add(struct block_device *bdev, sector_t sector,
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
EXPORT_SYMBOL_GPL(tv_pending_add);

int tv_pending_find_and_remove(struct block_device *bdev, sector_t sector,
			       unsigned int size)
{
	unsigned long flags;
	int mirror_disk = -1;
	unsigned int i;

	spin_lock_irqsave(&tv_pending_lock, flags);
	for (i = 0; i < tv_pending_count; i++) {
		unsigned int idx = (tv_pending_head + i) % TV_PENDING_MAX;
		struct tv_pending_read *pr = &tv_pending_reads[idx];

		if (pr->bdev == bdev && pr->sector == sector &&
		    pr->size == size) {
			unsigned int j;

			mirror_disk = pr->mirror_disk;
			for (j = i; j + 1 < tv_pending_count; j++) {
				unsigned int next =
					(tv_pending_head + j + 1) %
					TV_PENDING_MAX;

				tv_pending_reads[(tv_pending_head + j) %
						 TV_PENDING_MAX] =
					tv_pending_reads[next];
			}
			tv_pending_count--;
			break;
		}
	}
	spin_unlock_irqrestore(&tv_pending_lock, flags);
	return mirror_disk;
}
EXPORT_SYMBOL_GPL(tv_pending_find_and_remove);

/* ---- Pending-write tracking (1b) ---- */

struct tv_pending_write {
	struct block_device *bdev;
	sector_t sector;
	unsigned int size;
};

static struct tv_pending_write tv_pending_writes[TV_PENDING_MAX];
static unsigned int tv_pw_head;
static unsigned int tv_pw_count;
static DEFINE_SPINLOCK(tv_pw_lock);

void tv_pw_add(struct block_device *bdev, sector_t sector, unsigned int size)
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
EXPORT_SYMBOL_GPL(tv_pw_add);

static void tv_pw_remove(struct block_device *bdev, sector_t sector,
			  unsigned int size)
{
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&tv_pw_lock, flags);
	for (i = 0; i < tv_pw_count; i++) {
		unsigned int idx = (tv_pw_head + i) % TV_PENDING_MAX;
		struct tv_pending_write *pw = &tv_pending_writes[idx];

		if (pw->bdev == bdev && pw->sector == sector &&
		    pw->size == size) {
			unsigned int j;

			for (j = i; j + 1 < tv_pw_count; j++) {
				unsigned int next =
					(tv_pw_head + j + 1) % TV_PENDING_MAX;

				tv_pending_writes[(tv_pw_head + j) %
						 TV_PENDING_MAX] =
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

		if (pw->bdev == bdev && pw->sector == sector &&
		    pw->size == size) {
			found = true;
			break;
		}
	}
	spin_unlock_irqrestore(&tv_pw_lock, flags);
	return found;
}

/* ---- Mirror I/O completion ---- */

void tv_mirror_end_io(struct bio *bio)
{
	struct tieredvol_ctx *bio_ctx = bio->bi_private;

	if (bio->bi_status != BLK_STS_OK)
		bio_ctx->mirror.mirror_errors++;
	else
		bio_ctx->mirror.mirror_write_ops++;

	tv_pw_remove(bio->bi_bdev, bio->bi_iter.bi_sector, bio->bi_iter.bi_size);

	bio_put(bio);
}
EXPORT_SYMBOL_GPL(tv_mirror_end_io);

/* ---- Read retry work ---- */

static void tv_read_retry_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct tv_retry_ctx *rc =
		container_of(dwork, struct tv_retry_ctx, dwork);
	struct bio *clone;

	if (tv_pw_is_pending(rc->ctx->devs[rc->mirror_disk]->bdev,
			     rc->sector, rc->size)) {
		tv_log(TV_LOG_INFO, rc->mirror_disk, TV_LOG_MIRROR,
		       "retry delayed (mirror write pending) sec=%llu",
		       (u64)rc->sector);
		schedule_delayed_work(&rc->dwork, msecs_to_jiffies(1));
		return;
	}

	clone = bio_alloc(rc->ctx->devs[rc->mirror_disk]->bdev, 1,
			  REQ_OP_READ, GFP_NOIO);
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

/* ---- DM end_io handler ---- */

int tieredvol_end_io(struct dm_target *ti, struct bio *bio, blk_status_t *error)
{
	struct tieredvol_ctx *ctx = ti->private;

	if (bio->bi_status != BLK_STS_OK) {
		int i;

		for (i = 0; i < ctx->ndisks; i++) {
			if (bio->bi_bdev == ctx->devs[i]->bdev) {
				int errs;

				errs = atomic_inc_return(&ctx->deg.error_count[i]);
				tv_log(TV_LOG_ERR, i, TV_LOG_IO,
				       "I/O error on %s status=%d err=%d",
				       ctx->meta.disk_names[i],
				       bio->bi_status, errs);

				if (!ctx->deg.degraded[i] &&
				    errs >= (int)ctx->deg.error_threshold) {
					ctx->deg.degraded[i] = true;
					pr_warn("tieredvol: disk[%d] %s DEGRADED (errors=%d >= threshold=%u)\n",
						i, ctx->meta.disk_names[i],
						errs,
						ctx->deg.error_threshold);
					tv_log(TV_LOG_WARN, i, TV_LOG_IO,
					       "DEGRADED err=%d", errs);
					schedule_work(&ctx->trigger_event);
				}

				if (bio_data_dir(bio) == READ) {
					int mirror;

					mirror = tv_pending_find_and_remove(
						bio->bi_bdev,
						bio->bi_iter.bi_sector,
						bio->bi_iter.bi_size);

					if (mirror >= 0 &&
					    mirror < ctx->ndisks) {
						struct tv_retry_ctx *rc;

						rc = kmalloc(sizeof(*rc),
							     GFP_ATOMIC);
						if (rc) {
							INIT_DELAYED_WORK(
								&rc->dwork,
								tv_read_retry_work);
							rc->ctx = ctx;
							rc->sector =
								bio->bi_iter.bi_sector;
							rc->size =
								bio->bi_iter.bi_size;
							rc->mirror_disk =
								mirror;
							schedule_delayed_work(
								&rc->dwork, 0);
							tv_log(TV_LOG_INFO,
							       i,
							       TV_LOG_MIRROR,
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
EXPORT_SYMBOL_GPL(tieredvol_end_io);

/* ---- Rebuild thread (2c) ---- */

static void tv_rebuild_end_io(struct bio *bio)
{
	struct completion *done = bio->bi_private;

	complete(done);
}

int tv_rebuild_thread(void *data)
{
	struct tieredvol_ctx *ctx = data;
	struct tieredvol_segment *seg;
	struct bio *bio_r, *bio_w;
	unsigned int chunk_bytes;

	while (!kthread_should_stop()) {
		if (!atomic_read(&ctx->rebuild.running))
			break;

		seg = &ctx->meta.segments[ctx->rebuild.seg_idx];
		chunk_bytes = ctx->meta.chunk_size;

		if (ctx->rebuild.offset >= ctx->rebuild.total) {
			pr_info("tieredvol: rebuild seg%d complete (%llu bytes)\n",
				ctx->rebuild.seg_idx, ctx->rebuild.total);
			tv_log(TV_LOG_INFO, seg->mirror_disk, TV_LOG_MIRROR,
			       "rebuild seg%d complete %llu bytes",
			       ctx->rebuild.seg_idx, ctx->rebuild.total);
			atomic_set(&ctx->rebuild.running, 0);
			schedule_work(&ctx->trigger_event);
			break;
		}

		{
			struct page *pg;
			u64 logical_addr;
			struct tieredvol_map cur;

			logical_addr = ctx->rebuild.offset + seg->logical_begin;
			cur = tv_map_logical(logical_addr, &ctx->meta,
					     ctx->meta.chunk_size);
			if (cur.disk < 0 || cur.length == 0) {
				msleep(10);
				continue;
			}

			pg = alloc_page(GFP_NOIO);
			if (!pg) {
				msleep(10);
				continue;
			}

			reinit_completion(&ctx->rebuild.done_r);

			bio_r = bio_alloc(ctx->devs[cur.disk]->bdev, 1,
					  REQ_OP_READ, GFP_NOIO);
			if (!bio_r) {
				put_page(pg);
				msleep(10);
				continue;
			}
			bio_r->bi_iter.bi_sector = cur.offset >> TV_SECTOR_SHIFT;
			bio_r->bi_iter.bi_size = chunk_bytes;
			bio_r->bi_private = &ctx->rebuild.done_r;
			bio_r->bi_end_io = tv_rebuild_end_io;

			if (bio_add_page(bio_r, pg, chunk_bytes, 0) !=
			    chunk_bytes) {
				put_page(pg);
				bio_put(bio_r);
				msleep(10);
				continue;
			}

			submit_bio(bio_r);
			wait_for_completion(&ctx->rebuild.done_r);

			if (bio_r->bi_status != BLK_STS_OK) {
				pr_err("tieredvol: rebuild read failed at offset %llu\n",
				       ctx->rebuild.offset);
				put_page(pg);
				bio_put(bio_r);
				msleep(100);
				continue;
			}
			bio_put(bio_r);

			if (!atomic_read(&ctx->rebuild.running)) {
				put_page(pg);
				break;
			}

			reinit_completion(&ctx->rebuild.done_w);

			bio_w = bio_alloc(ctx->devs[seg->mirror_disk]->bdev, 1,
					  REQ_OP_WRITE, GFP_NOIO);
			if (!bio_w) {
				put_page(pg);
				msleep(10);
				continue;
			}
			bio_w->bi_iter.bi_sector =
				cur.offset >> TV_SECTOR_SHIFT;
			bio_w->bi_iter.bi_size = chunk_bytes;
			bio_w->bi_private = &ctx->rebuild.done_w;
			bio_w->bi_end_io = tv_rebuild_end_io;

			if (bio_add_page(bio_w, pg, chunk_bytes, 0) !=
			    chunk_bytes) {
				put_page(pg);
				bio_put(bio_w);
				msleep(10);
				continue;
			}

			submit_bio(bio_w);
			wait_for_completion(&ctx->rebuild.done_w);

			if (bio_w->bi_status != BLK_STS_OK) {
				pr_err("tieredvol: rebuild write failed at offset %llu\n",
				       ctx->rebuild.offset);
				put_page(pg);
				bio_put(bio_w);
				msleep(100);
				continue;
			}
			put_page(pg);
			bio_put(bio_w);
		}

		ctx->rebuild.offset += chunk_bytes;

		if ((ctx->rebuild.offset % (10 * 1024 * 1024)) == 0 ||
		    ctx->rebuild.offset >= ctx->rebuild.total) {
			u64 pct = ctx->rebuild.total ?
					  (ctx->rebuild.offset * 100 /
					   ctx->rebuild.total) :
					  0;

			pr_info("tieredvol: rebuild seg%d %llu/%llu (%llu%%)\n",
				ctx->rebuild.seg_idx,
				ctx->rebuild.offset, ctx->rebuild.total, pct);
		}

		cond_resched();
	}

	atomic_set(&ctx->rebuild.running, 0);
	return 0;
}
EXPORT_SYMBOL_GPL(tv_rebuild_thread);
