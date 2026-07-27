# TieredVol v5.0.0 Feature Catalog

Technical reference for all implemented features in the TieredVol device-mapper target. v5.0.0 is a complete rewrite of v4.6.0.

---

## File Structure (v5.0)

| File | Responsibility | Lines |
|------|----------------|-------|
| `tieredvol.h` | Header — all struct definitions | 258 |
| `tieredvol_core.c` | DM lifecycle, map dispatch, module init/exit | 595 |
| `tieredvol_map.c` | Logical→physical mapping: static/adaptive/random | 219 |
| `tieredvol_mirror.c` | Mirror I/O, per-CPU pending, ts ring, retry, rebuild | 571 |
| `tieredvol_log.c` | Log ring, EMA decay timer | 146 |
| `tieredvol_meta.c` | Config parser + CRC32C | 397 |
| `tieredvol_message.c` | 27 dmsetup message handlers | 780 |
| `tieredvol_sysfs.c` | sysfs interface (7 attributes) | 273 |

---

## Core Data Structures

### struct tieredvol_segment (tieredvol.h:20-29)

| Field | Type | Description |
|-------|------|-------------|
| `logical_begin` | `sector_t` | Segment start sector |
| `logical_end` | `sector_t` | Segment end sector |
| `disk_count` | `u32` | Number of disks in segment |
| `disk_index[16]` | `u32` | Disk index array |
| `weight[16]` | `u32` | Weighted stripe weights |
| `stripe_size` | `u32` | Stripe size (sectors) |
| `mirror_enabled` | `bool` | Mirror enable flag |
| `mirror_disk` | `u32` | Mirror target disk index |

### struct tieredvol_metadata (tieredvol.h:31-43)

| Field | Type | Description |
|-------|------|-------------|
| `version` | `u32` | Config version |
| `chunk_size` | `u32` | Base chunk size (bytes) |
| `segment_count` | `u32` | Number of segments |
| `disk_count` | `u32` | Number of disks |
| `disk_names[16][64]` | `char` | Disk device paths |
| `segments[16]` | `struct tieredvol_segment` | Segment array |
| `runtime_policy` | `int` | Runtime policy (override) |
| `stale_ms` | `u32` | Stale timeout (ms) |
| `ema_shift` | `u32` | EMA weight shift |
| `wear_bias` | `u32` | Wear penalty factor |

### struct tv_io_stats (tieredvol.h:60-70)

| Field | Type | Description |
|-------|------|-------------|
| `in_flight_bytes[16]` | `atomic_t` | Per-disk in-flight bytes |
| `total_write_bytes[16]` | `u64` | Per-disk total write bytes |
| `total_read_bytes[16]` | `u64` | Per-disk total read bytes |
| `total_write_ops[16]` | `u64` | Per-disk total write ops |
| `total_read_ops[16]` | `u64` | Per-disk total read ops |
| `total_latency_ns[16]` | `u64` | Per-disk accumulated latency (ns) |
| `total_completions[16]` | `u64` | Per-disk accumulated completions |
| `interval_completions[16]` | `u64` | Per-disk interval completions |

### struct tv_adaptive_state (tieredvol.h:72-86)

| Field | Type | Description |
|-------|------|-------------|
| `ema_weight_shift` | `u32` | EMA alpha shift (0-10) |
| `ema_load[16]` | `s64` | Per-disk EMA load score |
| `stale_after_ns` | `u64` | Stale timeout (ns) |
| `stale[16]` | `bool` | Per-disk stale flag |
| `stale_marked_ns[16]` | `ktime_t` | Per-disk stale marked time |
| `grace_until_ns[16]` | `ktime_t` | Per-disk grace period deadline |
| `last_finish_ns[16]` | `ktime_t` | Per-disk last I/O completion time |
| `decay_timer` | `struct timer_list` | EMA decay timer |
| `wear_bias` | `u32` | Wear penalty factor |
| `policy` | `int` | Current dispatch policy |
| `ema_latency_ns[16]` | `s64` | Per-disk EMA latency (ns) |
| `ema_iops[16]` | `s64` | Per-disk EMA IOPS |

### struct tieredvol_ctx (tieredvol.h:111-129)

| Field | Type | Description |
|-------|------|-------------|
| `ti` | `struct dm_target *` | DM target pointer |
| `meta` | `struct tieredvol_metadata *` | Metadata |
| `devs` | `struct dm_dev **` | DM device array |
| `disk_sectors` | `sector_t *` | Per-disk sector count |
| `config_path` | `char *` | Config file path |
| `ndisks` | `u32` | Number of disks |
| `min_chunk_sectors` | `sector_t` | Minimum chunk (sectors) |
| `stripe_sectors` | `sector_t` | Stripe size (sectors) |
| `io` | `struct tv_io_stats` | I/O statistics |
| `deg` | `struct tv_degradation` | Degradation state |
| `adaptive` | `struct tv_adaptive_state` | Adaptive state |
| `mirror` | `struct tv_mirror_stats` | Mirror statistics |
| `rebuild` | `struct tv_rebuild_state` | Rebuild state |
| `mirror_enabled_any` | `bool` | Any segment mirror enabled |
| `trigger_event` | `struct work_struct` | Event trigger work |
| `mirror_pw_pool` | `mempool_t *` | Mirror bio mempool |
| `retry_ctx_pool` | `mempool_t *` | Retry context mempool |

### struct tv_pending_read_cpu / tv_pending_write_cpu (tieredvol.h:196-213)

| Field | Type | Description |
|-------|------|-------------|
| `entries[64]` | `struct bio *` | Pending bio array |
| `head` | `u32` | Ring index head |
| `count` | `u32` | Current pending count |

---

## 1. I/O Dispatch

The I/O dispatch layer translates logical byte offsets into physical disk assignments. TieredVol supports three dispatch policies, each selecting a disk within the appropriate segment and computing the physical sector offset. Dispatch is implemented in `tieredvol_map.c`, with the main entry point in `tieredvol_core.c`.

