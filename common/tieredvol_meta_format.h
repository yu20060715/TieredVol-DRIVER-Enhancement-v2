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

/* ---- Metadata format version ---- */
#define TV_META_VERSION      1

/* ---- CRC32C (Castagnoli) polynomial ---- */
#define TV_META_CRC32_POLY   0x82F63B78u

/* ---- Config file section headers ---- */
#define TV_META_SECTION_WEIGHTED  "[weighted_striping]"
#define TV_META_SECTION_RUNTIME   "[runtime]"

/* ---- Config file key names (for parsers) ---- */
#define TV_META_KEY_VERSION        "version"
#define TV_META_KEY_CHUNK_SIZE     "chunk_size"
#define TV_META_KEY_SEGMENT_COUNT  "segment_count"
#define TV_META_KEY_DISK_COUNT     "disk_count"
#define TV_META_KEY_CRC32          "crc32"
#define TV_META_KEY_POLICY         "policy"
#define TV_META_KEY_STALE_MS       "stale_ms"
#define TV_META_KEY_EMA_SHIFT      "ema_shift"
#define TV_META_KEY_WEAR_BIAS      "wear_bias"

#endif /* TIEREDVOL_META_FORMAT_H */
