# TieredVol → TieredVol-DRIVER Migration Plan

> **Status: COMPLETED** — The migration from userspace io_uring to kernel dm-target
> is finished. The kernel module `tieredvol.ko` v4.2.0 is production-ready.
> See `BENCHMARK.md` for performance results.

## Goal

Replace the userspace io_uring I/O path with a kernel dm-target module, reducing overhead from ~20-30% to <10%.

## Architecture Comparison

```
TieredVol (userspace):
  App → tv_write() → memcpy to stripe buffer → io_uring submit
  → kernel syscall → dm-linear → disk
  Overhead: syscall + copy + CQE sync + dm-linear dispatch

TieredVol-DRIVER (kernel):
  App → write() → VFS → bio → tieredvol_submit() [kernel module]
  → weighted dispatch per-bio → disk
  Overhead: bio submit only (no copy, no syscall, no CQE)
```

## What to KEEP from TieredVol

These files contain pure logic with no I/O dependencies:

| File | Purpose | Action |
|------|---------|--------|
| `src/tieredvol_types.h` | Type definitions (TV_DISK, TV_SEGMENT, etc.) | **Keep**, remove io_uring-related structs |
| `src/tieredvol_partition.c` | Weight calculation + segment building | **Keep as-is** |
| `src/tieredvol_umapper.c` | Logical→physical offset mapping | **Keep as-is** |
| `src/tieredvol_umeta.c` | Metadata save/load (INI format) | **Keep**, adapt for kernel config |
| `src/tieredvol_common.h` | Input validation | **Keep as-is** |
| `src/version.h` | Version string | **Keep**, update to 2.0.0 |
| `src/tieredvol_exec.c/h` | fork/exec for dmsetup | **Keep as-is** |
| `src/tieredvol_discover.c/h` | Disk discovery (lsblk, sysfs) | **Keep as-is** |
| `src/tieredvol_bench.c/h` | Disk speed benchmarking | **Keep as-is** |

## What to REMOVE

These are the userspace I/O path — replaced by kernel module:

| File | Reason |
|------|--------|
| `src/tiered_sched.c` | Userspace scheduler (io_uring dispatch) → replaced by kernel bio handler |
| `src/tiered_sched.h` | Scheduler API → replaced by kernel module API |
| `src/tiered_io_uring.c` | io_uring wrappers → not needed in kernel |
| `src/tiered_io_uring.h` | io_uring API → not needed |
| `src/tieredvol_benchmark.c` | Init benchmark (O_DIRECT write) → replaced by kernel-side or kept in userspace tool |
| `src/tieredvol_warmup.c/h` | SLC cache warmup → keep in userspace tool |
| `src/io_bench.c/h` | Benchmark runners → keep in userspace tool |
| `src/tiered_io.c` | CLI I/O tool → rewrite to use dm target device directly |

## What to ADD (New Files)

### Kernel Module

| File | Purpose |
|------|---------|
| `driver/tieredvol_core.c` | dm_target setup, bio submission, completion handler |
| `driver/tieredvol_map.c` | bio→disk mapping using tiered_mapper logic |
| `driver/tieredvol_meta.c` | Read metadata from userspace config file |
| `driver/tieredvol.h` | Shared types between kernel and userspace |
| `driver/Makefile` | Kernel module build (kbuild) |

### Userspace Tools (Modified)

| File | Purpose |
|------|---------|
| `src/tiered_setup.c` | CLI tool: create/remove/list (adapted from main.c) |
| `src/tiered_bench.c` | Benchmark tool: direct block device read/write |

## Kernel Module Design

### Core Data Structures (kernel side)

```c
struct tieredvol_ctx {
    struct dm_target *ti;           /* dm target handle */
    struct tiered_metadata meta;    /* segment/disk config */
    struct block_device **bdevs;    /* underlying block devices */
    int ndisks;
    spinlock_t map_lock;            /* for segment lookup */
};
```

### bio Flow

```
tieredvol_map(bio)
  → tv_map_logical(bio->bi_iter.bi_sector * 512, &ctx->meta)
  → map = (disk_index, physical_offset, length)
  → bio_set_dev(bio, ctx->bdevs[map.disk])
  → bio->bi_iter.bi_sector = map.offset / 512
  → submit_bio(bio)
```