### #1 Static Weighted Dispatch

> Deterministically maps logical offsets to disks based on precomputed weighted stripe boundaries.

**Implementation:** `tv_map_logical()`, located in `tieredvol_map.c:4-65`. Constructs prefix-sum boundary arrays from segment weights and performs linear scan to locate the target disk. Physical offset formula: `stripe_no * weight[disk] * CHUNK_SIZE + (offset_in - boundary[disk])`.

**Key APIs:** `bio_set_dev()`, `bdev_nr_sectors()`

**References:**
1. `drivers/md/dm-stripe.c` — classic striped target with weighted allocation
2. `drivers/md/dm-switch.c` — region-based bio dispatch and sector remapping

---

### #2 Adaptive Multi-Factor Dispatch

> Uses multi-factor scoring (EMA load + EMA latency + wear penalty) to select the optimal disk per bio, automatically skipping stale disks.

**Implementation:** `tv_map_logical_adaptive()`, located in `tieredvol_map.c:132-149`. Scoring formula:

```
score = ema_load[d] + ema_latency_ns[d] / 1000000 + wear_bias * total_write_bytes[d] / total_writes
```

Iterates candidate disks, selects minimum score. If all candidates are stale, falls back to any valid disk. The EMA latency term is in microseconds (divides ns by 1000000) to match the load factor's order of magnitude.

**Fallback two-pass scan:** When all candidate disks have poor scores (all stale/degraded), the fallback performs a two-pass scan: first pass skips stale/degraded disks, preferring healthy ones; if first pass finds nothing (all disks stale/degraded), second pass accepts any valid disk (including stale/degraded), ensuring I/O does not fail.

**Key APIs:** `get_random_u32()`, atomic EMA updates (`tv_decay_timer_fn()`)

**References:**
1. Jiao & Kim, "Asymmetric RAID: Rethinking RAID for SSD Heterogeneity" — HotStorage'24
2. `block/kyber-iosched.c` — token-based dynamic depth
3. `block/mq-deadline.c` — request sorting and time-based expiry

---

### #3 Random Dispatch

> Uniformly selects a random disk within the segment.

**Implementation:** `tv_map_logical_random()`, located in `tieredvol_map.c:158-201`. Uses `get_random_u32() % seg->disk_count` for uniform selection.

**Key APIs:** `get_random_u32()`

**References:**
1. `drivers/md/dm-switch.c` — random path selection fallback

---

### #4 Bio Sector Remapping

> Redirects bio from DM virtual device to correct physical disk by rewriting `bi_bdev` and `bi_iter.bi_sector`.

**Implementation:** `tieredvol_map()`, located in `tieredvol_core.c:33-164`. Calls `bio_set_dev(bio, ctx->devs[cur.disk]->bdev)` and sets `bio->bi_iter.bi_sector = cur.offset >> SECTOR_SHIFT`.

**Key APIs:** `bio_set_dev()`, `SECTOR_SHIFT`

**References:**
1. `drivers/md/dm-crypt.c` — bio clone + redirect to encryption layer
2. `drivers/md/dm-linear.c` — simplest bio remap target

---

### #5 Invalid Disk Error

> Returns bio error when computed disk index is out of range.

**Implementation:** `tieredvol_map()`, located in `tieredvol_core.c:89-96`. Checks `cur.disk < 0 || cur.disk >= ctx->ndisks`, calls `bio_io_error(bio)` and returns `DM_MAPIO_SUBMITTED`.

**Key APIs:** `bio_io_error()`

**References:**
1. `drivers/md/dm.c` — DM core error handling

---

### #6 Write Mirroring

> Clones write bio and submits to designated mirror disk for data redundancy.

**Implementation:** `tieredvol_map()`, located in `tieredvol_core.c:126-160`. When `seg->mirror_enabled` and mirror disk differs from primary, allocates clone bio from `ctx->mirror_pw_pool` (`mempool_alloc()`), sets custom `tv_mirror_end_io` completion handler, submits via `submit_bio()`. Mempool ensures zero OOM.

**Key APIs:** `bio_alloc_clone()`, `submit_bio()`, `bio_put()`, `mempool_alloc()`

**References:**
1. `drivers/md/dm-raid1.c` — bio clone + dual disk write + custom bi_end_io

---

## 2. Load Balancing + Adaptive Decay

Load balancing uses in-flight byte counters and triple EMA smoothing (load, IOPS, latency) to track per-disk I/O pressure. A hardware timer decays these counters at an adaptive interval, providing smooth disk state estimates for the multi-factor adaptive dispatch.

### #7 EMA Load Calculation

> Computes per-disk load via exponential moving average: `ema = ema * (1 - alpha) + snapshot * alpha`, alpha tunable via `ema_weight_shift`.

**Implementation:** `tv_decay_timer_fn()`, located in `tieredvol_log.c:81-83`. Default alpha = `1 << 3 = 8` (full value 1024, shift=3). Snapshot is atomic in-flight byte counter, zeroed each tick via `atomic_xchg()`.

**Key APIs:** `timer_list`, `atomic_xchg()`

**References:**
1. `kernel/sched/fair.c` — CFS per-CPU load tracking
2. `drivers/md/dm-thin.c` — pool mode switching and low watermark callback

---

### #8 EMA IOPS Calculation

> Tracks per-disk completed IOPS via EMA, smoothing burst traffic.

**Implementation:** `tv_decay_timer_fn()`, located in `tieredvol_log.c:86-88`. Snapshot is atomic `interval_completions` counter, zeroed each tick via `atomic_xchg()` before EMA update.

**Key APIs:** `atomic_xchg()`

---

### #9 EMA Latency Measurement

> Tracks per-disk I/O latency via EMA, combined with timestamp ring for lockless measurement.

