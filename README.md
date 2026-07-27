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

### Key Results (kernel dm-target v4.6.0, fio + io_uring + QD=256)

| Config | Write | Efficiency | vs Theory |
|--------|-------|------------|-----------|
| 3-disk [1,1,6] | **1499 MB/s** | 75% | 1499/1999 |
| 2-disk [1,7] | 1370 MB/s | 80% | 1370/1713 |
| 2-disk [1,6] | 1383 MB/s | 79% | 1383/1749 |

Hardware: i5-4570, NVMe CT1000P3PSSD8 (~1499 MB/s via PCIe 2.0 x4 adapter), SATA CT500MX500SSD1 (~517 MB/s), SATA WDC WDS250G2B0A (~536 MB/s). Both SATA on same Intel 8 Series/C220 controller (shared bus ~957 MB/s combined).

DM overhead: **<1%** (raw NVMe 1475 MB/s vs DM 1499 MB/s). The 25% gap from theoretical is primarily due to the B85 platform's PCIe 2.0 x4 bandwidth limit (~1600 MB/s practical) and multi-device scheduling overhead.

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
TieredVol-DRIVER-Enhancement/
├── README.md
├── plan.md                       # Development roadmap (historical)
├── MIGRATION.md                  # Migration plan from userspace to kernel
├── Makefile
├── driver/                       # Kernel dm-target module
│   ├── Kbuild                    # Kernel build configuration
│   ├── tieredvol.h               # Shared kernel types
│   ├── tieredvol_core.c          # dm_target ops, bio submission, completion
│   ├── tieredvol_map.c           # Logical → Physical offset mapping
│   └── tieredvol_meta.c          # Metadata loading from config file
├── src/
│   ├── main.c                    # CLI entry point (tiered_setup)
│   ├── tieredvol_common.h        # Input validation (name, fs, mount)
│   ├── tieredvol_types.h         # Shared type definitions
│   ├── version.h                 # Version string
│   ├── tieredvol_umapper.c       # Offset mapping (userspace)
│   ├── tieredvol_partition.c     # Weight + segment calculation
│   ├── tieredvol_umeta.c         # Metadata save/load (INI format)
│   ├── tieredvol_benchmark.c     # Initialization benchmark
│   ├── tieredvol_warmup.c / .h   # SLC cache warm-up
│   ├── tieredvol_exec.c / .h     # External command execution
│   ├── tieredvol_discover.c / .h # Disk discovery (lsblk, sysfs)
│   ├── tieredvol_bench.c / .h    # Setup benchmark logic
│   ├── cmd_create.c / .h         # Volume creation (kernel dm + LVM)
│   ├── cmd_remove.c / .h         # Volume removal
│   ├── cmd_status.c / .h         # Target status queries
│   └── cmd_scheduler.c / .h      # Scheduler (DM) volume creation
├── tests/
│   ├── test_common.c             # Input validation tests
│   ├── test_mapper.c             # Mapping tests
│   ├── test_partition.c          # Weight/segment tests
│   └── test_metadata.c           # Metadata round-trip tests
├── benchmarks/                   # Raw benchmark data and summary
├── docs/
│   ├── USAGE.md                  # Detailed usage guide
│   ├── PARTITION_SPLITTING.md    # Weighted striping algorithm
│   ├── WEIGHTED_IO_SCHEDULER.md  # I/O dispatch implementation
│   └── BENCHMARK-RESULTS.md      # Benchmark results on B85 platform
└── scripts/
    ├── install_deps.sh           # Dependency installer
    ├── test_scheduler.sh         # End-to-end test
    ├── tieredvol-restore.sh      # Boot-time volume restore
    └── tieredvol-restore.service # Systemd service
```

### Code Architecture

| Module | Responsibility |
|--------|---------------|
| **Kernel module** | |
| `driver/tieredvol_core.c` | dm_target ctr/dtr/map, bio splitting, weighted dispatch |
| `driver/tieredvol_map.c` | Logical → Physical offset mapping (kernel) |
| `driver/tieredvol_meta.c` | Metadata loading from config file (kernel) |
| **Userspace tools** | |
| `main.c` | CLI entry point: argument dispatch, dependency checks |
| `cmd_create.c` | Volume creation: kernel dm target + legacy LVM |
| `cmd_remove.c` | Volume removal: dmsetup remove + legacy teardown |
| `cmd_status.c` | Target status queries |
| `cmd_scheduler.c` | Scheduler (DM) volume creation |
| `tieredvol_umapper.c` | Logical ↔ Physical offset mapping (userspace) |
| `tieredvol_partition.c` | Weight calculation, capacity segmentation |
| `tieredvol_umeta.c` | Metadata save/load (INI format) |
| `tieredvol_benchmark.c` | Initialization benchmark |
| `tieredvol_warmup.c` | SLC cache warm-up |
| `tieredvol_exec.c` | External command execution for dmsetup/lvm |
| `tieredvol_discover.c` | Disk discovery: list, filter, detect partitions |
| `tieredvol_bench.c` | Setup benchmark: parallel speed testing |

---

## Kernel Module (v4.6.0)

The `tieredvol` dm target processes bios in-kernel:

1. **bio arrives** at the dm target (from VFS `write()`/`read()`)
2. **Map**: `tv_map_logical()` translates logical byte offset → (disk, physical_offset, remaining)
3. **Policy**: static weights, adaptive (EMA load + wear bias), or random per-segment
4. **Mirror**: if enabled, `bio_alloc_clone()` + fire-and-forget to redundant disk
5. **Redirect**: `bio_set_dev()` + sector update → DM core submits to underlying device

Key features:
- `DM_TARGET_NOWAIT`: Non-blocking bio dispatch (optimized for io_uring)
- `flush_bypasses_map`: Flush FUA bios bypass the map function
- `dm_set_target_max_io_len()`: Bio splitting at chunk boundaries
- Per-CPU statistics counters (zero contention)
- **Map overhead: 0.25 µs/bio** (ftrace profiled)
- Mirror/RAID1: per-segment `mirror_enabled` + `mirror_disk` config
- Adaptive striping: EMA-based load balancing with `ema_weight_shift` (default α=0.8%)
- Staleness detection: 1s timer, grace period fallback to static weights
- Wear leveling: per-disk `total_write_bytes[]` with `wear_bias` parameter
- Integrity (CRC32C), atomic (O_DIRECT), crypto (AES-256-XTS) DM target passthrough

Key constants:
- `TV_CHUNK_SIZE` = 1 MB (weight unit)
- `TV_MAX_DISKS` = 16
- `TV_MAX_SEGS` = 16

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

- **Static weights only by default** — Adaptive striping available via `set_policy` message, but requires timer support.
- **No parity-based redundancy** — Mirror/RAID1 supported, but no RAID5/6.
- **No POSIX write() interception** — Applications use standard `write()`/`read()` on the dm target device.
- **No crash consistency** — No journaling or metadata recovery.
- **System disk cannot be used** — dm returns EBUSY on mounted root partition.
- **Module instability risk** — A kernel module bug can oops the system.
- **NVMe write cache must stay ON** — Disabling it causes -21% throughput loss.
- **Platform bottleneck**: B85 PCIe 2.0 x4 adapter limits NVMe to ~1600 MB/s (drive supports PCIe 4.0). Upgrade to PCIe 3.0/4.0 platform for higher throughput.

## License

MIT
