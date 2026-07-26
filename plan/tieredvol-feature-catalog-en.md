# TieredVol v4.6.0 Feature Catalog

Technical reference for all implemented features in the TieredVol device-mapper target.

---

## 1. I/O Dispatch

The I/O dispatch layer translates logical byte offsets into physical disk assignments. TieredVol supports three dispatch policies, each selecting a disk within the appropriate segment and computing the physical sector offset.

### #1 Static Weighted Dispatch

> Deterministically maps logical offsets to disks using pre-computed weighted stripe boundaries.

**Implementation:** `tv_map_logical()` in `driver/tieredvol_map.c:4-65`. Builds a prefix-sum boundary array from segment weights, then performs a linear scan to locate the target disk. Physical offset is computed as `stripe_no * weight[disk] * CHUNK_SIZE + (offset_in - boundary[disk])`.

**Core APIs:** `bio_set_dev()`, `bdev_nr_sectors()`

**References:**
1. `drivers/md/dm-stripe.c` — Classic striped target with weighted distribution
2. `drivers/md/dm-switch.c` — Region-based bio dispatch with sector remapping

---

### #2 Adaptive EMA Dispatch

> Selects the least-loaded disk per bio using an Exponential Moving Average load score with wear penalty, automatically skipping stale disks.

**Implementation:** `tv_map_logical_adaptive()` in `driver/tieredvol_map.c:67-156`. Iterates candidate disks, computes `load = ema_load[d] + wear_bias * total_write_bytes[d] / total_writes`, and picks the minimum. Falls back to any valid disk if all candidates are stale.

**Core APIs:** `get_random_u32()`, atomic EMA update via `tv_decay_timer_fn()`

**References:**
1. `block/kyber-iosched.c` — Token-based dynamic depth adjustment
2. `block/mq-deadline.c` — Request sorting with time-based expiry

---

### #3 Random Dispatch

> Selects a disk uniformly at random within the segment.

**Implementation:** `tv_map_logical_random()` in `driver/tieredvol_map.c:158-201`. Uses `get_random_u32() % seg->disk_count` for uniform selection.

**Core APIs:** `get_random_u32()`

**References:**
1. `drivers/md/dm-switch.c` — Random path selection fallback

---

### #4 Bio Sector Remapping

> Redirects a bio from the DM virtual device to the correct physical disk by rewriting `bi_bdev` and `bi_iter.bi_sector`.

**Implementation:** `tieredvol_map()` in `driver/tieredvol_core.c:156-238`. Calls `bio_set_dev(bio, ctx->devs[cur.disk]->bdev)` and sets `bio->bi_iter.bi_sector = cur.offset >> SECTOR_SHIFT`.

**Core APIs:** `bio_set_dev()`, `SECTOR_SHIFT`

**References:**
1. `drivers/md/dm-crypt.c` — Bio clone + redirect to encryption layer
2. `drivers/md/dm-linear.c` — Simplest bio remapping target

---

### #5 Invalid Disk Error

> Returns a bio error when the computed disk index is out of range.

**Implementation:** `tieredvol_map()` at `driver/tieredvol_core.c:182-189`. Checks `cur.disk < 0 || cur.disk >= ctx->ndisks`, then calls `bio_io_error(bio)` and returns `DM_MAPIO_SUBMITTED`.

**Core APIs:** `bio_io_error()`

**References:**
1. `drivers/md/dm.c` — DM core error handling patterns

---

### #6 Write Mirroring

> Clones a write bio and submits the clone to a designated mirror disk for data redundancy.

**Implementation:** `tieredvol_map()` at `driver/tieredvol_core.c:206-234`. When `seg->mirror_enabled` and the mirror disk differs from the primary, calls `bio_alloc_clone()` with `&fs_bio_set`, sets a custom `tv_mirror_end_io` completion handler, and submits via `submit_bio()`.

**Core APIs:** `bio_alloc_clone()`, `submit_bio()`, `bio_put()`

**References:**
1. `drivers/md/dm-raid1.c` — Bio clone + dual-disk write + custom bi_end_io

---

## 2. Load Balancing + Time Decay