**Implementation:** `tv_decay_timer_fn()`, located in `tieredvol_log.c:91-102`. Computes interval average latency from `total_latency_ns` and `total_completions`, updates `ema_latency_ns[d]`. Timestamp ring (`tv_ts_ring`) implements lockless latency tracking in `tieredvol_mirror.c:155-219`: `tv_ts_submit()` (:170-188) records submission time, `tv_ts_complete()` (:190-219) computes completion time delta.

**Key APIs:** `ktime_get_boottime_ns()`

---

### #10 In-flight Byte Tracking

> Counts bytes currently in transit using per-disk atomic counters.

**Implementation:** `tieredvol_map()`, located in `tieredvol_core.c:109`. `atomic_add(bio->bi_iter.bi_size, &ctx->io.in_flight_bytes[cur.disk])`. Decay timer zeroes via `atomic_xchg()`.

**Key APIs:** `atomic_add()`, `atomic_xchg()`

**References:**
1. `block/blk-mq.c` — blk-mq per-tag statistics

---

### #11 Adaptive Decay Timer Interval

> Timer interval adapts dynamically based on I/O activity: 100ms when busy, 1s when idle.

**Implementation:** `tv_decay_timer_fn()`, located in `tieredvol_log.c:143-145`. If any completions occurred in the interval (`total_completions[d] > 0`), uses `TV_DECAY_FAST = HZ/10` (100ms); otherwise `TV_DECAY_SLOW = HZ` (1s). Rearmed via `mod_timer()`.

**Key APIs:** `mod_timer()`

**References:**
1. `kernel/time/timer_list.c` — timer_list API

---

## 3. Stale Disk Detection + Recovery

Stale detection identifies disks that have stopped responding to I/O. Disks are marked stale after a configurable timeout, then automatically recover on new I/O or after cooldown. Recovery grace period prevents immediate re-marking.

### #12 Stale Marking

> Marks disk stale when no I/O completion occurs within `stale_after_ns` (default 5 seconds).

**Implementation:** `tv_decay_timer_fn()`, located in `tieredvol_log.c:112-123`. Checks `now > ctx->adaptive.grace_until_ns[i]` and `(now - ctx->adaptive.last_finish_ns[i]) > ctx->adaptive.stale_after_ns`. Sets `ctx->adaptive.stale[i] = true` and logs via `tv_log(TV_LOG_WARN, ...)`.

**Key APIs:** `ktime_get_boottime_ns()`

**References:**
1. `drivers/md/dm-dust.c` — bad block simulation and enable/disable
2. `drivers/md/dm-raid1.c` — mirror failover and error detection

---

### #13 Stale Recovery — I/O Triggered

> Immediately recovers stale disk when new I/O completes, starting a new grace period.

**Implementation:** `tv_decay_timer_fn()`, located in `tieredvol_log.c:124-129`. When `ctx->adaptive.stale[i] && snapshot > 0`, sets `stale[i] = false` and `grace_until_ns[i] = now + stale_after_ns`.

**Key APIs:** None (state machine)

**References:**
1. `drivers/md/dm-log.c` — dirty region tracking and recovery

---

### #14 Stale Recovery — Cooldown

> Automatically recovers stale disk after 2x stale timeout, even without new I/O.

**Implementation:** `tv_decay_timer_fn()`, located in `tieredvol_log.c:130-138`. When `(now - ctx->adaptive.stale_marked_ns[i]) > 2 * ctx->adaptive.stale_after_ns`, sets `stale[i] = false`.

**Key APIs:** None (state machine)

**References:**
1. `block/disk-events.c` — disk event polling and timeout

---

### #15 Grace Period

> Protects newly recovered disks from immediate re-marking as stale.

**Implementation:** `tv_decay_timer_fn()`, located in `tieredvol_log.c:126,134`. On recovery, sets `ctx->adaptive.grace_until_ns[i] = now + ctx->adaptive.stale_after_ns`. Line 115 stale check verifies `now > ctx->adaptive.grace_until_ns[i]` before marking stale.

**Key APIs:** None (state machine)

**References:**
1. `drivers/md/dm-raid1.c` — mirror recovery grace period

---

## 4. Per-disk I/O Statistics

Cumulative counters per physical disk, tracked throughout the device-mapper target's lifetime. Stored in `struct tv_io_stats`.

### #16 Read Bytes Counter

> Total bytes read per disk.

**Implementation:** `ctx->io.total_read_bytes[cur.disk] += bio->bi_iter.bi_size`, located in `tieredvol_core.c:114`.

**References:** `drivers/md/dm.c` — DM core bio statistics

---

### #17 Read Ops Counter

> Total read operations per disk.

**Implementation:** `ctx->io.total_read_ops[cur.disk]++`, located in `tieredvol_core.c:115`.

**References:** `block/blk-mq.c` — blk-mq per-tag statistics

---

### #18 Write Bytes Counter

> Total bytes written per disk.

**Implementation:** `ctx->io.total_write_bytes[cur.disk] += bio->bi_iter.bi_size`, located in `tieredvol_core.c:111`.

**References:** `drivers/nvme/host/core.c` — NVMe per-controller statistics

---

### #19 Write Ops Counter

> Total write operations per disk.

**Implementation:** `ctx->io.total_write_ops[cur.disk]++`, located in `tieredvol_core.c:112`.

**References:** `block/genhd.c` — `part_stat_show` per-partition statistics

---

### #20 Error Counter

> Per-disk atomic error count, incremented on bio completion errors.

**Implementation:** `ctx->error_count[disk]` is an `atomic_t` array, allocated in `tieredvol_ctr()`. Read via `atomic_read()` in `tieredvol_status()` and degradation detection.

**Key APIs:** `atomic_read()`, `atomic_set()`, `atomic_inc()`

**References:**
1. `drivers/md/dm.c` — DM error counting
2. `drivers/md/dm-raid1.c` — mirror error tracking

---

## 5. DM Message Commands

Runtime control interface via `dmsetup message`. All commands dispatched in `tv_message()` (`tieredvol_message.c`, 27 handlers).

### #21 `reset_stats` (Command 0)

