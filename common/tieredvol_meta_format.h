/*
 * tieredvol_meta_format.h — Shared metadata format definitions
 *
 * Used by both kernel module (driver/) and userspace tool (src/).
 * Defines on-disk config format constants and key names.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef TIEREDVOL_META_FORMAT_H
#define TIEREDVOL_META_FORMAT_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

/* ---- Limits ---- */
#define TV_META_MAX_DISKS    16
#define TV_META_MAX_SEGS     16
#define TV_META_MAX_WEIGHT   16
#define TV_META_CHUNK_SIZE   (1UL << 20)

#endif /* TIEREDVOL_META_FORMAT_H */
