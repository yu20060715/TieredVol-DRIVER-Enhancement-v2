// SPDX-License-Identifier: GPL-2.0-only
/*
 * tieredvol_message.c — dmsetup message handler dispatch + metadata save
 *
 * Phase 3: Refactored from 440-line if/else chain into dispatch table
 * with 28 individual handler functions.
 */
#include <linux/module.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/crc32c.h>
#include <linux/kfifo.h>
#include <linux/kthread.h>
#include <linux/device-mapper.h>
#include "tieredvol.h"

/* ---- Metadata write-back (1d) ---- */

static int tv_metadata_save_kernel(struct tieredvol_ctx *ctx)
{
	struct file *f;
	char *buf;
	int off = 0;
	int ret;
	u32 crc;
	loff_t pos = 0;
	char bak_path[260];

	if (!ctx->config_path[0])
		return -ENOENT;

	buf = kmalloc(4096, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	off += scnprintf(buf + off, sizeof(buf) - off, "[weighted_striping]\n");
	off += scnprintf(buf + off, sizeof(buf) - off, "version=%u\n",
			  ctx->meta.version);
	off += scnprintf(buf + off, sizeof(buf) - off, "chunk_size=%u\n",
			  ctx->meta.chunk_size);
	off += scnprintf(buf + off, sizeof(buf) - off, "segment_count=%u\n",
			  ctx->meta.segment_count);
	off += scnprintf(buf + off, sizeof(buf) - off, "disk_count=%u\n",
			  ctx->meta.disk_count);

	for (u32 i = 0; i < ctx->meta.disk_count; i++)
		off += scnprintf(buf + off, sizeof(buf) - off,
				 "disk%u_name=%s\n", i, ctx->meta.disk_names[i]);

	for (u32 i = 0; i < ctx->meta.segment_count; i++) {
		struct tieredvol_segment *seg = &ctx->meta.segments[i];

		off += scnprintf(buf + off, sizeof(buf) - off,
				 "seg%u_begin=%llu\n", i, seg->logical_begin);
		off += scnprintf(buf + off, sizeof(buf) - off,
				 "seg%u_end=%llu\n", i, seg->logical_end);
		off += scnprintf(buf + off, sizeof(buf) - off,
				 "seg%u_count=%u\n", i, seg->disk_count);

		off += scnprintf(buf + off, sizeof(buf) - off, "seg%u_disks=",
				 i);
		for (u32 j = 0; j < seg->disk_count; j++)
			off += scnprintf(buf + off, sizeof(buf) - off,
					 "%s%u", j ? "," : "", seg->disk_index[j]);
		off += scnprintf(buf + off, sizeof(buf) - off, "\n");

		off += scnprintf(buf + off, sizeof(buf) - off, "seg%u_weight=",
				 i);
		for (u32 j = 0; j < seg->disk_count; j++)
			off += scnprintf(buf + off, sizeof(buf) - off,
					 "%s%u", j ? "," : "", seg->weight[j]);
		off += scnprintf(buf + off, sizeof(buf) - off, "\n");

		off += scnprintf(buf + off, sizeof(buf) - off,
				 "seg%u_stripe=%llu\n", i, seg->stripe_size);
		if (seg->mirror_enabled)
			off += scnprintf(buf + off, sizeof(buf) - off,
					 "seg%u_mirror=%u\n", i,
					 seg->mirror_disk);
	}

	off += scnprintf(buf + off, sizeof(buf) - off, "[runtime]\n");
	off += scnprintf(buf + off, sizeof(buf) - off, "policy=%d\n",
			  ctx->adaptive.policy);
	off += scnprintf(buf + off, sizeof(buf) - off, "stale_ms=%llu\n",
			  ctx->adaptive.stale_after_ns / 1000000ULL);
	off += scnprintf(buf + off, sizeof(buf) - off, "ema_shift=%u\n",
			  ctx->adaptive.ema_weight_shift);
	off += scnprintf(buf + off, sizeof(buf) - off, "wear_bias=%u\n",
			  ctx->adaptive.wear_bias);

	crc = crc32c(0, buf, off);
	off += scnprintf(buf + off, sizeof(buf) - off, "crc32=%u\n", crc);

	scnprintf(bak_path, sizeof(bak_path), "%s.bak", ctx->config_path);

	f = filp_open(ctx->config_path, O_RDONLY, 0);
	if (!IS_ERR(f)) {
		struct file *bak;

		bak = filp_open(bak_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (!IS_ERR(bak)) {
			char kbuf[256];
			loff_t rpos = 0, wpos = 0;
			ssize_t nrd;

			while ((nrd = kernel_read(f, kbuf, sizeof(kbuf),
						  &rpos)) > 0)
				kernel_write(bak, kbuf, nrd, &wpos);
			filp_close(bak, NULL);
		}
		filp_close(f, NULL);
	}

	f = filp_open(ctx->config_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (IS_ERR(f)) {
		pr_err("tieredvol: save failed to open %s: %ld\n",
		       ctx->config_path, PTR_ERR(f));
		kfree(buf);
		return PTR_ERR(f);
	}

	pos = 0;
	ret = kernel_write(f, buf, off, &pos);
	filp_close(f, NULL);

	if (ret != off) {
		pr_err("tieredvol: save write error %d (wrote %lld of %d)\n",
		       ret, pos, off);
		kfree(buf);
		return ret < 0 ? ret : -EIO;
	}

	tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG, "metadata saved crc=0x%08x",
	       crc);
	pr_info("tieredvol: metadata saved crc=0x%08x to %s\n", crc,
		ctx->config_path);
	kfree(buf);
	return 0;
}

/* ---- Message handler functions (Phase 3) ---- */

static int msg_reset_stats(struct dm_target *ti, unsigned int argc,
			   char **argv, char *result, unsigned int maxlen)
{
	tv_reset_stats();
	snprintf(result, maxlen, "stats reset");
	return 0;
}

static int msg_show_stats(struct dm_target *ti, unsigned int argc,
			  char **argv, char *result, unsigned int maxlen)
{
	u64 cnt = tv_read_count();
	u64 bytes = tv_read_bytes();
	u64 avg = cnt ? bytes / cnt : 0;

	pr_info("tieredvol: maps=%llu avg_bytes=%llu total_bytes=%llu",
		cnt, avg, bytes);
	snprintf(result, maxlen, "maps=%llu avg_bytes=%llu", cnt, avg);
	return 0;
}

static int msg_status(struct dm_target *ti, unsigned int argc,
		      char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i, off = 0;

	for (i = 0; i < ctx->ndisks && off < (int)maxlen - 1; i++) {
		u32 w = 0;
		int si;

		for (si = 0; si < (int)ctx->meta.segment_count; si++) {
			struct tieredvol_segment *seg =
				&ctx->meta.segments[si];
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

static int msg_show_inflight(struct dm_target *ti, unsigned int argc,
			     char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i, off = 0;

	for (i = 0; i < ctx->ndisks && off < (int)maxlen - 1; i++) {
		off += snprintf(result + off, maxlen - off,
				"%s%s=%u", i > 0 ? " " : "",
				ctx->meta.disk_names[i],
				atomic_read(&ctx->io.in_flight_bytes[i]));
	}
	pr_info("tieredvol: %s\n", result);
	return 0;
}

static int msg_adaptive_on(struct dm_target *ti, unsigned int argc,
			   char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;

	ctx->adaptive.policy = TV_POLICY_ADAPTIVE;
	pr_info("tieredvol: policy = adaptive\n");
	tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG, "policy=adaptive");
	tv_metadata_save_kernel(ctx);
	return 0;
}

static int msg_adaptive_off(struct dm_target *ti, unsigned int argc,
			    char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;

	ctx->adaptive.policy = TV_POLICY_STATIC;
	pr_info("tieredvol: policy = static\n");
	tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG, "policy=static");
	tv_metadata_save_kernel(ctx);
	return 0;
}

static int msg_set_policy(struct dm_target *ti, unsigned int argc,
			  char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;

	if (strcmp(argv[1], "static") == 0)
		ctx->adaptive.policy = TV_POLICY_STATIC;
	else if (strcmp(argv[1], "adaptive") == 0)
		ctx->adaptive.policy = TV_POLICY_ADAPTIVE;
	else if (strcmp(argv[1], "random") == 0)
		ctx->adaptive.policy = TV_POLICY_RANDOM;
	else
		return -EINVAL;
	pr_info("tieredvol: policy = %s\n", argv[1]);
	tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG, "policy=%s", argv[1]);
	tv_metadata_save_kernel(ctx);
	return 0;
}

static int msg_set_ema_shift(struct dm_target *ti, unsigned int argc,
			     char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	u32 shift;

	if (kstrtou32(argv[1], 10, &shift) || shift > 10)
		return -EINVAL;
	ctx->adaptive.ema_weight_shift = shift;
	pr_info("tieredvol: ema_weight_shift=%u (alpha=%u/1024)\n",
		shift, 1 << shift);
	tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG, "ema_shift=%u", shift);
	tv_metadata_save_kernel(ctx);
	return 0;
}

static int msg_set_stale_ms(struct dm_target *ti, unsigned int argc,
			    char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	u32 ms;

	if (kstrtou32(argv[1], 10, &ms))
		return -EINVAL;
	ctx->adaptive.stale_after_ns = (u64)ms * 1000000ULL;
	pr_info("tieredvol: stale_after=%ums\n", ms);
	tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG, "stale_ms=%u", ms);
	tv_metadata_save_kernel(ctx);
	return 0;
}