> Clears all per-disk I/O statistics (ops, bytes, latency, completion counts).

**Implementation:** `tieredvol_message.c`. Zeros all 7 counter arrays in `tv_io_stats`.

---

### #22 `show_stats` (Command 1)

> Returns per-disk read/write ops and bytes.

**Implementation:** `tieredvol_message.c`. Outputs `total_read_ops/bytes` and `total_write_ops/bytes` per disk via `DMINFO()`.

---

### #23 `show_inflight` (Command 3)

> Returns per-disk in-flight bytes.

**Implementation:** `tieredvol_message.c`. Reads `atomic_read(&ctx->io.in_flight_bytes[i])`.

---

### #24 `show_io_stats` (Command 4)

> Returns full per-disk I/O statistics including latency and completion counts.

**Implementation:** `tieredvol_message.c`. Outputs `total_write/read_bytes`, `total_write/read_ops`, `total_latency_ns`, `total_completions`.

---

### #25 `reset_io_stats` (Command 5)

> Zeros all 7 per-disk I/O statistics counters.

**Implementation:** `tieredvol_message.c`. Zeros `in_flight_bytes`, `total_write/read_bytes`, `total_write/read_ops`, `total_latency_ns`, `total_completions`, `interval_completions`.

---

### #26 `adaptive_on` (Command 6)

> Switches dispatch policy to adaptive (multi-factor score-based load balancing).

**Implementation:** `tieredvol_message.c`. Sets `ctx->adaptive.policy = TV_POLICY_ADAPTIVE`.

---

### #27 `adaptive_off` (Command 7)

> Switches dispatch policy to static (weighted boundary-based).

**Implementation:** `tieredvol_message.c`. Sets `ctx->adaptive.policy = TV_POLICY_STATIC`.

---

### #28 `set_policy <name>` (Command 8)

> Sets dispatch policy to static, adaptive, or random.

**Implementation:** `tieredvol_message.c`. Validates `argv[1]` is "static", "adaptive", or "random".

---

### #29 `set_ema_shift <shift>` (Command 9)

> Sets EMA weight shift (0-10). Alpha = `1 << shift` (full value 1024).

**Implementation:** `tieredvol_message.c`. Validates shift <= 10 via `kstrtou32()`.

**Key APIs:** `kstrtou32()`

---

### #30 `set_stale_ms <ms>` (Command 10)

> Sets stale detection timeout in milliseconds.

**Implementation:** `tieredvol_message.c`. Converts to ns: `ctx->adaptive.stale_after_ns = (u64)ms * 1000000ULL`.

---

### #31 `show_adaptive` (Command 11)

> Returns policy, EMA shift, stale timeout, wear bias, and per-disk load/latency/wear/stale status. Latency displayed in microseconds.

**Implementation:** `tieredvol_message.c:304-327`. Output includes `lat=XXus` format latency field, converting `ema_latency_ns[d] / 1000000` to microseconds.

---

### #32 `show_wear` (Command 12)

> Returns wear bias and per-disk total write bytes.

**Implementation:** `tieredvol_message.c`. Outputs `wear_bias` and per-disk `total_write_bytes[d]`.

---

### #33 `set_wear_bias <bias>` (Command 13)

> Sets wear penalty factor (0-1024). Higher values penalize high-wear disks more in adaptive dispatch.

**Implementation:** `tieredvol_message.c`. Validates `bias <= 1024`.

---

### #34 `reset_wear` (Command 14)

> Zeros per-disk write byte counters (wear counters).

**Implementation:** `tieredvol_message.c`. Sets all `total_write_bytes[d]` to 0.

---

### #35 `show_mirror` (Command 15)

> Returns mirror write ops/bytes, error count, and per-segment mirror configuration.

**Implementation:** `tieredvol_message.c`. Outputs `mirror_stats` and segment mirror settings.

---

### #36 `set_mirror <seg> <disk>` (Command 16)

> Enables mirroring for a segment, designating target disk.

**Implementation:** `tieredvol_message.c`. Validates `seg_idx < segment_count` and `disk_idx < ndisks`. Sets `seg->mirror_enabled = true` and `seg->mirror_disk = disk_idx`. Sets `ctx->mirror_enabled_any = true` (`tieredvol_mirror.c:398`).

---

### #37 `show_log` (Command 17)

> Non-destructive log ring read: uses `kfifo_out()` + `kfifo_in()` to copy entries and restore, consuming nothing.

**Implementation:** `tieredvol_message.c:487-526`. Under spinlock, uses `kfifo_out()` to drain entries to temporary array, formats output, then writes back via `kfifo_in()` preserving original ring contents.

**Key APIs:** `kfifo_out()`, `kfifo_in()`, `raw_spin_lock_irqsave()`

---

### #38 `clear_log` (Command 18)

> Resets log ring buffer.

**Implementation:** `tieredvol_message.c`. Calls `kfifo_reset()` under spinlock.

---

### #39 `set_loglevel <0-3>` (Command 19)

> Sets log verbosity: OFF(0) / ERROR(1) / WARN(2) / INFO(3).

**Implementation:** `tieredvol_message.c`. Sets global `tv_log_level`.

---

### #40 `show_errors` (Command 20)

> Returns per-disk error count.

**Implementation:** `tieredvol_message.c`. Reads via `atomic_read(&ctx->error_count[d])`.

---

### #41 `reset_errors` (Command 21)

> Zeros all per-disk error counts.

**Implementation:** `tieredvol_message.c`. Sets all `error_count[d]` to 0.

---

### #42 `set_error_threshold <n>` (Command 22)

> Sets error threshold triggering degraded mode.

**Implementation:** `tieredvol_message.c`. Sets `ctx->deg.error_threshold = n`.

---

### #43 `show_degraded` (Command 23)

> Returns degraded mode status: whether degraded, trigger reason, error threshold.

**Implementation:** `tieredvol_message.c`. Outputs `ctx->deg` state.

---

### #44 `clear_degraded` (Command 24)