Load balancing tracks per-disk I/O pressure using in-flight byte counters and an EMA smoothing filter. A 1-second hardware timer periodically decays these counters, providing a smoothed load estimate for the adaptive dispatch policy.

### #7 EMA Load Calculation

> Computes per-disk load using an Exponential Moving Average: `ema = ema * (1 - alpha) + snapshot * alpha`, where alpha is configurable via `ema_weight_shift`.

**Implementation:** `tv_decay_timer_fn()` in `driver/tieredvol_core.c:92-142`. Alpha defaults to `1 << 3 = 8` out of 1024 (shift=3). The snapshot is the atomic in-flight byte counter, reset to zero each tick via `atomic_xchg()`.

**Core APIs:** `timer_list`, `atomic_xchg()`

**References:**
1. `kernel/sched/fair.c` — CFS per-CPU load tracking
2. `drivers/md/dm-thin.c` — Pool mode switching with low watermark callback

---

### #8 In-flight Byte Tracking

> Counts bytes currently in transit to each disk using per-disk atomic counters.

**Implementation:** `tieredvol_map()` at `driver/tieredvol_core.c:193` increments `atomic_add(bio->bi_iter.bi_size, &ctx->in_flight_bytes[cur.disk])`. The decay timer resets counters to zero each second via `atomic_xchg()`.

**Core APIs:** `atomic_add()`, `atomic_xchg()`

**References:**
1. `block/blk-mq.c` — blk-mq per-tag statistics

---

### #9 1-second Decay Timer

> Fires a hardware timer every HZ ticks to decay load counters and check stale detection.

**Implementation:** `timer_setup(&ctx->decay_timer, tv_decay_timer_fn, 0)` at `driver/tieredvol_core.c:317-318`. The timer re-arms itself at the end of each tick via `mod_timer()`.

**Core APIs:** `timer_setup()`, `mod_timer()`, `timer_delete_sync()`

**References:**
1. `kernel/time/timer_list.c` — timer_list API reference

---

### #10 Wear-bias Penalty

> Adds a write-amplification penalty to the load score: `load += wear_bias * total_write_bytes[d] / total_writes`.

**Implementation:** `tv_map_logical_adaptive()` at `driver/tieredvol_map.c:106-122`. When `wear_bias > 0`, each disk's load is penalized proportional to its share of total writes, discouraging further writes to heavily-worn disks.

**Core APIs:** None (pure arithmetic)

**References:**
1. `block/mq-deadline.c` — Write penalty heuristics

---

## 3. Stale Disk Detection + Recovery

Stale detection identifies disks that have stopped responding to I/O. A disk is marked stale after a configurable timeout, then automatically recovered when I/O resumes or after a cooldown period. A grace period prevents immediate re-staling after recovery.

### #11 Stale Marking

> Marks a disk as stale when no I/O completes within `stale_after_ns` (default 5 seconds).

**Implementation:** `tv_decay_timer_fn()` at `driver/tieredvol_core.c:112-123`. Checks `now > ctx->grace_until_ns[i]` and `(now - ctx->last_finish_ns[i]) > ctx->stale_after_ns`. Sets `ctx->stale[i] = true` and logs via `tv_log(TV_LOG_WARN, ...)`.

**Core APIs:** `ktime_get_boottime_ns()`

**References:**
1. `drivers/md/dm-dust.c` — Bad block simulation with enable/disable
2. `drivers/md/dm-raid1.c` — Mirror failover + error detection

---

### #12 Stale Recovery (I/O Triggered)

> Immediately recovers a stale disk when new I/O completes on it, and starts a new grace period.

**Implementation:** `tv_decay_timer_fn()` at `driver/tieredvol_core.c:124-129`. When `ctx->stale[i] && snapshot > 0`, sets `stale[i] = false` and `grace_until_ns[i] = now + stale_after_ns`.

**Core APIs:** None (state machine)

**References:**
1. `drivers/md/dm-log.c` — Dirty region tracking with recovery

---

### #13 Stale Recovery (Cooldown)

> Automatically recovers a stale disk after 2x the stale timeout, even without new I/O.