static int msg_show_adaptive(struct dm_target *ti, unsigned int argc,
			     char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i, off = 0;

	off += snprintf(result + off, maxlen - off,
			"policy=%d ema_shift=%u stale_ms=%llu wear_bias=%u",
			ctx->adaptive.policy,
			ctx->adaptive.ema_weight_shift,
			ctx->adaptive.stale_after_ns / 1000000ULL,
			ctx->adaptive.wear_bias);
	for (i = 0; i < ctx->ndisks && off < (int)maxlen - 1; i++) {
		off += snprintf(result + off, maxlen - off,
				" %s:load=%llu writes=%llu stale=%d",
				ctx->meta.disk_names[i],
				ctx->adaptive.ema_load[i],
				ctx->io.total_write_bytes[i],
				ctx->adaptive.stale[i]);
	}
	pr_info("tieredvol: %s\n", result);
	return 0;
}

static int msg_show_wear(struct dm_target *ti, unsigned int argc,
			 char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i, off = 0;

	off += snprintf(result + off, maxlen - off,
			"wear_bias=%u", ctx->adaptive.wear_bias);
	for (i = 0; i < ctx->ndisks && off < (int)maxlen - 2; i++) {
		off += snprintf(result + off, maxlen - off,
				" %s=%llu",
				ctx->meta.disk_names[i],
				ctx->io.total_write_bytes[i]);
	}
	pr_info("tieredvol: %s\n", result);
	return 0;
}