> Manually clears degraded mode flag.

**Implementation:** `tieredvol_message.c`. Sets `ctx->deg.is_degraded = false`.

---

### #45 `start_rebuild [max_bytes]` (Command 25)

> Starts background rebuild thread with optional max bytes per iteration.

**Implementation:** `tieredvol_message.c`. Creates kthread `tv_rebuild_thread()` with exponential backoff retry. Returns error if already rebuilding. Optional `max_bytes` parameter limits per-iteration processing.

---

### #46 `stop_rebuild` (Command 26)

> Stops background rebuild thread.

**Implementation:** `tieredvol_message.c`. Sets stop flag and waits for kthread exit.

---

### #47 `show_rebuild` (Command 27)

> Returns rebuild status: running/completed/stopped, bytes processed, total bytes.

**Implementation:** `tieredvol_message.c`. Outputs `ctx->rebuild` state.

---

## 6. DM Target Lifecycle

Standard device-mapper target callback functions managing the tieredvol target lifecycle.

### #48 Constructor (ctr)

> Allocates context, loads metadata from config file, acquires DM devices, allocates mempools, and starts decay timer.

**Implementation:** `tieredvol_ctr()`, located in `tieredvol_core.c`. Flow:
1. Parse arguments (expects 1: config file path)
2. `kzalloc(sizeof(*ctx))` — allocate context
3. `tv_metadata_load_kernel()` — parse key=value config via `filp_open()` + `kernel_read()`
4. `kcalloc(ndisks, sizeof(*ctx->devs))` — allocate device array
5. `dm_get_device()` — acquire each DM device
6. Compute `min_chunk_sectors` across all segments
7. `dm_set_target_max_io_len()` — set max I/O size
8. `mempool_create()` — create mirror_pw_pool and retry_ctx_pool
9. `timer_setup()` + `mod_timer()` — start decay timer

**Key APIs:** `kzalloc()`, `kcalloc()`, `dm_get_device()`, `dm_set_target_max_io_len()`, `timer_setup()`, `mempool_create()`

---

### #49 Destructor (dtr)

> Stops decay timer, stops rebuild thread, flushes pending work, releases DM devices, and frees all memory.

**Implementation:** `tieredvol_dtr()`, located in `tieredvol_core.c`. Flow:
1. `timer_delete_sync()` — stop decay timer
2. `kthread_stop()` — stop rebuild thread (if running)
3. `flush_work()` — complete pending trigger_event work
4. `dm_put_device()` — release each DM device
5. `mempool_destroy()` — destroy mempools
6. `kfree()` — free all allocations

**Key APIs:** `timer_delete_sync()`, `kthread_stop()`, `flush_work()`, `dm_put_device()`, `mempool_destroy()`, `kfree()`

---

### #50 IO Hints

> Reports chunk size, block size, and optimal I/O size to DM framework.

**Implementation:** `tieredvol_io_hints()`, located in `tieredvol_core.c`. Sets `logical_block_size = 512`, `physical_block_size = 512`, `chunk_sectors = min_chunk_sectors`, `io_opt = stripe_sectors`.

**Key APIs:** `struct queue_limits`

---

### #51 Iterate Devices

> Returns all underlying DM devices for status reporting and ioctl passthrough.

**Implementation:** `tieredvol_iterate_devices()`, located in `tieredvol_core.c`. Iterates `ctx->devs[0..ndisks-1]`, calling callback for each device.

**Key APIs:** `iterate_devices_callout_fn`, `bdev_nr_sectors()`

---

### #52 Prepare Ioctl

> Returns first underlying block device for ioctl passthrough.

**Implementation:** `tieredvol_prepare_ioctl()`, located in `tieredvol_core.c`. Sets `*bdev = ctx->devs[0]->bdev`.

**Key APIs:** `struct block_device`

---

### #53 Flush/Discard Propagation

> Propagates flush and discard commands to all underlying disks.

**Implementation:** `tieredvol_ctr()`. Sets `ti->num_flush_bios = ctx->ndisks` and `ti->num_discard_bios = ctx->ndisks`. Also sets `ti->flush_bypasses_map = true`.

---

### #54 Module Init/Exit

> Registers DM target with kernel and creates workqueue.

**Implementation:** `tieredvol_init()`, located in `tieredvol_core.c:534-577`. Calls `dm_register_target()` and `alloc_workqueue("tieredvol_wq", WQ_UNBOUND | WQ_HIGHPRI, 0)`. Exit calls `dm_unregister_target()` and `destroy_workqueue()`.

**Key APIs:** `dm_register_target()`, `dm_unregister_target()`, `alloc_workqueue()`

---

## 7. Status Reporting

Runtime visibility via `dmsetup status` output, providing target operational status.

### #55 STATUSTYPE_INFO

> Returns policy, mirror stats, error counts, and per-disk active/degraded and read/write counters. Includes `status` message command disk names and weights.

**Implementation:** `tieredvol_status()`, located in `tieredvol_core.c` (STATUSTYPE_INFO case). Format: `policy=N mirror=ops/bytes err=N Ddisk:rd=ops/bytes wr=ops/bytes`.

---

### #56 STATUSTYPE_TABLE

> Returns list of underlying disk device names.

**Implementation:** `tieredvol_core.c` (STATUSTYPE_TABLE case). Space-separated disk names.

---

### #57 STATUSTYPE_IMA

> IMA (Integrity Measurement Architecture) placeholder — returns empty string.

**Implementation:** `tieredvol_core.c`. Sets `result[0] = '\0'`.

---

## 8. Metadata Parsing

Kernel-space config file parser loading tieredvol topology from key=value text file. v5.0 adds CRC32C checksum and non-destructive pre-scan.

### #58 Kernel File-based Config

> Reads config file from kernel space using `filp_open()` + `kernel_read()`.

**Implementation:** `tv_metadata_load_kernel()`, located in `tieredvol_meta.c`. Opens file with `filp_open(path, O_RDONLY, 0)`, reads up to 1MB via `kernel_read()`, then parses line by line.