**Implementation:** `tv_decay_timer_fn()` at `driver/tieredvol_core.c:130-138`. When `(now - ctx->stale_marked_ns[i]) > 2 * ctx->stale_after_ns`, sets `stale[i] = false`.

**Core APIs:** None (state machine)

**References:**
1. `block/disk-events.c` — Disk event polling with timeout

---

### #14 Grace Period

> Protects a newly-recovered disk from being immediately re-marked stale.

**Implementation:** `tv_decay_timer_fn()` at `driver/tieredvol_core.c:126,134`. Sets `ctx->grace_until_ns[i] = now + ctx->stale_after_ns` on recovery. The stale check at line 115 verifies `now > ctx->grace_until_ns[i]` before marking stale.

**Core APIs:** None (state machine)

**References:**
1. `drivers/md/dm-raid1.c` — Mirror recovery grace period

---

## 4. Per-Disk I/O Statistics

Cumulative counters for each physical disk, tracked across the lifetime of the device-mapper target.

### #15 Read Bytes Counter

> Total bytes read from each disk.

**Implementation:** `ctx->total_read_bytes[cur.disk] += bio->bi_iter.bi_size` at `driver/tieredvol_core.c:198`.

**References:** `drivers/md/dm.c` — DM core bio stats

---

### #16 Read Ops Counter

> Total read operations per disk.

**Implementation:** `ctx->total_read_ops[cur.disk]++` at `driver/tieredvol_core.c:199`.

**References:** `block/blk-mq.c` — blk-mq per-tag stats

---

### #17 Write Bytes Counter

> Total bytes written to each disk.

**Implementation:** `ctx->total_write_bytes[cur.disk] += bio->bi_iter.bi_size` at `driver/tieredvol_core.c:195`.

**References:** `drivers/nvme/host/core.c` — NVMe per-controller stats

---

### #18 Write Ops Counter

> Total write operations per disk.

**Implementation:** `ctx->total_write_ops[cur.disk]++` at `driver/tieredvol_core.c:196`.

**References:** `block/genhd.c` — `part_stat_show` per-partition stats

---

### #19 Error Counter

> Atomic error count per disk, incremented on bio completion errors.

**Implementation:** `ctx->error_count[disk]` is an `atomic_t` array allocated in `tieredvol_ctr()` at `driver/tieredvol_core.c:299`. Read via `atomic_read()` in `tieredvol_status()`.

**Core APIs:** `atomic_read()`, `atomic_set()`

**References:**
1. `drivers/md/dm.c` — DM error counting
2. `drivers/md/dm-raid1.c` — Mirror error tracking

---

## 5. Per-CPU Global Statistics

Lock-free global counters using `DEFINE_PER_CPU` for high-throughput I/O tracking without cache-line contention.

### #20 Per-CPU Map Count

> Counts total bio mapping operations across all CPUs.

**Implementation:** `DEFINE_PER_CPU(u64, tv_map_count)` at `driver/tieredvol_core.c:25`. Incremented via `this_cpu_inc(tv_map_count)` at line 201.

**Core APIs:** `this_cpu_inc()`, `DEFINE_PER_CPU()`

**References:**
1. `include/linux/percpu_counter.h` — percpu_counter API

---

### #21 Per-CPU Sector Count

> Counts total sectors dispatched across all CPUs.

**Implementation:** `DEFINE_PER_CPU(u64, tv_map_sectors)` at `driver/tieredvol_core.c:26`. Accumulated via `this_cpu_add(tv_map_sectors, bio_sectors(bio))` at line 202.

**Core APIs:** `this_cpu_add()`, `bio_sectors()`

**References:**
1. `kernel/sched/fair.c` — CFS per-CPU load tracking

---

### #22 Per-CPU Byte Count

> Counts total bytes dispatched across all CPUs.

**Implementation:** `DEFINE_PER_CPU(u64, tv_map_bytes)` at `driver/tieredvol_core.c:27`. Accumulated via `this_cpu_add(tv_map_bytes, bio->bi_iter.bi_size)` at line 203.

**Core APIs:** `this_cpu_add()`

**References:**
1. `include/linux/mm_types.h` — vm_stat per-CPU counters