static int msg_show_io_stats(struct dm_target *ti, unsigned int argc,
			     char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i, off = 0;

	for (i = 0; i < ctx->ndisks && off < (int)maxlen - 2; i++) {
		off += snprintf(result + off, maxlen - off,
				"%s%s:rd=%llu/%llu wr=%llu/%llu",
				i > 0 ? " " : "",
				ctx->meta.disk_names[i],
				ctx->io.total_read_ops[i],
				ctx->io.total_read_bytes[i],
				ctx->io.total_write_ops[i],
				ctx->io.total_write_bytes[i]);
	}
	pr_info("tieredvol: %s\n", result);
	return 0;
}

static int msg_reset_io_stats(struct dm_target *ti, unsigned int argc,
			      char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i;

	for (i = 0; i < ctx->ndisks; i++) {
		ctx->io.total_read_bytes[i] = 0;
		ctx->io.total_write_bytes[i] = 0;
		ctx->io.total_read_ops[i] = 0;
		ctx->io.total_write_ops[i] = 0;
	}
	pr_info("tieredvol: IO stats reset\n");
	return 0;
}

static int msg_set_wear_bias(struct dm_target *ti, unsigned int argc,
			     char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	u32 bias;

	if (kstrtou32(argv[1], 10, &bias) || bias > 1024)
		return -EINVAL;
	ctx->adaptive.wear_bias = bias;
	pr_info("tieredvol: wear_bias=%u\n", bias);
	tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG, "wear_bias=%u", bias);
	tv_metadata_save_kernel(ctx);
	return 0;
}