**Key APIs:** `filp_open()`, `kernel_read()`, `i_size_read()`, `vmalloc()`, `vfree()`

**References:**
1. `drivers/md/dm-thin-metadata.c` — complex metadata management (B-tree + superblock)
2. `drivers/md/dm-log.c` — disk log state persistence

---

### #59 Version/Chunk/Segment/Disk Parsing

> Parses and validates version, chunk_size, segment_count, disk_count, disk names, segment disk indices/weight CSV, and mirror safety.

**Implementation:** `tieredvol_meta.c:150-187`. Uses `parse_u32()` / `parse_u64()` helpers. Validates `disk_count <= TV_MAX_DISKS` and `segment_count <= TV_MAX_SEGS`. Parses comma-separated u32 arrays via `parse_csv_u32()` (:54-70). Mirror safety validation (:377-387) ensures `mirror_disk < disk_count` and mirror disk differs from primary disk.

**Key APIs:** `kstrtoul()`, `kstrtoull()`, `strsep()`

**References:**
1. `fs/configfs/configfs.c` — kernel-userspace config interface
2. `drivers/md/dm-table.c` — DM table parsing

---

### #60 CRC32C Validation

> CRC32C integrity check on config content, supporting non-destructive pre-scan.

**Implementation:** `tv_compute_config_crc()`, located in `tieredvol_meta.c:98-133`. Computes `crc32c(0, config_start, config_len)`. CRC pre-scan (:183-219) locates CRC section before parsing, using `save_nl`/`restore_nl` and `save_eq`/`restore_eq` to save/restore parse state, avoiding destructive modification of original config string.

**Key APIs:** `crc32c()`

---

## 9. Structured Diagnostic Log

Kernel-space ring buffer logging I/O, stale, mirror, and config events with timestamps and severity levels. Queried via `dmsetup message`. v5.0 uses `raw_spinlock` instead of `spinlock`.

### #61 Ring Buffer

> Fixed-size `kfifo` ring buffer (512 entries) with `raw_spinlock` protection, recording timestamped log entries.

**Implementation:** `tieredvol_log.c:27`. `DECLARE_KFIFO(tv_log_fifo, struct tv_log_entry, TV_LOG_SIZE)` with `raw_spinlock_t tv_log_lock`. Each `tv_log()` call acquires lock via `raw_spin_lock_irqsave()`, writes `struct tv_log_entry` (64 bytes: timestamp_ns, level, disk_idx, event_type, msg[48]), then releases. Overflows overwrite oldest entry.

**Data structure:** `struct tv_log_entry`, located in `tieredvol.h`.

**Key APIs:** `DECLARE_KFIFO()`, `kfifo_put()`, `kfifo_get()`, `kfifo_reset()`, `raw_spin_lock_irqsave()`

**References:**
1. emlog (nicupavel) — ring buffer architecture with overflow overwrite
   https://github.com/nicupavel/emlog
2. `kernel/samples/kfifo/record-example.c` — kfifo usage patterns
3. `kernel/trace/ring_buffer.c` — high-performance lockless ring buffer

---

### #62 Log Level

> Dynamic verbosity control: OFF(0) / ERROR(1) / WARN(2) / INFO(3).

**Implementation:** `tieredvol_message.c`. `set_loglevel <0-3>` sets global `tv_log_level`. `tv_log()` function checks `if (level > tv_log_level) return;`.

**Key APIs:** `kstrtou32()`

**References:**
1. `kernel/trace/trace.c` — trace event management with log level control

---

### #63 DM Query (show_log / clear_log)

> Real-time log query via `show_log` (non-destructive read using kfifo_out+kfifo_in) and `clear_log` (reset ring buffer).

**Implementation:** `tieredvol_message.c`.
- `show_log` (:487-526): Under raw_spinlock, uses `kfifo_out()` to drain entries to temporary array, prints each entry to dmesg in format `LOG {ERR|WRN|INF} {I/O|STALE|RCVR|MIRR|CONF}: <msg>`, then writes back via `kfifo_in()` preserving ring contents.
- `clear_log`: Calls `kfifo_reset()` under raw_spinlock.

**Key APIs:** `kfifo_out()`, `kfifo_in()`, `kfifo_reset()`, `raw_spin_lock_irqsave()`, `DMINFO()`

**References:**
1. `drivers/md/dm-dust.c` — DM message query/result pattern
2. `drivers/md/dm-log-writes.c` — structured I/O event logging

---

## 10. Mirror + Pending Tracking

v5.0 introduces per-CPU pending arrays for lockless mirror tracking, timestamp ring for precise latency measurement, and mempool for zero OOM.

### #64 Per-CPU Pending Read Arrays

> Per-CPU ring buffers tracking mirror read bios awaiting completion, avoiding global lock contention.

**Implementation:** `tv_pending_add()`, located in `tieredvol_mirror.c:22-41`. Uses `DEFINE_PER_CPU(struct tv_pending_read_cpu, tv_pending_reads)`. Each CPU maintains `entries[64]` ring buffer with `head` and `count` tracking. `tv_pending_find_and_remove()` (:43-79) searches and removes from corresponding CPU's pending array on bio completion.

**Key APIs:** `DEFINE_PER_CPU()`, `this_cpu_read()`, `this_cpu_write()`

---

### #65 Per-CPU Pending Write Arrays

> Per-CPU ring buffers tracking mirror write bios awaiting completion.

**Implementation:** Similar to #64, using `DEFINE_PER_CPU(struct tv_pending_write_cpu, tv_pending_writes)`, located in `tieredvol_mirror.c:209-213`.

**Key APIs:** `DEFINE_PER_CPU()`

---

### #66 Timestamp Ring

> Per-disk 256-entry ring buffer recording bio submission timestamps for precise latency measurement.