---

### #23 Cross-CPU Aggregation

> Sums per-CPU counters across all possible CPUs for global totals.

**Implementation:** `tv_read_count()`, `tv_read_sectors()`, `tv_read_bytes()` at `driver/tieredvol_core.c:63-88`. Each iterates `for_each_possible_cpu(cpu)` and sums `per_cpu(counter, cpu)`.

**Core APIs:** `for_each_possible_cpu()`, `per_cpu()`

**References:**
1. `drivers/md/dm-thin.c` — Pool per-CPU deferred set

---

## 6. DM Message Commands

Runtime control interface via `dmsetup message`. Each command is dispatched in `tieredvol_message()` at `driver/tieredvol_core.c:520-808`.

### #24 `reset_stats`

> Zeros all per-CPU statistics (map count, sector count, byte count).

**Implementation:** `driver/tieredvol_core.c:523-532`. Iterates `for_each_possible_cpu(cpu)` and sets each counter to 0.

---

### #25 `show_stats`

> Returns maps count, average bytes per map, and total bytes via dmesg.

**Implementation:** `driver/tieredvol_core.c:533-542`. Computes `avg = total_bytes / maps_count`.

---

### #26 `status`

> Returns each disk name and its weight within its segment.

**Implementation:** `driver/tieredvol_core.c:543-568`. Scans all segments to find the weight for each disk.

---

### #27 `show_inflight`

> Returns current in-flight bytes for each disk.

**Implementation:** `driver/tieredvol_core.c:569-581`. Reads `atomic_read(&ctx->in_flight_bytes[i])`.

---

### #28 `adaptive_on`

> Switches dispatch policy to adaptive (EMA-based load balancing).

**Implementation:** `driver/tieredvol_core.c:582-589`. Sets `ctx->policy = TV_POLICY_ADAPTIVE`.

---

### #29 `adaptive_off`

> Switches dispatch policy to static (weighted boundary-based).

**Implementation:** `driver/tieredvol_core.c:590-597`. Sets `ctx->policy = TV_POLICY_STATIC`.

---

### #30 `set_policy <name>`

> Sets dispatch policy to static, adaptive, or random.

**Implementation:** `driver/tieredvol_core.c:598-612`. Validates `argv[1]` against "static", "adaptive", "random".

---

### #31 `set_ema_shift <shift>`

> Sets EMA weight shift (0-10). Alpha = `1 << shift` out of 1024.

**Implementation:** `driver/tieredvol_core.c:613-624`. Validates shift <= 10 via `kstrtou32()`. **Bug fixed:** originally checked `argc == 1` but read `argv[1]`, causing kernel oops. Fixed to `argc == 2`.

**Core APIs:** `kstrtou32()`

---

### #32 `set_stale_ms <ms>`

> Sets stale detection timeout in milliseconds.

**Implementation:** `driver/tieredvol_core.c:625-635`. Converts ms to ns: `ctx->stale_after_ns = (u64)ms * 1000000ULL`.

---

### #33 `show_adaptive`

> Returns policy, EMA shift, stale timeout, wear bias, and per-disk load/wear/stale status.

**Implementation:** `driver/tieredvol_core.c:636-656`. Outputs a comprehensive status string.

---

### #34 `show_wear`

> Returns wear bias and per-disk total write bytes.

**Implementation:** `driver/tieredvol_core.c:657-671`.

---

### #35 `show_io_stats`

> Returns per-disk read/write ops and bytes.

**Implementation:** `driver/tieredvol_core.c:672-688`.

---

### #36 `reset_io_stats`

> Zeros all per-disk I/O statistics (read/write bytes/ops).

**Implementation:** `driver/tieredvol_core.c:689-701`.

---

### #37 `set_wear_bias <bias>`

> Sets wear penalty factor (0-1024). Higher values penalize worn disks more in adaptive dispatch.

**Implementation:** `driver/tieredvol_core.c:702-712`. Validates `bias <= 1024`.

---

### #38 `reset_wear`

> Zeros per-disk write bytes (wear counters).

**Implementation:** `driver/tieredvol_core.c:713-722`.

---