static int msg_reset_wear(struct dm_target *ti, unsigned int argc,
			  char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i;

	for (i = 0; i < ctx->ndisks; i++)
		ctx->io.total_write_bytes[i] = 0;
	pr_info("tieredvol: wear counters reset\n");
	tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG, "wear reset");
	return 0;
}

static int msg_show_mirror(struct dm_target *ti, unsigned int argc,
			   char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i, off = 0;

	off += snprintf(result + off, maxlen - off,
			"mirror_wr=%llu/%llu mirror_err=%llu",
			ctx->mirror.mirror_write_ops,
			ctx->mirror.mirror_write_bytes,
			ctx->mirror.mirror_errors);
	for (i = 0; i < (int)ctx->meta.segment_count &&
		     off < (int)maxlen - 2; i++) {
		struct tieredvol_segment *seg = &ctx->meta.segments[i];

		off += snprintf(result + off, maxlen - off,
				" seg%d:mirror=%s%d", i,
				seg->mirror_enabled ? "" : "off",
				seg->mirror_enabled ? (int)seg->mirror_disk : 0);
	}
	pr_info("tieredvol: %s\n", result);
	return 0;
}

static int msg_set_mirror(struct dm_target *ti, unsigned int argc,
			  char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	u32 seg_idx, disk_idx;

	if (kstrtou32(argv[1], 10, &seg_idx) ||
	    kstrtou32(argv[2], 10, &disk_idx) ||
	    seg_idx >= ctx->meta.segment_count ||
	    disk_idx >= (u32)ctx->ndisks)
		return -EINVAL;
	ctx->meta.segments[seg_idx].mirror_enabled = true;
	ctx->meta.segments[seg_idx].mirror_disk = disk_idx;
	pr_info("tieredvol: seg%u mirror -> disk%u (%s)\n", seg_idx,
		disk_idx, ctx->meta.disk_names[disk_idx]);
	tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG, "mirror seg%u->disk%u",
	       seg_idx, disk_idx);
	tv_metadata_save_kernel(ctx);
	return 0;
}

static int msg_show_log(struct dm_target *ti, unsigned int argc,
			char **argv, char *result, unsigned int maxlen)
{
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

static int msg_clear_log(struct dm_target *ti, unsigned int argc,
			 char **argv, char *result, unsigned int maxlen)
{
	unsigned long flags;

	spin_lock_irqsave(&tv_log_lock, flags);
	kfifo_reset(&tv_log_fifo);
	spin_unlock_irqrestore(&tv_log_lock, flags);
	pr_info("tieredvol: log cleared\n");
	return 0;
}

static int msg_set_loglevel(struct dm_target *ti, unsigned int argc,
			    char **argv, char *result, unsigned int maxlen)
{
	u32 lvl;

	if (kstrtou32(argv[1], 10, &lvl) || lvl > TV_LOG_INFO)
		return -EINVAL;
	tv_log_level = lvl;
	pr_info("tieredvol: loglevel = %u\n", tv_log_level);
	return 0;
}

static int msg_show_errors(struct dm_target *ti, unsigned int argc,
			   char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i, off = 0;

	for (i = 0; i < ctx->ndisks && off < (int)maxlen - 2; i++) {
		off += snprintf(result + off, maxlen - off,
				"%s%s=%d", i > 0 ? " " : "",
				ctx->meta.disk_names[i],
				atomic_read(&ctx->deg.error_count[i]));
	}
	return 0;
}

static int msg_reset_errors(struct dm_target *ti, unsigned int argc,
			    char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i;

	for (i = 0; i < ctx->ndisks; i++)
		atomic_set(&ctx->deg.error_count[i], 0);
	pr_info("tieredvol: error counts reset\n");
	tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG, "errors reset");
	return 0;
}