### Key Difference from TieredVol

| Aspect | TieredVol | TieredVol-DRIVER |
|--------|-----------|------------------|
| Buffer | Userspace stripe buffer (64×) | None — bio goes directly to disk |
| Scheduling | Batch submit all disks per stripe | Per-bio submit (no batching needed) |
| Sync | CQE wait per stripe | None — bio completion is synchronous |
| Copy | memcpy to stripe buffer | Zero-copy (bio pages passed through) |
| Alignment | Must match O_DIRECT | Kernel handles alignment |

## Build System Changes

### Makefile

```makefile
# Remove: tiered_sched.o, tiered_io_uring.o from SCHED_OBJS
# Remove: -luring from linker flags
# Add: kernel module build target

SCHED_OBJS=src/tiered_partition.o src/tiered_mapper.o \
           src/tiered_metadata.o src/tiered_benchmark.o src/warmup.o

# Kernel module
module:
    make -C /lib/modules/$(shell uname -r)/build M=$(PWD)/driver modules

module_install:
    make -C /lib/modules/$(shell uname -r)/build M=$(PWD)/driver modules_install
    depmod -a
```

### Kernel Module Kbuild

```makefile
obj-m := tieredvol.o
tieredvol-objs := tieredvol_core.o tieredvol_map.o tieredvol_meta.o

ccflags-y := -I$(src)/../src
```

## Migration Steps

### Phase 1: Cleanup (Day 1)
1. Remove `src/tiered_sched.c`, `src/tiered_sched.h`
2. Remove `src/tiered_io_uring.c`, `src/tiered_io_uring.h`
3. Remove `-luring` from Makefile
4. Update `SCHED_OBJS` to exclude io_uring files
5. Verify `tiered_setup` still compiles (weight calc, segment building, metadata)

### Phase 2: Kernel Module Skeleton (Day 2-3)
1. Create `driver/` directory
2. Implement `tieredvol_core.c`: dm_target register/unregister
3. Implement `tieredvol_map.c`: bio mapping using `tv_map_logical()`
4. Implement `tieredvol_meta.c`: read config from `/etc/tieredvol/`
5. Write Kbuild and build

### Phase 3: Userspace Tool (Day 4-5)
1. Rewrite `tiered_setup` CLI to use `dmsetup` to create tieredvol target
2. Command flow:
   ```
   tiered_setup create --disks /dev/nvme0n1,/dev/sdb --name myvol
     → benchmark disks → compute weights → build segments
     → save metadata to /etc/tieredvol/myvol.conf
     → dmsetup create myvol --table "0 <sectors> tieredvol <conf_path>"
   ```
3. Command flow for removal:
   ```
   tiered_setup remove --name myvol
     → dmsetup remove myvol
   ```

### Phase 4: Benchmark & Validate (Day 6-7)
1. Benchmark on same hardware (i5-4570, NVMe + 2× SATA)
2. Compare with TieredVol results
3. Target: η > 90% (overhead < 10%)

## Expected Performance

| Metric | TieredVol | TieredVol-DRIVER (est.) |
|--------|-----------|------------------------|
| 2-disk write η | 82.9% | ~95%+ |
| 3-disk write η | 67.4% | ~92%+ |
| CQE loss | 2-40/bench | 0 |
| CPU usage | High (copy + CQE) | Low (bio only) |
| Memory | 64 MB (buf × 16) | 0 (zero-copy) |

## Risks

1. **Kernel module instability** — a bug can crash the system (oops)
2. **dm-linear interaction** — need to verify tieredvol + dm-linear coexistence
3. **Alignment requirements** — kernel bio has its own alignment constraints
4. **Metadata persistence** — kernel module reads config at creation time, no hot-reload

## Testing Strategy

1. Unit tests: reuse `test_mapper.c`, `test_partition.c` (no I/O dependency)
2. Integration test: create tieredvol target, run `dd` through it, verify data integrity
3. Benchmark: same script as TieredVol (`run_benchmarks_v3.sh`), compare results