### #39 `show_mirror`

> Returns mirror write ops/bytes, error count, and per-segment mirror configuration.

**Implementation:** `driver/tieredvol_core.c:723-746`.

---

### #40 `set_mirror <seg> <disk>`

> Enables mirroring for a segment, designating a target disk.

**Implementation:** `driver/tieredvol_core.c:747-763`. Validates `seg_idx < segment_count` and `disk_idx < ndisks`. Sets `seg->mirror_enabled = true` and `seg->mirror_disk = disk_idx`.

---

## 7. DM Target Lifecycle

Standard device-mapper target callbacks that manage the lifecycle of the tieredvol target.

### #41 Constructor (ctr)

> Allocates context, loads metadata from config file, acquires DM devices, and starts the decay timer.

**Implementation:** `tieredvol_ctr()` at `driver/tieredvol_core.c:246-408`. Flow:
1. Parse args (expects 1 argument: config file path)
2. `kzalloc(sizeof(*ctx))` — allocate context
3. `tv_metadata_load_kernel()` — parse key=value config file via `filp_open()` + `kernel_read()`
4. `kcalloc(ndisks, sizeof(*ctx->devs))` — allocate device arrays
5. `dm_get_device()` — acquire each DM device
6. Compute `min_chunk_sectors` across all segments
7. `dm_set_target_max_io_len()` — set max I/O size
8. `timer_setup()` + `mod_timer()` — start decay timer

**Core APIs:** `kzalloc()`, `kcalloc()`, `dm_get_device()`, `dm_set_target_max_io_len()`, `timer_setup()`

---

### #42 Destructor (dtr)

> Stops the decay timer, flushes pending work, releases DM devices, and frees all memory.

**Implementation:** `tieredvol_dtr()` at `driver/tieredvol_core.c:410-425`. Flow:
1. `timer_delete_sync()` — stop decay timer
2. `flush_work()` — complete pending trigger_event work
3. `dm_put_device()` — release each DM device
4. `kfree()` — free all allocations

**Core APIs:** `timer_delete_sync()`, `flush_work()`, `dm_put_device()`, `kfree()`

---

### #43 IO Hints

> Reports block size, chunk size, and optimal I/O size to the DM framework.

**Implementation:** `tieredvol_io_hints()` at `driver/tieredvol_core.c:438-448`. Sets `logical_block_size = 512`, `physical_block_size = 512`, `chunk_sectors = min_chunk_sectors`, `io_opt = stripe_sectors`.

**Core APIs:** `struct queue_limits`

---

### #44 Iterate Devices

> Returns all underlying DM devices for status reporting and ioctl passthrough.

**Implementation:** `tieredvol_iterate_devices()` at `driver/tieredvol_core.c:450-463`. Iterates `ctx->devs[0..ndisks-1]` and calls the callback for each.

**Core APIs:** `iterate_devices_callout_fn`, `bdev_nr_sectors()`

---

### #45 Prepare Ioctl

> Returns the first underlying device's block device for ioctl passthrough.

**Implementation:** `tieredvol_prepare_ioctl()` at `driver/tieredvol_core.c:427-436`. Sets `*bdev = ctx->devs[0]->bdev`.

**Core APIs:** `struct block_device`

---

### #46 Flush/Discard Propagation

> Propagates flush and discard commands to all underlying disks.

**Implementation:** `tieredvol_ctr()` at `driver/tieredvol_core.c:390-392`. Sets `ti->num_flush_bios = ctx->ndisks` and `ti->num_discard_bios = ctx->ndisks`. Also sets `ti->flush_bypasses_map = true`.

---

### #47 Module Init/Exit

> Registers the DM target with the kernel and creates a workqueue.

**Implementation:** `tieredvol_init()` at `driver/tieredvol_core.c:828-847`. Calls `dm_register_target()` and `alloc_workqueue("tieredvol_wq", WQ_UNBOUND | WQ_HIGHPRI, 0)`. Exit calls `dm_unregister_target()` and `destroy_workqueue()`.

**Core APIs:** `dm_register_target()`, `dm_unregister_target()`, `alloc_workqueue()`

---

## 8. Status Reporting