static int msg_set_error_threshold(struct dm_target *ti, unsigned int argc,
				   char **argv, char *result,
				   unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	u32 thresh;

	if (kstrtou32(argv[1], 10, &thresh) || thresh == 0)
		return -EINVAL;
	ctx->deg.error_threshold = thresh;
	pr_info("tieredvol: error_threshold=%u\n", thresh);
	tv_log(TV_LOG_INFO, 0, TV_LOG_CONFIG, "err_thresh=%u", thresh);
	return 0;
}

static int msg_show_degraded(struct dm_target *ti, unsigned int argc,
			     char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i, off = 0;

	for (i = 0; i < ctx->ndisks && off < (int)maxlen - 2; i++) {
		off += snprintf(result + off, maxlen - off,
				"%s%s=%c(err=%d)", i > 0 ? " " : "",
				ctx->meta.disk_names[i],
				ctx->deg.degraded[i] ? 'D' : 'A',
				atomic_read(&ctx->deg.error_count[i]));
	}
	return 0;
}

static int msg_clear_degraded(struct dm_target *ti, unsigned int argc,
			      char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	int i, cleared = 0;

	for (i = 0; i < ctx->ndisks; i++) {
		if (ctx->deg.degraded[i]) {
			ctx->deg.degraded[i] = false;
			atomic_set(&ctx->deg.error_count[i], 0);
			cleared++;
			pr_info("tieredvol: disk[%d] %s cleared from DEGRADED\n",
				i, ctx->meta.disk_names[i]);
			tv_log(TV_LOG_INFO, i, TV_LOG_IO, "CLEARED degraded");
		}
	}
	snprintf(result, maxlen, "%d disk(s) cleared", cleared);
	return 0;
}

static int msg_start_rebuild(struct dm_target *ti, unsigned int argc,
			     char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;
	u32 seg_idx;
	u64 max_bytes = 0;

	if (kstrtou32(argv[1], 10, &seg_idx) ||
	    seg_idx >= ctx->meta.segment_count)
		return -EINVAL;
	if (argc >= 3) {
		if (kstrtou64(argv[2], 10, &max_bytes) || max_bytes == 0)
			return -EINVAL;
	}
	if (atomic_read(&ctx->rebuild.running))
		return -EBUSY;
	if (!ctx->meta.segments[seg_idx].mirror_enabled)
		return -EINVAL;

	ctx->rebuild.seg_idx = seg_idx;
	ctx->rebuild.offset = 0;
	ctx->rebuild.total =
		ctx->meta.segments[seg_idx].logical_end -
		ctx->meta.segments[seg_idx].logical_begin;
	if (max_bytes > 0 && max_bytes < ctx->rebuild.total)
		ctx->rebuild.total = max_bytes;
	atomic_set(&ctx->rebuild.running, 1);
	reinit_completion(&ctx->rebuild.done_r);
	reinit_completion(&ctx->rebuild.done_w);

	ctx->rebuild.thread = kthread_run(tv_rebuild_thread, ctx,
					  "tv_rebuild_%d", seg_idx);
	if (IS_ERR(ctx->rebuild.thread)) {
		atomic_set(&ctx->rebuild.running, 0);
		return PTR_ERR(ctx->rebuild.thread);
	}
	pr_info("tieredvol: rebuild started seg%u %llu bytes\n",
		seg_idx, ctx->rebuild.total);
	tv_log(TV_LOG_INFO, ctx->meta.segments[seg_idx].mirror_disk,
	       TV_LOG_MIRROR, "rebuild start seg%u %llu bytes",
	       seg_idx, ctx->rebuild.total);
	return 0;
}

static int msg_stop_rebuild(struct dm_target *ti, unsigned int argc,
			    char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;

	if (!atomic_read(&ctx->rebuild.running))
		return 0;
	atomic_set(&ctx->rebuild.running, 0);
	complete(&ctx->rebuild.done_r);
	complete(&ctx->rebuild.done_w);
	if (!IS_ERR_OR_NULL(ctx->rebuild.thread)) {
		kthread_stop(ctx->rebuild.thread);
		ctx->rebuild.thread = NULL;
	}
	pr_info("tieredvol: rebuild stopped at %llu/%llu\n",
		ctx->rebuild.offset, ctx->rebuild.total);
	tv_log(TV_LOG_WARN, 0, TV_LOG_MIRROR,
	       "rebuild stopped %llu/%llu",
	       ctx->rebuild.offset, ctx->rebuild.total);
	return 0;
}

