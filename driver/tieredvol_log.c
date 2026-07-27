// SPDX-License-Identifier: GPL-2.0-only
/*
 * tieredvol_log.c — Log ring buffer, EMA load tracking, per-CPU counters
 *
 * Extracted from tieredvol_core.c in Phase 1 refactoring.
 */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/percpu.h>
#include <linux/spinlock.h>
#include <linux/kfifo.h>
#include <linux/stdarg.h>
#include <linux/ktime.h>
#include <linux/jiffies.h>
#include <linux/timer.h>
#include <linux/sprintf.h>
#include "tieredvol.h"

/* ---- Per-CPU map counters ---- */

static DEFINE_PER_CPU(u64, tv_map_count);
static DEFINE_PER_CPU(u64, tv_map_sectors);
static DEFINE_PER_CPU(u64, tv_map_bytes);

u64 tv_read_count(void)
{
	u64 total = 0;
	int cpu;

	for_each_possible_cpu(cpu)
		total += per_cpu(tv_map_count, cpu);
	return total;
}

u64 tv_read_sectors(void)
{
	u64 total = 0;
	int cpu;

	for_each_possible_cpu(cpu)
		total += per_cpu(tv_map_sectors, cpu);
	return total;
}

u64 tv_read_bytes(void)
{
	u64 total = 0;
	int cpu;

	for_each_possible_cpu(cpu)
		total += per_cpu(tv_map_bytes, cpu);
	return total;
}

void tv_reset_stats(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		per_cpu(tv_map_count, cpu) = 0;
		per_cpu(tv_map_sectors, cpu) = 0;
		per_cpu(tv_map_bytes, cpu) = 0;
	}
}
EXPORT_SYMBOL_GPL(tv_reset_stats);

/* ---- Log ring buffer ---- */

unsigned int log_size = TV_LOG_SIZE;
module_param(log_size, uint, 0644);
MODULE_PARM_DESC(log_size, "Ring buffer log entries (default 512, power of 2)");

struct kfifo tv_log_fifo;
EXPORT_SYMBOL_GPL(tv_log_fifo);

raw_spinlock_t tv_log_lock = __RAW_SPIN_LOCK_UNLOCKED(tv_log_lock);
EXPORT_SYMBOL_GPL(tv_log_lock);

u8 tv_log_level = TV_LOG_INFO;
EXPORT_SYMBOL_GPL(tv_log_level);

void tv_log(u8 level, u8 disk_idx, u8 event_type, const char *fmt, ...)
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

	raw_spin_lock_irqsave(&tv_log_lock, flags);
	kfifo_in(&tv_log_fifo, &entry, sizeof(entry));
	raw_spin_unlock_irqrestore(&tv_log_lock, flags);
}
EXPORT_SYMBOL_GPL(tv_log);

/* ---- EMA decay timer (Adaptive v2: adaptive interval) ---- */

/* Fast decay (100ms) when system is busy, slow decay (1s) when idle */
#define TV_DECAY_FAST (HZ / 10)
#define TV_DECAY_SLOW (HZ)

void tv_decay_timer_fn(struct timer_list *timer)
{
	struct tieredvol_ctx *ctx = from_timer(ctx, timer, adaptive.decay_timer);
	u32 alpha_shift = ctx->adaptive.ema_weight_shift;
	u64 alpha = (alpha_shift < 10) ? (1ULL << alpha_shift) : 1024;
	u64 one_minus_alpha = 1024 - alpha;
	u64 now = ktime_get_boottime_ns();
	u64 total_activity = 0;
	int i;
	int next_interval;

	for (i = 0; i < ctx->ndisks; i++) {
		u64 snapshot = (u64)atomic_xchg(&ctx->io.in_flight_bytes[i], 0);
		u64 completions = (u64)atomic64_xchg(&ctx->io.interval_completions[i], 0);

		total_activity += snapshot + completions;

		/* EMA load (bytes per interval) */
		ctx->adaptive.ema_load[i] =
			(ctx->adaptive.ema_load[i] * one_minus_alpha +
			 snapshot * alpha) >> 10;

		/* EMA IOPS (completions per interval, smoothed) */
		ctx->adaptive.ema_iops[i] =
			(ctx->adaptive.ema_iops[i] * one_minus_alpha +
			 completions * alpha) >> 10;

		/* EMA latency (average latency per I/O in ns, smoothed) */
		{
			u64 completions_total =
				(u64)atomic64_xchg(&ctx->io.total_completions[i], 0);
			u64 latency_total =
				(u64)atomic64_xchg(&ctx->io.total_latency_ns[i], 0);
			u64 avg_latency = completions_total ?
				latency_total / completions_total : 0;

			ctx->adaptive.ema_latency_ns[i] =
				(ctx->adaptive.ema_latency_ns[i] * one_minus_alpha +
				 avg_latency * alpha) >> 10;
		}

		ctx->adaptive.last_interval_bytes[i] = snapshot;

		if (ctx->adaptive.stale_after_ns > 0 && snapshot > 0)
			ctx->adaptive.last_finish_ns[i] = now;

		if (ctx->adaptive.stale_after_ns > 0 &&
		    !ctx->adaptive.stale[i] &&
		    ctx->adaptive.last_finish_ns[i] > 0 &&
		    now > ctx->adaptive.grace_until_ns[i] &&
		    (now - ctx->adaptive.last_finish_ns[i]) >
			    ctx->adaptive.stale_after_ns) {
			ctx->adaptive.stale[i] = true;
			ctx->adaptive.stale_marked_ns[i] = now;
			pr_info("tieredvol: disk[%d] %s STALE (no I/O for %llu ms)\n",
				i, ctx->meta.disk_names[i],
				(now - ctx->adaptive.last_finish_ns[i]) /
					1000000ULL);
			tv_log(TV_LOG_WARN, i, TV_LOG_STALE,
			       "STALE %llums",
			       (now - ctx->adaptive.last_finish_ns[i]) /
				       1000000ULL);
		} else if (ctx->adaptive.stale[i] && snapshot > 0) {
			ctx->adaptive.stale[i] = false;
			ctx->adaptive.grace_until_ns[i] =
				now + ctx->adaptive.stale_after_ns;
			pr_info("tieredvol: disk[%d] %s RECOVERED (I/O resumed)\n",
				i, ctx->meta.disk_names[i]);
			tv_log(TV_LOG_INFO, i, TV_LOG_RECOVER, "RECOVERED io");
		} else if (ctx->adaptive.stale[i] &&
			   (now - ctx->adaptive.stale_marked_ns[i]) >
				   2 * ctx->adaptive.stale_after_ns) {
			ctx->adaptive.stale[i] = false;
			ctx->adaptive.grace_until_ns[i] =
				now + ctx->adaptive.stale_after_ns;
			pr_info("tieredvol: disk[%d] %s RECOVERED (cooldown)\n",
				i, ctx->meta.disk_names[i]);
			tv_log(TV_LOG_INFO, i, TV_LOG_RECOVER,
			       "RECOVERED cooldown");
		}
	}

	/* Adaptive interval: fast when busy, slow when idle */
	next_interval = (total_activity > 1024) ? TV_DECAY_FAST : TV_DECAY_SLOW;
	mod_timer(&ctx->adaptive.decay_timer, jiffies + next_interval);
}