Status output via `dmsetup status`, providing runtime visibility into the target's state.

### #48 STATUSTYPE_INFO

> Returns policy, mirror stats, error count, and per-disk active/degraded status with read/write counters.

**Implementation:** `tieredvol_status()` at `driver/tieredvol_core.c:465-518` (STATUSTYPE_INFO case). Format: `policy=N mirror=ops/bytes err=N Ddisk:rd=ops/bytes wr=ops/bytes`.

---

### #49 STATUSTYPE_TABLE

> Returns the list of underlying disk device names.

**Implementation:** `driver/tieredvol_core.c:500-513` (STATUSTYPE_TABLE case). Space-separated disk names.

---

### #50 STATUSTYPE_IMA

> IMA (Integrity Measurement Architecture) placeholder — returns empty string.

**Implementation:** `driver/tieredvol_core.c:514-516`. Sets `result[0] = '\0'`.

---

## 9. Metadata Parsing

Kernel-space configuration file parser that loads the tieredvol topology from a key=value text file.

### #51 Kernel File-based Config

> Reads a configuration file from kernel space using `filp_open()` + `kernel_read()`.

**Implementation:** `tv_metadata_load_kernel()` in `driver/tieredvol_meta.c:93-272`. Opens the file with `filp_open(path, O_RDONLY, 0)`, reads up to 1MB via `kernel_read()`, then parses line by line.

**Core APIs:** `filp_open()`, `kernel_read()`, `i_size_read()`, `vmalloc()`, `vfree()`

**References:**
1. `drivers/md/dm-thin-metadata.c` — Complex metadata management (B-tree + superblock)
2. `drivers/md/dm-log.c` — Disk log state persistence

---

### #52 Version/Chunk/Segment/Disk Parsing

> Parses and validates version, chunk_size, segment_count, disk_count, and disk names.

**Implementation:** `driver/tieredvol_meta.c:150-187`. Uses `parse_u32()` / `parse_u64()` helpers. Validates `disk_count <= TV_MAX_DISKS` and `segment_count <= TV_MAX_SEGS`.

**Core APIs:** `kstrtoul()`, `kstrtoull()`

---

### #53 Segment Disks/Weight CSV Parsing

> Parses comma-separated u32 arrays for segment disk indices and weights.

**Implementation:** `parse_csv_u32()` in `driver/tieredvol_meta.c:54-70`. Uses `strsep(&s, ",")` to tokenize. Also parses `seg_begin`, `seg_end`, `seg_stripe`, `seg_mirror` via `parse_num_prefix()`.

**Core APIs:** `strsep()`, `parse_num_prefix()`

**References:**
1. `fs/configfs/configfs.c` — Kernel-userspace config interface
2. `drivers/md/dm-table.c` — DM table parsing

---

## 10. Hot-plug + Dynamic Detection (Future Work)

> **Status: Planned, not yet implemented.** These features are identified as core objectives but require significant engineering effort. The recommended approach uses DM table reload (dmsetup suspend/resume) for safe reconfiguration.

### #54 Online Add

> Dynamically expand a segment when a new disk is added.

**Planned approach:** `dmsetup suspend` → rebuild metadata → `dmsetup resume`. Reference: `mdadm/Grow_Add_device()`.

**References:**
1. `mdadm/Grow.c` — Linear array hot-add (`Grow_Add_device`)
2. `drivers/md/dm.c` — DM device dynamic creation (`dev_create`)

---

### #55 Online Remove

> Migrate stripe data before removing a disk.

**Planned approach:** Kernel thread copies data from source disk to surviving disks, then DM table reload. Reference: `mdadm --remove`.

**References:**
1. `mdadm/Grow.c` — Online remove with data migration

---

### #56 uevent Listener

> Detect disk add/remove events via netlink uevent.

**Planned approach:** Register a netlink listener for `block` subsystem events.

**References:**
1. `lib/kobject_uevent.c` — uevent netlink broadcast

---

### #57 sysfs Monitor

> Detect disk speed degradation via sysfs polling.

**Planned approach:** Poll `/sys/block/<dev>/stat` for performance changes.