static int msg_show_rebuild(struct dm_target *ti, unsigned int argc,
			    char **argv, char *result, unsigned int maxlen)
{
	struct tieredvol_ctx *ctx = ti->private;

	if (atomic_read(&ctx->rebuild.running)) {
		u64 pct = ctx->rebuild.total ?
				  (ctx->rebuild.offset * 100 /
				   ctx->rebuild.total) :
				  0;

		snprintf(result, maxlen,
			 "rebuilding seg%d %llu/%llu (%llu%%)",
			 ctx->rebuild.seg_idx,
			 ctx->rebuild.offset, ctx->rebuild.total, pct);
	} else {
		snprintf(result, maxlen, "idle");
	}
	return 0;
}

/* ---- Dispatch table ---- */

typedef int (*tv_msg_fn)(struct dm_target *ti, unsigned int argc,
			 char **argv, char *result, unsigned int maxlen);

struct tv_msg_handler {
	const char *name;
	int min_argc;
	int max_argc; /* 0 = unlimited */
	tv_msg_fn fn;
};

/* clang-format off */
static const struct tv_msg_handler tv_msg_handlers[] = {
	/* Stats */
	{ "reset_stats",      1, 1, msg_reset_stats },
	{ "show_stats",       1, 1, msg_show_stats },
	{ "status",           1, 1, msg_status },
	{ "show_inflight",    1, 1, msg_show_inflight },
	{ "show_io_stats",    1, 1, msg_show_io_stats },
	{ "reset_io_stats",   1, 1, msg_reset_io_stats },
	/* Adaptive / load balancing */
	{ "adaptive_on",      1, 1, msg_adaptive_on },
	{ "adaptive_off",     1, 1, msg_adaptive_off },
	{ "set_policy",       2, 2, msg_set_policy },
	{ "set_ema_shift",    2, 2, msg_set_ema_shift },
	{ "set_stale_ms",     2, 2, msg_set_stale_ms },
	{ "show_adaptive",    1, 1, msg_show_adaptive },
	/* Wear tracking */
	{ "show_wear",        1, 1, msg_show_wear },
	{ "set_wear_bias",    2, 2, msg_set_wear_bias },
	{ "reset_wear",       1, 1, msg_reset_wear },
	/* Mirror */
	{ "show_mirror",      1, 1, msg_show_mirror },
	{ "set_mirror",       3, 3, msg_set_mirror },
	/* Log */
	{ "show_log",         1, 1, msg_show_log },
	{ "clear_log",        1, 1, msg_clear_log },
	{ "set_loglevel",     2, 2, msg_set_loglevel },
	/* Error / degradation */
	{ "show_errors",      1, 1, msg_show_errors },
	{ "reset_errors",     1, 1, msg_reset_errors },
	{ "set_error_threshold", 2, 2, msg_set_error_threshold },
	{ "show_degraded",    1, 1, msg_show_degraded },
	{ "clear_degraded",   1, 1, msg_clear_degraded },
	/* Rebuild */
	{ "start_rebuild",    2, 0, msg_start_rebuild },
	{ "stop_rebuild",     1, 1, msg_stop_rebuild },
	{ "show_rebuild",     1, 1, msg_show_rebuild },
};
/* clang-format on */

#define TV_MSG_HANDLERS (sizeof(tv_msg_handlers) / sizeof(tv_msg_handlers[0]))

/* ---- Dispatch function ---- */

int tieredvol_message(struct dm_target *ti, unsigned int argc, char **argv,
		      char *result, unsigned int maxlen)
{
	unsigned int i;

	for (i = 0; i < TV_MSG_HANDLERS; i++) {
		const struct tv_msg_handler *h = &tv_msg_handlers[i];

		if (strcmp(argv[0], h->name) != 0)
			continue;
		if (argc < h->min_argc)
			return -EINVAL;
		if (h->max_argc > 0 && argc > h->max_argc)
			return -EINVAL;
		return h->fn(ti, argc, argv, result, maxlen);
	}

	return -EINVAL;
}