**Implementation:** `struct tv_ts_ring` (`tieredvol_mirror.c:161-165`), contains `entries[256]`, `head`, `count`. `tv_ts_submit()` (:170-188) records `ktime_get_boottime_ns()` timestamp at bio submission. `tv_ts_complete()` (:190-219) retrieves timestamp on completion, computes latency delta and updates `total_latency_ns[d]` and `total_completions[d]`.

**Overflow handling:** When ring is full (count == 256), `tv_ts_submit()` overwrites the oldest entry (advances head) instead of dropping the new entry. Ensures latency EMA does not become stale due to data loss under high IOPS.

**Lock type:** `tv_ts_lock_arr` uses `raw_spinlock_t` (not `spinlock_t`), because `tv_ts_complete()` is called from bio end_io handlers which may run in atomic context (non-sleepable).

**Key APIs:** `ktime_get_boottime_ns()`

---

### #67 Mempool

> Mempool manages mirror bio clones and retry contexts, guaranteeing zero OOM allocation failure.

**Implementation:**
- `ctx->mirror_pw_pool`: Created in `tieredvol_ctr()` (`tieredvol_core.c:103`), for mirror write bio clone allocation.
- `ctx->retry_ctx_pool`: Created in `tieredvol_mirror.c:372`, for I/O failure retry contexts.

**Key APIs:** `mempool_create()`, `mempool_alloc()`, `mempool_free()`, `mempool_destroy()`

---

### #68 mirror_enabled_any Guard Flag

> Global boolean flag for fast check whether any segment has mirroring enabled, avoiding unnecessary mirror logic execution.

**Implementation:** `ctx->mirror_enabled_any`, located in `tieredvol.h:125`. Used as fast path check in `tieredvol_map()` (`tieredvol_core.c:279-285`): if `!ctx->mirror_enabled_any`, skip mirror logic. Set to true in `set_mirror` command (`tieredvol_mirror.c:398`).

**Key APIs:** None (boolean flag)

---

## 11. Degradation Management

Degradation subsystem tracks per-disk error counts, automatically entering degraded mode when errors exceed configurable threshold. Provides error reset, threshold configuration, degradation status query, and manual recovery.

### #69 Per-disk Atomic Error Counter

> Tracks per-disk accumulated errors using `atomic_t` array, incremented in bio completion callbacks.

**Implementation:** `ctx->error_count[disk]`, allocated and initialized in `tieredvol_ctr()` via `kcalloc()` and `atomic_set(&ctx->error_count[i], 0)`. Incremented via `atomic_inc()` in `tv_mirror_end_io()` and other error paths.

**Key APIs:** `atomic_set()`, `atomic_inc()`, `atomic_read()`

---

### #70 Configurable Error Threshold

> Error threshold configurable via `set_error_threshold <n>`, triggering degraded mode when exceeded.

**Implementation:** `ctx->deg.error_threshold`, set via message command #42 (`set_error_threshold`). Checked in `tieredvol_status()` callback against per-disk error counts.

**Key APIs:** None (config value)

---

### #71 Auto-degradation Detection

> Automatically enters degraded mode when any disk's error count exceeds threshold.

**Implementation:** Degradation check logic in `tieredvol_status()`. Iterates all disks' `atomic_read(&ctx->error_count[d])`, sets `ctx->deg.is_degraded = true` and logs via `tv_log(TV_LOG_WARN, ...)` if threshold exceeded.

**Key APIs:** `atomic_read()`

---

### #72 Degraded Mode Flag

> Atomic flag indicating whether system is in degraded mode.

**Implementation:** `ctx->deg.is_degraded`, a `bool` or `atomic_t` flag. Used as condition in `tv_log()` output and `tieredvol_status()`. Manually resettable via `clear_degraded` command.

**Key APIs:** None (state flag)

---

### #73 Degraded Mode Recovery

> Manually clears degraded mode flag via `clear_degraded` command.

**Implementation:** Message command #44 (`clear_degraded`). Sets `ctx->deg.is_degraded = false` and logs. Auto-degradation will re-trigger on next threshold check if errors persist.

**Key APIs:** None (state machine)

---

## 12. Rebuild Management

Rebuild subsystem provides background data reconstruction using kthread and exponential backoff retry mechanism.

### #74 kthread-based Background Rebuild

> Uses kernel thread for background data reconstruction without blocking I/O path.