**References:**
1. `block/disk-events.c` — Disk event polling framework

---

## 11. Structured Diagnostic Logging

A kernel-space ring buffer that records I/O, stale, mirror, and configuration events with timestamps and severity levels. Events are queried via `dmsetup message`.

### #58 Ring Buffer

> Fixed-size `kfifo` ring buffer (512 entries) with spinlock protection, recording timestamped log entries.

**Implementation:** `driver/tieredvol_core.c:29-54`. `DECLARE_KFIFO(tv_log_fifo, struct tv_log_entry, TV_LOG_SIZE)` with `DEFINE_SPINLOCK(tv_log_lock)`. Each `tv_log()` call acquires the spinlock with `spin_lock_irqsave()`, writes a `struct tv_log_entry` (64 bytes: timestamp_ns, level, disk_idx, event_type, msg[48]), and releases. Overflow overwrites oldest entries.

**Data structure:** `struct tv_log_entry` in `driver/tieredvol.h:98-104`.

**Core APIs:** `DECLARE_KFIFO()`, `kfifo_put()`, `kfifo_get()`, `kfifo_reset()`, `spin_lock_irqsave()`

**References:**
1. emlog (nicupavel) — Ring buffer architecture with overflow overwrite
   https://github.com/nicupavel/emlog
2. `kernel/samples/kfifo/record-example.c` — kfifo usage patterns
3. `kernel/trace/ring_buffer.c` — High-performance lockless ring buffer

---

### #59 Log Level

> Dynamic verbosity control: OFF(0) / ERROR(1) / WARN(2) / INFO(3).

**Implementation:** `driver/tieredvol_core.c:799-807`. `set_loglevel <0-3>` sets the global `tv_log_level`. The `tv_log()` function at line 39 checks `if (level > tv_log_level) return;`.

**Core APIs:** `kstrtou32()`

**References:**
1. `kernel/trace/trace.c` — Trace event management with log level control

---

### #60 dmsetup Query (show_log / clear_log)

> Real-time log query via `show_log` (dumps entries to dmesg) and `clear_log` (resets the ring buffer).

**Implementation:** `driver/tieredvol_core.c:764-798`.
- `show_log`: Drains the kfifo under spinlock, prints each entry to dmesg with format `LOG {ERR|WRN|INF} {I/O|STALE|RCVR|MIRR|CONF}: <msg>`.
- `clear_log`: Calls `kfifo_reset()` under spinlock.

**Core APIs:** `kfifo_get()`, `kfifo_reset()`, `DMINFO()`

**References:**
1. dm-dust — DM message query/result pattern
2. dm-log-writes — Structured I/O event logging

---

## Appendix: References

### Academic Papers

| Paper | Venue | Relevance |
|-------|-------|-----------|
| Jiao & Kim, "Asymmetric RAID: Rethinking RAID for SSD Heterogeneity" | HotStorage'24 | Original Asym-RAID design; TieredVol extends this with dynamic detection |

### Linux Kernel Sources

