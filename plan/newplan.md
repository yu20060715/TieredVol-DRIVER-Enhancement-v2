# TieredVol v4.6.0 → v5.0.0 Roadmap

Production-ready upgrade plan. Each item includes open-source references for implementation guidance.

---

## Phase 1: Data Integrity (Priority 1)

### 1a. Remove False DM Flags

**Problem:** Module advertises `DM_TARGET_ATOMIC_WRITES | DM_TARGET_PASSES_INTEGRITY` but implements neither. Misleads upper layers into unsafe assumptions.

**Fix:** Remove the flags, or implement true atomic write semantics.

**Effort:** 1 day

**References:**
1. `drivers/md/dm-table.c` — Flag registration and validation
   https://github.com/torvalds/linux/blob/master/drivers/md/dm-table.c
2. `drivers/md/dm-crypt.c` — Correct use of `DM_TARGET_PASSES_CRYPTO` (passes through, doesn't claim)
   https://github.com/torvalds/linux/blob/master/drivers/md/dm-crypt.c
3. `drivers/md/dm-integrity.c` — How to actually implement integrity (checksum per sector)
   https://github.com/torvalds/linux/blob/master/drivers/md/dm-integrity.c

---

### 1b. Mirror Write Ordering

**Problem:** Primary bio completes before mirror bio finishes. Read-after-write may miss mirror copy.

**Fix:** Use `BIOFENCE` or `dm_barrier` to ensure primary+mirror write ordering. Alternative: complete mirror before returning `DM_MAPIO_REMAPPED`.

**Effort:** 3-5 days

**References:**
1. `drivers/md/dm-raid1.c` — `mirror_write_endio()` + write ordering via `region_t` tracking
   https://github.com/torvalds/linux/blob/master/drivers/md/dm-raid1.c
2. `drivers/md/dm-log.c` — Region-based dirty tracking for write ordering
   https://github.com/torvalds/linux/blob/master/drivers/md/dm-log.c
3. `drivers/md/dm-bufio.c` — Buffer cache with write barrier support
   https://github.com/torvalds/linux/blob/master/drivers/md/dm-bufio.c

---

### 1c. Mirror Read Fallback

**Problem:** When primary disk I/O fails, reads cannot fall back to mirror. Zero actual redundancy.

**Fix:** In `tieredvol_end_io()`, check `bio->bi_status`. On error, resubmit read to mirror disk.

**Effort:** 2-3 days

**References:**
1. `drivers/md/dm-raid1.c` — `do_read()` fallback to mirror on primary failure
   https://github.com/torvalds/linux/blob/master/drivers/md/dm-raid1.c
2. `drivers/md/dm-mirror.c` — Mirror failover with read recovery
   https://github.com/torvalds/linux/blob/master/drivers/md/dm-mirror.c

---

### 1d. Metadata Write-back

**Problem:** Runtime changes (`set_mirror`, `set_policy`, etc.) only modify in-memory `ctx`. Lost on reboot.

**Fix:** After each DM message that modifies metadata, write updated config file back to disk.

**Effort:** 2-3 days

**References:**
1. `drivers/md/dm-thin-metadata.c` — Metadata commit with `dm_pool_commit_metadata()`
   https://github.com/torvalds/linux/blob/master/drivers/md/dm-thin-metadata.c
2. `drivers/md/dm-log.c` — Persistent log state via `dm_io()` to dedicated area
   https://github.com/torvalds/linux/blob/master/drivers/md/dm-log.c
3. `fs/configfs/configfs.c` — Kernel-to-userspace config persistence
   https://github.com/torvalds/linux/blob/master/fs/configfs/configfs.c

---

### 1e. Metadata Backup with CRC

**Problem:** Single config file, no integrity check. File corruption = volume destruction.

**Fix:** Add CRC32 to config file. Keep backup copy (`<name>.conf.bak`).

**Effort:** 1 day

**References:**
1. `lib/crc32.c` — CRC32 kernel API
   https://github.com/torvalds/linux/blob/master/lib/crc32.c
2. `drivers/md/dm-thin-metadata.c` — Superblock CRC validation
   https://github.com/torvalds/linux/blob/master/drivers/md/dm-thin-metadata.c

---

## Phase 2: Availability (Priority 2)

### 2a. Disk Failure Detection

**Problem:** `error_count` is allocated but never incremented. Disks always show 'A' (active) regardless of health.

**Fix:** In `tieredvol_end_io()`, check `bio->bi_status != BLK_STS_OK` and increment `error_count[disk]`. Update `tieredvol_status()` to reflect actual state.

**Effort:** 2-3 days

**References:**
1. `drivers/md/dm-raid1.c` — `error_count` tracking in mirror path
   https://github.com/torvalds/linux/blob/master/drivers/md/dm-raid1.c
2. `drivers/md/dm.c` — `dm_io_error()` pattern for bio completion errors
   https://github.com/torvalds/linux/blob/master/drivers/md/dm.c
3. `block/blk-mq.c` — Error injection and tracking
   https://github.com/torvalds/linux/blob/master/block/blk-mq.c

---

### 2b. Degraded Mode

**Problem:** When a disk fails, I/O to that disk returns error. No graceful degradation.

**Fix:** When `error_count[disk] > threshold`, mark disk as "degraded". Adaptive policy skips degraded disks. Reads can be served from mirror if available.

**Effort:** 5-7 days

**References:**
1. `drivers/md/dm-raid1.c` — Degraded mirror with `mirror_assign_failed()`
   https://github.com/torvalds/linux/blob/master/drivers/md/dm-raid1.c
2. `drivers/md/dm-thin.c` — Pool mode switching (PM_WRITE → PM_READ_ONLY) on metadata error
   https://github.com/torvalds/linux/blob/master/drivers/md/dm-thin.c
3. `drivers/md/md.c` — RAID degraded state management
   https://github.com/torvalds/linux/blob/master/drivers/md/md.c

---

### 2c. Mirror Rebuild

**Problem:** No mechanism to rebuild mirror after disk replacement. Manual intervention required.

**Fix:** Kernel thread reads from healthy disk, writes to new mirror disk. Progress tracked via `dmsetup status`.

**Effort:** 5-7 days

**References:**
1. `drivers/md/dm-raid1.c` — `do_recovery()` background mirror rebuild
   https://github.com/torvalds/linux/blob/master/drivers/md/dm-raid1.c
2. `drivers/md/md.c` — `md_do_sync()` resync/rebuild thread
   https://github.com/torvalds/linux/blob/master/drivers/md/md.c
3. `drivers/md/dm.c` — `dm_kcopyd_copy()` async data copy
   https://github.com/torvalds/linux/blob/master/drivers/md/dm.c

---

### 2d. ~~Hot-plug Phase 1 (Online Add)~~ **[ABANDONED]**

**Problem:** Cannot add disks at runtime. Must tear down and recreate volume.

**Fix:** DM table reload (`dmsetup suspend` → rebuild metadata → `dmsetup resume`).

**Effort:** 7-10 days

**Status:** 放棄。研究版本不需要此功能，複雜度過高（需 DM table reload + metadata 重建），ROI 不符合研究專案需求。

---

## Phase 3: Performance (Priority 3)

### 3a. Binary Search for Segment Lookup

**Problem:** Linear scan of up to 16 segments per bio. Measurable at high IOPS.

**Fix:** Sort segments by `logical_begin`, use binary search (`bisect`).

**Effort:** 1 day

**References:**
1. `drivers/md/dm-btree.c` — B-tree lookup for sorted data
   https://github.com/torvalds/linux/blob/master/drivers/md/dm-btree.c
2. `lib/bsearch.c` — Kernel binary search API
   https://github.com/torvalds/linux/blob/master/lib/bsearch.c

---

### 3b. ~~Write Coalescing~~ **[ABANDONED]**

**Problem:** No bio merging for adjacent sequential writes. Small random writes not batched.

**Fix:** Bio plug/unplug mechanism or writeback cache with dirty page tracking.

**Effort:** 5-7 days

**Status:** 放棄。研究版本不需要此功能，需大量改動 block layer（plug/unplug 或 writeback cache），風險過高且對研究目標無直接幫助。

---

### 3c. Configurable Chunk Size

**Problem:** `TV_CHUNK_SIZE` hardcoded to 1MB. Not optimal for all workloads.

**Fix:** Parse `chunk_size` from config file (already parsed in metadata, just not used).

**Effort:** 1 day

**References:**
1. `drivers/md/dm-stripe.c` — Configurable chunk size via `chunk_size` parameter
   https://github.com/torvalds/linux/blob/master/drivers/md/dm-stripe.c

---

## Phase 4: Operational (Priority 4)

### 4a. sysfs Attributes

**Problem:** No standard Linux monitoring interface. Only `dmsetup status`/`message`.

**Fix:** Register kobject with attributes: `policy`, `stale_ms`, `wear_bias`, per-disk stats.

**Effort:** 3-5 days

**References:**
1. `drivers/md/dm-table.c` — `dm_sysfs_init()` for DM target sysfs
   https://github.com/torvalds/linux/blob/master/drivers/md/dm-table.c
2. `drivers/md/dm-thin.c` — Pool sysfs attributes (`threshold_id`, `scan_set`)
   https://github.com/torvalds/linux/blob/master/drivers/md/dm-thin.c
3. `drivers/nvme/host/core.c` — NVMe sysfs attributes pattern
   https://github.com/torvalds/linux/blob/master/drivers/nvme/host/core.c

---

### 4b. Restore Script for dm-target

**Problem:** `tieredvol-restore.sh` doesn't handle kernel dm-target volumes. After reboot, volumes lost.

**Fix:** Add dm-target volume detection and recreation in restore script.

**Effort:** 2-3 days

**References:**
1. `scripts/tieredvol-restore.sh` — Current restore script (lines 192-257 need extension)
2. `scripts/tieredvol-setup.sh` — Volume creation flow to replicate
3. `drivers/md/dm.c` — `dm_create()` for programmatic target creation

---

### 4c. Configurable Log Buffer

**Problem:** 512-entry ring buffer fills in seconds under heavy I/O.

**Fix:** Make `TV_LOG_SIZE` a module parameter or DM message configurable.

**Effort:** 1 day

**References:**
1. `kernel/trace/ring_buffer.c` — Dynamic ring buffer sizing
   https://github.com/torvalds/linux/blob/master/kernel/trace/ring_buffer.c
2. `include/linux/kfifo.h` — `DECLARE_KFIFO` macro alternatives
   https://github.com/torvalds/linux/blob/master/include/linux/kfifo.h

---

## Summary

| Phase | Items | Effort | Impact |
|-------|:-----:|--------|--------|
| Phase 1: Data Integrity | 5/5 done | ~10 days | Data won't silently corrupt |
| Phase 2: Availability | 3/4 done, 1 abandoned (2d) | ~20 days | Disk failure → continue serving |
| Phase 3: Performance | 2/3 done, 1 abandoned (3b) | ~10 days | Better throughput |
| Phase 4: Operational | 3/3 done | ~7 days | Manageable |
| **Total** | **13/15 done, 2 abandoned** | **~47 days** | **Production-ready prototype** |

> **Abandoned items:**
> - **2d Hot-plug** — 複雜度過高（DM table reload + metadata 重建），研究版本不需要
> - **3b Write Coalescing** — 需大量改動 block layer，風險過高且對研究目標無直接幫助

---

## Open Source References (Consolidated)

### Linux Kernel Sources

| File | Features | URL |
|------|----------|-----|
| `drivers/md/dm-raid1.c` | 1b, 1c, 2a, 2b, 2c | https://github.com/torvalds/linux/blob/master/drivers/md/dm-raid1.c |
| `drivers/md/dm-thin.c` | 2b, 4a | https://github.com/torvalds/linux/blob/master/drivers/md/dm-thin.c |
| `drivers/md/dm-thin-metadata.c` | 1d, 1e | https://github.com/torvalds/linux/blob/master/drivers/md/dm-thin-metadata.c |
| `drivers/md/dm-log.c` | 1b, 1d | https://github.com/torvalds/linux/blob/master/drivers/md/dm-log.c |
| `drivers/md/dm.c` | 2a, 2c, ~~2d~~ | https://github.com/torvalds/linux/blob/master/drivers/md/dm.c |
| `drivers/md/dm-table.c` | 1a, ~~2d~~, 4a | https://github.com/torvalds/linux/blob/master/drivers/md/dm-table.c |
| `drivers/md/dm-integrity.c` | 1a | https://github.com/torvalds/linux/blob/master/drivers/md/dm-integrity.c |
| `drivers/md/dm-bufio.c` | 1b, ~~3b~~ | https://github.com/torvalds/linux/blob/master/drivers/md/dm-bufio.c |
| `drivers/md/dm-writecache.c` | ~~3b~~ | https://github.com/torvalds/linux/blob/master/drivers/md/dm-writecache.c |
| `drivers/md/dm-btree.c` | 3a | https://github.com/torvalds/linux/blob/master/drivers/md/dm-btree.c |
| `drivers/md/md.c` | 2b, 2c | https://github.com/torvalds/linux/blob/master/drivers/md/md.c |
| `drivers/md/dm-mirror.c` | 1c | https://github.com/torvalds/linux/blob/master/drivers/md/dm-mirror.c |
| `drivers/nvme/host/core.c` | 4a | https://github.com/torvalds/linux/blob/master/drivers/nvme/host/core.c |
| `block/blk-mq.c` | 2a, ~~3b~~ | https://github.com/torvalds/linux/blob/master/block/blk-mq.c |
| `fs/configfs/configfs.c` | 1d | https://github.com/torvalds/linux/blob/master/fs/configfs/configfs.c |
| `lib/crc32.c` | 1e | https://github.com/torvalds/linux/blob/master/lib/crc32.c |
| `lib/bsearch.c` | 3a | https://github.com/torvalds/linux/blob/master/lib/bsearch.c |
| `kernel/trace/ring_buffer.c` | 4c | https://github.com/torvalds/linux/blob/master/kernel/trace/ring_buffer.c |
| `include/linux/kfifo.h` | 4c | https://github.com/torvalds/linux/blob/master/include/linux/kfifo.h |

### External Projects

| Project | Features | URL |
|---------|----------|-----|
| mdadm | ~~2d~~ | https://github.com/md-raid-utilities/mdadm |