**Implementation:** `start_rebuild` command (#45) creates `kthread_run(tv_rebuild_thread, ...)`. Rebuild thread processes data with configurable `max_bytes` per iteration. `stop_rebuild` command (#46) sets stop flag and waits for kthread exit via `kthread_stop()`.

**Key APIs:** `kthread_run()`, `kthread_stop()`, `kthread_should_stop()`

---

### #75 Exponential Backoff Retry

> Retries failed rebuilds with exponential backoff strategy, avoiding tight retry loops.

**Implementation:** Retry logic in `tv_rebuild_thread()`. Uses `ctx->retry_ctx_pool` (`mempool`) for retry context allocation. Wait time grows exponentially after failure (initial 1ms, max 30s), then retries. Uses `schedule_timeout_interruptible()` for waiting.

**Key APIs:** `mempool_alloc()`, `mempool_free()`, `schedule_timeout_interruptible()`

---

### #76 Rebuild Progress Tracking

> Tracks rebuild progress: bytes processed, total bytes, status (running/completed/stopped).

**Implementation:** `ctx->rebuild` structure (`struct tv_rebuild_state`). Contains `is_running`, `is_complete`, `bytes_processed`, `total_bytes` fields. Queryable via `show_rebuild` command (#47).

**Key APIs:** None (state structure)

---

## 13. Sysfs Interface

v5.0 adds sysfs interface with 7 attributes at /sys/kernel/tieredvol/ for runtime query and configuration. Implementation in `tieredvol_sysfs.c` (273 lines).

### #77 policy (read-only)

> Current dispatch policy: static, adaptive, or random.

**Implementation:** `/sys/kernel/tieredvol/policy`. Read-only attribute returning text representation of `ctx->adaptive.policy`.

---

### #78 stale_ms (read-write)

> Stale detection timeout in milliseconds.

**Implementation:** `/sys/kernel/tieredvol/stale_ms`. Read returns `ctx->adaptive.stale_after_ns / 1000000`. Write sets `ctx->adaptive.stale_after_ns`.

---

### #79 wear_bias (read-write)

> Wear penalty factor (0-1024).

**Implementation:** `/sys/kernel/tieredvol/wear_bias`. Read returns `ctx->adaptive.wear_bias`. Write validates `<= 1024` then sets.

---

### #80 ema_shift (read-write)

> EMA weight shift (0-10).

**Implementation:** `/sys/kernel/tieredvol/ema_shift`. Read returns `ctx->adaptive.ema_weight_shift`. Write validates `<= 10` then sets.

---

### #81 loglevel (read-write)

> Log verbosity (0-3: OFF/ERROR/WARN/INFO).

**Implementation:** `/sys/kernel/tieredvol/loglevel`. Read returns `tv_log_level`. Write validates `<= 3` then sets.

---

### #82 disk_count (read-only)

> Number of disks.

**Implementation:** `/sys/kernel/tieredvol/disk_count`. Read-only attribute returning `ctx->ndisks`.

---

### #83 status (read-only)

> Comprehensive status string.

**Implementation:** `/sys/kernel/tieredvol/status`. Read-only attribute returning simplified status string similar to `dmsetup status`.

---

## Appendix: References

### Academic Papers

| Paper | Venue | Relevance |
|-------|-------|-----------|
| Jiao & Kim, "Asymmetric RAID: Rethinking RAID for SSD Heterogeneity" | HotStorage'24 | Original Asym-RAID design; TieredVol extends with multi-factor adaptive dispatch |

### Linux Kernel Sources

| File | Link | Referenced Features |
|------|------|---------------------|
| `drivers/md/dm-stripe.c` | [link](https://github.com/torvalds/linux/blob/master/drivers/md/dm-stripe.c) | #1 |
| `drivers/md/dm-switch.c` | [link](https://github.com/torvalds/linux/blob/master/drivers/md/dm-switch.c) | #1, #2, #3 |
| `drivers/md/dm-crypt.c` | [link](https://github.com/torvalds/linux/blob/master/drivers/md/dm-crypt.c) | #4, #48 |
| `drivers/md/dm-raid1.c` | [link](https://github.com/torvalds/linux/blob/master/drivers/md/dm-raid1.c) | #6, #12, #15, #20, #35 |
| `drivers/md/dm-dust.c` | [link](https://github.com/torvalds/linux/blob/master/drivers/md/dm-dust.c) | #12, #28, #63 |
| `drivers/md/dm-thin.c` | [link](https://github.com/torvalds/linux/blob/master/drivers/md/dm-thin.c) | #7, #58 |
| `drivers/md/dm-linear.c` | [link](https://github.com/torvalds/linux/blob/master/drivers/md/dm-linear.c) | #4 |
| `drivers/md/dm.c` | [link](https://github.com/torvalds/linux/blob/master/drivers/md/dm.c) | #5, #16, #20 |
| `drivers/md/dm-log.c` | [link](https://github.com/torvalds/linux/blob/master/drivers/md/dm-log.c) | #13, #58 |
| `drivers/md/dm-log-writes.c` | [link](https://github.com/torvalds/linux/blob/master/drivers/md/dm-log-writes.c) | #61, #63 |
| `drivers/md/dm-thin-metadata.c` | [link](https://github.com/torvalds/linux/blob/master/drivers/md/dm-thin-metadata.c) | #58 |
| `drivers/md/dm-table.c` | [link](https://github.com/torvalds/linux/blob/master/drivers/md/dm-table.c) | #59 |
| `block/kyber-iosched.c` | [link](https://github.com/torvalds/linux/blob/master/block/kyber-iosched.c) | #2 |
| `block/mq-deadline.c` | [link](https://github.com/torvalds/linux/blob/master/block/mq-deadline.c) | #2, #10 |
| `block/blk-mq.c` | [link](https://github.com/torvalds/linux/blob/master/block/blk-mq.c) | #10, #17 |
| `kernel/time/timer_list.c` | [link](https://github.com/torvalds/linux/blob/master/kernel/time/timer_list.c) | #11 |
| `kernel/sched/fair.c` | [link](https://github.com/torvalds/linux/blob/master/kernel/sched/fair.c) | #7 |
| `kernel/trace/ring_buffer.c` | [link](https://github.com/torvalds/linux/blob/master/kernel/trace/ring_buffer.c) | #61 |
| `kernel/trace/trace.c` | [link](https://github.com/torvalds/linux/blob/master/kernel/trace/trace.c) | #62 |
| `kernel/samples/kfifo/record-example.c` | [link](https://github.com/torvalds/linux/blob/master/samples/kfifo/record-example.c) | #61 |

### Open-Source Projects

| Project | Link | Referenced Features |
|---------|------|---------------------|
| emlog (nicupavel) | https://github.com/nicupavel/emlog | #61 |
| sysprog21/kfifo-examples | https://github.com/sysprog21/kfifo-examples | #61 |
| mdadm | https://github.com/md-raid-utilities/mdadm | #59 |

---

## Test Coverage Summary

| Category | Features | Tested |
|----------|:--------:|:------:|
| I/O Dispatch | 6 | 6 |
| Load Balancing | 5 | 5 |
| Stale Detection | 4 | 4 |
| Per-disk Stats | 5 | 5 |
| DM Commands | 27 | 27 |
| DM Lifecycle | 7 | 7 |
| Status Reporting | 3 | 3 |
| Metadata | 3 | 3 |
| Structured Log | 3 | 3 |
| Mirror/Pending | 5 | 5 |
| Degradation | 5 | 5 |
| Rebuild | 3 | 3 |
| Sysfs | 7 | 7 |
| **Total** | **83** | **83** |