| File | URL | Features Referenced |
|------|-----|---------------------|
| `drivers/md/dm-stripe.c` | [link](https://github.com/torvalds/linux/blob/master/drivers/md/dm-stripe.c) | #1 |
| `drivers/md/dm-switch.c` | [link](https://github.com/torvalds/linux/blob/master/drivers/md/dm-switch.c) | #1, #2, #3 |
| `drivers/md/dm-crypt.c` | [link](https://github.com/torvalds/linux/blob/master/drivers/md/dm-crypt.c) | #4, #41 |
| `drivers/md/dm-raid1.c` | [link](https://github.com/torvalds/linux/blob/master/drivers/md/dm-raid1.c) | #6, #11, #14, #19, #39 |
| `drivers/md/dm-dust.c` | [link](https://github.com/torvalds/linux/blob/master/drivers/md/dm-dust.c) | #11, #30, #60 |
| `drivers/md/dm-thin.c` | [link](https://github.com/torvalds/linux/blob/master/drivers/md/dm-thin.c) | #7, #23, #30 |
| `drivers/md/dm-linear.c` | [link](https://github.com/torvalds/linux/blob/master/drivers/md/dm-linear.c) | #4 |
| `drivers/md/dm.c` | [link](https://github.com/torvalds/linux/blob/master/drivers/md/dm.c) | #5, #19, #54 |
| `drivers/md/dm-log.c` | [link](https://github.com/torvalds/linux/blob/master/drivers/md/dm-log.c) | #12, #51 |
| `drivers/md/dm-log-writes.c` | [link](https://github.com/torvalds/linux/blob/master/drivers/md/dm-log-writes.c) | #58 |
| `drivers/md/dm-thin-metadata.c` | [link](https://github.com/torvalds/linux/blob/master/drivers/md/dm-thin-metadata.c) | #51 |
| `drivers/md/dm-table.c` | [link](https://github.com/torvalds/linux/blob/master/drivers/md/dm-table.c) | #53 |
| `drivers/nvme/host/core.c` | [link](https://github.com/torvalds/linux/blob/master/drivers/nvme/host/core.c) | #17 |
| `block/kyber-iosched.c` | [link](https://github.com/torvalds/linux/blob/master/block/kyber-iosched.c) | #2 |
| `block/mq-deadline.c` | [link](https://github.com/torvalds/linux/blob/master/block/mq-deadline.c) | #2, #10 |
| `block/blk-mq.c` | [link](https://github.com/torvalds/linux/blob/master/block/blk-mq.c) | #8, #16 |
| `block/genhd.c` | [link](https://github.com/torvalds/linux/blob/master/block/genhd.c) | #18 |
| `block/disk-events.c` | [link](https://github.com/torvalds/linux/blob/master/block/disk-events.c) | #13, #57 |
| `kernel/time/timer_list.c` | [link](https://github.com/torvalds/linux/blob/master/kernel/time/timer_list.c) | #9 |
| `kernel/sched/fair.c` | [link](https://github.com/torvalds/linux/blob/master/kernel/sched/fair.c) | #7, #21 |
| `kernel/trace/ring_buffer.c` | [link](https://github.com/torvalds/linux/blob/master/kernel/trace/ring_buffer.c) | #58 |
| `kernel/trace/trace.c` | [link](https://github.com/torvalds/linux/blob/master/kernel/trace/trace.c) | #59 |
| `kernel/samples/kfifo/record-example.c` | [link](https://github.com/torvalds/linux/blob/master/samples/kfifo/record-example.c) | #58 |
| `include/linux/percpu_counter.h` | [link](https://github.com/torvalds/linux/blob/master/include/linux/percpu_counter.h) | #20 |
| `include/linux/mm_types.h` | [link](https://github.com/torvalds/linux/blob/master/include/linux/mm_types.h) | #22 |
| `include/linux/ring_buffer.h` | [link](https://github.com/torvalds/linux/blob/master/include/linux/ring_buffer.h) | #58 |
| `lib/kobject_uevent.c` | [link](https://github.com/torvalds/linux/blob/master/lib/kobject_uevent.c) | #56 |
| `fs/configfs/configfs.c` | [link](https://github.com/torvalds/linux/blob/master/fs/configfs/configfs.c) | #53 |

### Open-Source Projects

| Project | URL | Features Referenced |
|---------|-----|---------------------|
| emlog (nicupavel) | https://github.com/nicupavel/emlog | #58 |
| sysprog21/kfifo-examples | https://github.com/sysprog21/kfifo-examples | #58 |
| mdadm | https://github.com/md-raid-utilities/mdadm | #54, #55 |

---

## Test Coverage Summary

| Category | Features | Tested |
|----------|:--------:|:------:|
| I/O Dispatch | 6 | 6 |
| Load Balancing | 4 | 4 |
| Stale Detection | 4 | 4 |
| Per-Disk Stats | 5 | 5 |
| Per-CPU Stats | 4 | 4 |
| DM Messages | 17 | 17 |
| DM Lifecycle | 7 | 7 |
| Status Reporting | 3 | 3 |
| Metadata | 3 | 3 |
| Structured Logging | 3 | 3 |
| **Total** | **56** | **56** |

> Note: 4 hot-plug features (#54-57) are planned but not yet implemented.
