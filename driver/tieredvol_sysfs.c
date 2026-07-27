// SPDX-License-Identifier: GPL-2.0-only
/*
 * tieredvol_sysfs.c — sysfs attributes at /sys/kernel/tieredvol/
 *
 * Extracted from tieredvol_core.c in Phase 1 refactoring.
 * All tv_active_ctx access is RCU-protected to prevent use-after-free.
 */
#include <linux/module.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/rcupdate.h>
#include "tieredvol.h"

static struct kobject *tv_kobj;

static ssize_t policy_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	struct tieredvol_ctx *ctx;
	ssize_t ret;

	rcu_read_lock();
	ctx = rcu_dereference(tv_active_ctx);
	if (!ctx) {
		rcu_read_unlock();
		return -ENODEV;
	}
	ret = sysfs_emit(buf, "%s\n",
			  ctx->adaptive.policy == TV_POLICY_ADAPTIVE ?
				  "adaptive" :
			  ctx->adaptive.policy == TV_POLICY_RANDOM ?
				  "random" :
				  "static");
	rcu_read_unlock();
	return ret;
}

static ssize_t stale_ms_show(struct kobject *kobj, struct kobj_attribute *attr,
			      char *buf)
{
	struct tieredvol_ctx *ctx;
	ssize_t ret;

	rcu_read_lock();
	ctx = rcu_dereference(tv_active_ctx);
	if (!ctx) {
		rcu_read_unlock();
		return -ENODEV;
	}
	ret = sysfs_emit(buf, "%llu\n",
			  ctx->adaptive.stale_after_ns / 1000000ULL);
	rcu_read_unlock();
	return ret;
}

static ssize_t stale_ms_store(struct kobject *kobj, struct kobj_attribute *attr,
			       const char *buf, size_t count)
{
	struct tieredvol_ctx *ctx;
	u32 ms;

	rcu_read_lock();
	ctx = rcu_dereference(tv_active_ctx);
	if (!ctx) {
		rcu_read_unlock();
		return -ENODEV;
	}
	if (kstrtou32(buf, 10, &ms)) {
		rcu_read_unlock();
		return -EINVAL;
	}
	ctx->adaptive.stale_after_ns = (u64)ms * 1000000ULL;
	rcu_read_unlock();
	return count;
}

static ssize_t wear_bias_show(struct kobject *kobj, struct kobj_attribute *attr,
			       char *buf)
{
	struct tieredvol_ctx *ctx;
	ssize_t ret;

	rcu_read_lock();
	ctx = rcu_dereference(tv_active_ctx);
	if (!ctx) {
		rcu_read_unlock();
		return -ENODEV;
	}
	ret = sysfs_emit(buf, "%u\n", ctx->adaptive.wear_bias);
	rcu_read_unlock();
	return ret;
}

static ssize_t wear_bias_store(struct kobject *kobj,
			       struct kobj_attribute *attr, const char *buf,
			       size_t count)
{
	struct tieredvol_ctx *ctx;
	u32 bias;

	rcu_read_lock();
	ctx = rcu_dereference(tv_active_ctx);
	if (!ctx) {
		rcu_read_unlock();
		return -ENODEV;
	}
	if (kstrtou32(buf, 10, &bias) || bias > 1024) {
		rcu_read_unlock();
		return -EINVAL;
	}
	ctx->adaptive.wear_bias = bias;
	rcu_read_unlock();
	return count;
}

static ssize_t ema_shift_show(struct kobject *kobj, struct kobj_attribute *attr,
			       char *buf)
{
	struct tieredvol_ctx *ctx;
	ssize_t ret;

	rcu_read_lock();
	ctx = rcu_dereference(tv_active_ctx);
	if (!ctx) {
		rcu_read_unlock();
		return -ENODEV;
	}
	ret = sysfs_emit(buf, "%u\n", ctx->adaptive.ema_weight_shift);
	rcu_read_unlock();
	return ret;
}

static ssize_t ema_shift_store(struct kobject *kobj,
			       struct kobj_attribute *attr, const char *buf,
			       size_t count)
{
	struct tieredvol_ctx *ctx;
	u32 shift;

	rcu_read_lock();
	ctx = rcu_dereference(tv_active_ctx);
	if (!ctx) {
		rcu_read_unlock();
		return -ENODEV;
	}
	if (kstrtou32(buf, 10, &shift) || shift > 10) {
		rcu_read_unlock();
		return -EINVAL;
	}
	ctx->adaptive.ema_weight_shift = shift;
	rcu_read_unlock();
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

static ssize_t disk_count_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	struct tieredvol_ctx *ctx;
	ssize_t ret;

	rcu_read_lock();
	ctx = rcu_dereference(tv_active_ctx);
	if (!ctx) {
		rcu_read_unlock();
		return -ENODEV;
	}
	ret = sysfs_emit(buf, "%d\n", ctx->ndisks);
	rcu_read_unlock();
	return ret;
}

static ssize_t status_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	struct tieredvol_ctx *ctx;
	int i, off = 0;

	rcu_read_lock();
	ctx = rcu_dereference(tv_active_ctx);
	if (!ctx) {
		rcu_read_unlock();
		return -ENODEV;
	}

	off += sysfs_emit_at(buf, off,
			     "policy=%d mirror=%llu/%llu err=%llu\n",
			     ctx->adaptive.policy,
			     atomic64_read(&ctx->mirror.mirror_write_ops),
			     atomic64_read(&ctx->mirror.mirror_write_bytes),
			     atomic64_read(&ctx->mirror.mirror_errors));

	for (i = 0; i < ctx->ndisks; i++) {
		off += sysfs_emit_at(
			buf, off,
			"%s: err=%d %s rd=%llu/%llu wr=%llu/%llu stale=%d ema=%llu\n",
			ctx->meta.disk_names[i],
			atomic_read(&ctx->deg.error_count[i]),
			ctx->deg.degraded[i] ? "DEGRADED" : "active",
			atomic64_read(&ctx->io.total_read_ops[i]),
			atomic64_read(&ctx->io.total_read_bytes[i]),
			atomic64_read(&ctx->io.total_write_ops[i]),
			atomic64_read(&ctx->io.total_write_bytes[i]),
			ctx->adaptive.stale[i],
			ctx->adaptive.ema_load[i]);
	}
	rcu_read_unlock();
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

void tv_sysfs_init(void)
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
EXPORT_SYMBOL_GPL(tv_sysfs_init);

void tv_sysfs_exit(void)
{
	if (tv_kobj) {
		sysfs_remove_group(tv_kobj, &tv_attr_group);
		kobject_put(tv_kobj);
		tv_kobj = NULL;
	}
}
EXPORT_SYMBOL_GPL(tv_sysfs_exit);
