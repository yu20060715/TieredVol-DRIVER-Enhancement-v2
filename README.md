# TieredVol-DRIVER-Enhancement

> **Research version.** All features developed here have been merged back to [TieredVol-DRIVER](https://github.com/yu20060715/TieredVol-DRIVER). This repo preserves the development history and roadmap.

A kernel-level Device Mapper target for weighted striping across heterogeneous storage devices.

TieredVol-DRIVER replaces the userspace io_uring I/O path with a kernel dm-target module (`tieredvol.ko`), achieving near-zero overhead. Applications interact with tiered storage through standard `write()`/`read()` syscalls on `/dev/mapper/<name>`.

```
Application
      │
      ▼
  write() / read()         ← Standard POSIX I/O
      │
      ▼
  VFS → bio                ← Kernel bio path
      │
      ▼
  tieredvol_submit()       ← Kernel module: weighted dispatch per-bio
      │
      ▼
  Disk A / Disk B / ...    ← Direct to underlying block devices
```

### What This Prototype Validates

- Kernel-level weighted bio dispatch (proportional to disk speed)
- Bio splitting for bios spanning multiple disk regions
- Segment-based mapping for unequal disk capacities
- Logical ↔ Physical offset mapping (zero-copy)
- Zero overhead from syscall/copy/CQE
- Mirror/RAID1 redundancy (bio_alloc_clone + fire-and-forget)
- Adaptive striping with EMA-based load balancing
- Staleness detection with grace period (timer-based monitoring)
- Wear leveling (write-count-aware weight adjustment)
- Per-disk I/O statistics (in-flight tracking, completion counters)
- Integrity (CRC32C), atomic (O_DIRECT), crypto (AES-256-XTS) passthrough
- Error detection with per-disk error_count and workqueue event notification

### Key Results (kernel dm-target v5.0, fio + io_uring + QD=256)

| Config | Write | Read | vs Raw | vs LVM |
|--------|-------|------|--------|--------|
| 4-disk [nvme,sdb,sdc,sdd] | **3325 MB/s** | **4063 MB/s** | 81%/99% | +136%/+122% |
| 3-disk [nvme,sdb,sdc] | 2893 MB/s | 3938 MB/s | 83%/100% | +111%/+174% |
| 2-disk [nvme,sdb] | 2179 MB/s | 3261 MB/s | 82%/97% | +129%/+238% |

Hardware: NVMe P3 Plus 1T (PCIe 3.0 x4, ~2783 MB/s), SATA MX500 (~481 MB/s), WD Blue (~492 MB/s), BX100 (~366 MB/s).

vs LVM: LVM fixed stripe 4-disk W 1407 / R 1829 MB/s. TieredVol weighted striping + adaptive EMA beats LVM by 100-238%.

### What Is Intentionally Excluded

- Filesystem implementation
- Data redundancy via parity (RAID5/6)
- Crash consistency / journaling
- Metadata recovery
- Dynamic online rebalancing
- Write cache (attempted and removed — bio_chain + flush_bypasses_map caused D-state hangs)

---

## Quick Start

```bash
git clone https://github.com/yu20060715/TieredVol-DRIVER.git
cd TieredVol-DRIVER

# Build userspace tools + kernel module
make
make module

# Create a weighted volume (loads kernel module automatically)
sudo ./tiered_setup --create --name fastpool --disks nvme0n1,sdb,sdc --scheduler

# Benchmark (expected: ~1500 MB/s)
sudo ./benchmark.sh

# Or manual fio:
sudo fio --name=bench --filename=/dev/mapper/fastpool --rw=write --bs=2m \
  --size=2G --direct=1 --ioengine=io_uring --iodepth=256 --numjobs=1 --end_fsync=1

# Remove
sudo ./tiered_setup --remove --name fastpool
```

---

## Weighted Striping vs Fixed-Size Striping

Traditional RAID0 and LVM striping use a fixed stripe size for all disks. When storage devices exhibit significantly different sequential bandwidth, fixed striping underutilizes faster devices.

```
Fixed striping (LVM):          Weighted striping (TieredVol):

NVMe  3100 MB/s → 1 chunk     NVMe  3100 MB/s → 7 chunks = 1792KB
SATA  1700 MB/s → 1 chunk     SATA  1700 MB/s → 4 chunks = 1024KB
SATA   800 MB/s → 1 chunk     SATA   800 MB/s → 2 chunks = 512KB
SATA   450 MB/s → 1 chunk     SATA   450 MB/s → 1 chunk  =  256KB

NVMe idle waiting for SATA     All disks finish at approximately
→ throughput ≈ slowest disk    the same time → higher aggregate
```

**Weight is generated at initialization** via a benchmark that measures sequential write speed with SLC cache warm-up (2GB pre-write).

---

## CLI Usage

### Tiered Storage (Kernel dm target)

```bash
# Create weighted volume (kernel module)
sudo tiered_setup --create --name fastpool --disks nvme0n1,sdb --scheduler

# Benchmark the volume
sudo tiered_io --path /dev/mapper/fastpool --bench --size 5GB
sudo tiered_io --path /dev/mapper/fastpool --bench-all

# Show volume metadata
sudo tiered_io --name fastpool --info

# Remove volume
sudo tiered_setup --remove --name fastpool
```

### LVM Striping (Legacy)

```bash
# Create LVM striped volume
sudo tiered_setup --create --name pool --disks sdb:300,sdc:200 --fs ext4 --mount /mnt/pool

# Benchmark
sudo tiered_io --path /dev/mapper/tv_vg_pool-tv_lv_pool --bench --size 5GB

# Remove
sudo tiered_setup --remove --name pool
```

### Disk Management

```bash
sudo tiered_setup --list         # List all disks
sudo tiered_setup --bench --disks sdb,sdc,nvme0n1  # Benchmark disks
sudo tiered_setup --status       # Show status
```

---

## Requirements

- Linux kernel 6.x (tested on 6.14.0-27)
- `lvm2` `dmsetup` `gcc` `make`
- `linux-headers-$(uname -r)` (for kernel module build)
- Root privileges (sudo)

### Install Dependencies

```bash
# Debian / Ubuntu
sudo apt install lvm2 gcc make linux-headers-$(uname -r)
```

## Build

```bash
make                    # Build tiered_setup + tiered_io
make test               # Unit tests (81 assertions, 4 suites, no sudo)
make module             # Build kernel module
sudo make module_install # Install kernel module
sudo depmod -a          # Update module dependencies
sudo make test-full     # Unit + integration tests
make clean              # Remove all build artifacts
sudo make install       # Install to /usr/local/bin/
```

---

## Project Structure

```
TieredVol-DRIVER-Enhancement-v2/
├── README.md
├── VERIFY.md                        # Verification results + benchmarks
├── Makefile
├── driver/                          # Kernel dm-target module
│   ├── tieredvol.h                  # Central header: all structs + exports
│   ├── tieredvol_core.c             # DM lifecycle: ctr/dtr/map/status/init/exit
│   ├── tieredvol_map.c              # Logical→Physical: static/adaptive/random
│   ├── tieredvol_mirror.c           # Mirror I/O + pending tracking + end_io
│   ├── tieredvol_log.c              # Log ring buffer + EMA decay timer
│   ├── tieredvol_meta.c             # Metadata read/write (config file)
│   ├── tieredvol_sysfs.c            # sysfs interface
│   ├── tieredvol_message.c          # DM message handler (show/set commands)
│   └── Makefile
├── src/                             # Userspace tools
│   ├── tiered_setup                 # CLI: create/setup/configure
│   ├── tieredvol_benchmark.c        # Benchmark utility
│   ├── tieredvol_partition.c        # Disk partitioning logic
│   └── cmd_create.c / cmd_scheduler.c / ...
├── tests/                           # Test suite
│   ├── test_*.sh                    # Shell integration tests (48)
│   └── test_*.c                     # C unit tests (81)
├── docs/
│   ├── USAGE.md                     # Usage tutorial
│   └── PARTITION_SPLITTING.md       # Weighted striping algorithm
├── plan/
│   └── asym-raid-comparison.md      # Academic comparison (Asym-RAID)
└── scripts/
    ├── test_scheduler.sh            # End-to-end test
    └── tieredvol-restore.sh         # Boot-time volume restore
```

### Kernel Module Architecture

| File | Responsibility |
|------|---------------|
| `tieredvol_core.c` | DM lifecycle, I/O entry (`tieredvol_map`), module init/exit |
| `tieredvol_map.c` | Logical→Physical mapping (static/adaptive/random dispatch) |
| `tieredvol_mirror.c` | Mirror I/O, pending tracking (per-CPU), timestamp ring, end_io handler |
| `tieredvol_log.c` | Log ring buffer, EMA decay timer (load/latency/IOPS tracking) |
| `tieredvol_meta.c` | Config file parse/save, CRC32 validation |
| `tieredvol_message.c` | DM message commands (show/set/modify at runtime) |
| `tieredvol_sysfs.c` | sysfs attributes |

### I/O Flow

```
User → write()/read() → VFS → bio → tieredvol_map()
  → tv_map_logical_adaptive()     # Multi-factor scoring: load + latency + wear
  → tv_ts_submit()                # Record submit timestamp
  → [mirror?] → bio_alloc_clone() → submit_bio(clone)
  → return DM_MAPIO_REMAPPED      # DM submits to physical disk
  ...
  → tieredvol_end_io()            # Completion: latency delta, in_flight--
    → tv_ts_complete()            # Calculate latency
    → [mirror retry?] → schedule_delayed_work()
```

---

## Kernel Module (v5.0)

The `tieredvol` dm target processes bios in-kernel:

1. **bio arrives** at the dm target (from VFS `write()`/`read()`)
2. **Map**: `tv_map_logical_adaptive()` translates logical byte offset → (disk, physical_offset)
3. **Scoring**: multi-factor score = EMA load + EMA latency + wear penalty (lower = better)
4. **Mirror**: if enabled, `bio_alloc_clone()` + fire-and-forget to redundant disk
5. **Redirect**: `bio_set_dev()` + sector update → DM core submits to underlying device

Key features:
- `DM_TARGET_NOWAIT`: Non-blocking bio dispatch
- `flush_bypasses_map`: Flush FUA bios bypass the map function
- `dm_set_target_max_io_len()`: Bio splitting at chunk boundaries
- **Adaptive EMA dispatch**: load + latency + wear scoring per bio
- **Per-CPU pending arrays**: lockless write path for mirror tracking
- **Per-disk timestamp ring**: latency tracking for adaptive scoring
- **Mempool**: zero OOM for mirror/retry contexts
- Mirror/RAID1: per-segment `mirror_enabled` + `mirror_disk` config
- Staleness detection: adaptive timer (100ms busy / 1s idle), grace period
- CRC32 config validation (two-pass parse)
- Error detection with per-disk error_count and degraded mode

Key constants:
- `TV_MAX_DISKS` = 8
- `TV_MAX_SEGS` = 8

### Metadata Format

Config files are stored in `/etc/tieredvol/<name>.conf` (INI format):

```ini
[weighted_striping]
version=1
chunk_size=1048576
segment_count=1
disk_count=2
disk0_name=/dev/nvme0n1
disk1_name=/dev/sdb
seg0_begin=0
seg0_end=931520000000
seg0_count=2
seg0_disks=0,1
seg0_weight=2,1
seg0_stripe=3145728
seg0_mirror=1          # optional: mirror to disk index 1
seg0_policy=adaptive   # optional: static (default), adaptive, random
```

---

## Limitations

- **Static weights only by default** — Adaptive striping available via `set_policy` message.
- **No parity-based redundancy** — Mirror/RAID1 supported, but no RAID5/6.
- **No crash consistency** — No journaling or metadata recovery.
- **System disk cannot be used** — dm returns EBUSY on mounted root partition.
- **Module instability risk** — A kernel module bug can oops the system.
- **NVMe write cache must stay ON** — Disabling it causes -21% throughput loss.

## License

MIT
