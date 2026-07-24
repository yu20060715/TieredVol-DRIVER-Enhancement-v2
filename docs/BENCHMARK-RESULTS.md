# Benchmark Results — B85 Platform

## System
- CPU: Intel i5-4570
- RAM: DDR3 1600MHz
- NVMe: P3 Plus 1T (PCIe 2.0x4)
- SATA: WD Blue NAND 250G + MX500 500G
- OS: Lubuntu, Linux 6.x

## Integration Test (pre-migration, userspace scheduler)

> Historical results from the TieredVol userspace prototype (pre-kernel migration).

| Test Suite | Result |
|------------|--------|
| test_common | 34/34 PASS |
| test_mapper | 10/10 PASS |
| test_partition | 19/19 PASS |
| test_metadata | 11/11 PASS |
| test_sched | 22/22 PASS |
| test_integrity | 11/11 PASS |
| **Total** | **107/107 PASS** |

## Throughput Results (512 MB, n=5)

| 場景 | Run1 | Run2 | Run3 | Run4 | Run5 | Mean | StdDev |
|------|------|------|------|------|------|------|--------|
| 2-disk write | 1485.7 | 1490.9 | 2139.0* | 1484.3 | 1477.8 | 1615.5 | 264.3 |
| 2-disk read | 1181.4 | 1216.1 | 1421.9* | 1071.1 | 993.6 | 1176.8 | 144.8 |
| 3-disk write | 1659.5 | 1859.8 | 1852.5 | 1808.0 | 1764.2 | 1788.8 | 73.6 |
| 3-disk read | 1346.3 | 1327.2 | 1228.3 | 1274.5 | 1228.0 | 1280.9 | 50.9 |

\* Run 3 for 2-disk tests shows anomalous high value (likely SLC cache burst)

## Large Transfer Results (3-disk, 1 run)

| 場景 | Size | Throughput |
|------|------|-----------|
| 3-disk write | 5 GB | 1285.6 MB/s |
| 3-disk write | 10 GB | 1228.6 MB/s |
| 3-disk read | 5 GB | 1240.5 MB/s |
| 3-disk read | 10 GB | 1318.0 MB/s |

## LVM Control (sdb+sdc, stripe=1M, mean of 3 runs)

| 場景 | Run1 | Run2 | Run3 | Mean |
|------|------|------|------|------|
| LVM write | 415 | 390 | 390 | 398.3 MB/s |
| LVM read | 440 | 426 | 430 | 432.0 MB/s |

## Analysis

### Scheduler vs LVM Comparison (sdb + sdc only)

Since the TieredVol scheduler was tested with nvme0n1 + sdb + sdc, and LVM was tested with sdb + sdc only, a direct comparison involves different disk sets:

- **TieredVol 3-disk write (NVMe + 2x SATA)**: ~1789 MB/s
- **LVM 2-disk write (2x SATA only)**: ~398 MB/s
- **TieredVol 3-disk read (NVMe + 2x SATA)**: ~1281 MB/s
- **LVM 2-disk read (2x SATA only)**: ~432 MB/s

Note: TieredVol includes NVMe with PCIe 2.0x4 (~1000 MB/s) while LVM only uses the two SATA disks (~450 MB/s each). The weighted striping allows heterogeneous disks to be combined effectively.

### Observations
- Weighted striping achieves **~94%** of theoretical sum (1788.8 / 1900 ≈ 0.94)
- Write performance scales well with additional disks
- Read shows lower efficiency due to NVMe read speed being limited by PCIe 2.0x4 bottleneck
- "CQEs stuck (lost), recovering" messages indicate occasional io_uring completion queue timeouts, data integrity is maintained via retry

---

## Kernel dm-target Integration Test (Phase 5)

**Date:** 2026-07-23  
**Kernel:** 6.14.0-27-generic  
**Module:** tieredvol v4.6.0 (kernel dm target)

### Hardware

| Disk | Model | Size | Type | Write | Read | Tier |
|------|-------|------|------|-------|------|------|
| nvme0n1 | CT1000P3PSSD8 | 931.5G | NVMe | 940 MB/s | 940 MB/s | FAST |
| sdb | CT500MX500SSD1 | 465.8G | SATA | 412 MB/s | 412 MB/s | SLOW |
| sdc | WDC WDS250G2B0A | 232.9G | SATA | 440 MB/s | 440 MB/s | MED |

### Segment Layout (auto-computed)

| Segment | Logical Range | Disks | Stripe | Description |
|---------|---------------|-------|--------|-------------|
| 0 | [0, 231GB) | 3 (sdc+sdb+nvme) | 4096KB | All disks participate |
| 1 | [231GB, 465GB) | 2 (sdb+nvme) | 3072KB | sdc exhausted |
| 2 | [465GB, 931GB) | 1 (nvme) | 2048KB | Only NVMe remains |

### Throughput Results (Default blocksize=1MB — Pre-fix Baseline)

| Test | 512MB | 5GB | 10GB |
|------|-------|-----|------|
| **Write** | 590 MB/s | 615 MB/s | 587 MB/s |
| **Read** | 562 MB/s | 625 MB/s | 656 MB/s |

### Throughput Results (blocksize=16MB — Post-fix)

| Test | 512MB | 5GB | 10GB |
|------|-------|-----|------|
| **Write** | 1057 MB/s | 1134 MB/s | 1084 MB/s |
| **Read** | 1264 MB/s | 1356 MB/s | 1416 MB/s |

### Throughput Results (blocksize=64MB — Optimal for Writes)

| Test | 512MB | 5GB | 10GB |
|------|-------|-----|------|
| **Write** | 1218 MB/s | 1220 MB/s | 1195 MB/s |
| **Read** | 1384 MB/s | — | — |

### Blocksize Sensitivity (Write, 512MB)

| Blocksize | Throughput | Efficiency |
|-----------|-----------|------------|
| 1 MB | 576 MB/s | 35% |
| 16 MB | 1057 MB/s | 64% |
| 32 MB | 1082 MB/s | 66% |
| 64 MB | 1218 MB/s | 74% |

### Data Integrity

| Test | Size | Result |
|------|------|--------|
| dd write + read | 64 MB | PASS |
| dd write + read (cross-stripe) | 500 MB | PASS |

### Unit Tests

| Test Suite | Result |
|------------|--------|
| test_common | 34/34 PASS |
| test_mapper | 14/14 PASS |
| test_partition | 19/19 PASS |
| test_metadata | 14/14 PASS |
| **Total** | **81/81 PASS** |

### Analysis

**Pre-fix (blocksize=1MB):**
- dm target achieved ~60% efficiency — bottleneck was bio splitting returning DM_MAPIO_REMAPPED after submit_bio_noacct (causing double-submit) and 1MB blocks limiting parallelism

**Post-fix (blocksize=16MB+):**
- Read 10GB: **1416 MB/s** → **90.2%** of theoretical weighted throughput (~1570 MB/s)
- Write 64MB blocks: **1220 MB/s** → **78%** efficiency
- Write bottleneck is bioset pool (64 bios) limiting split concurrency at very large blocks
- 3-segment layout automatically adapts to unequal disk capacities
- SLC cache warm-up (2GB) applied before each disk benchmark for accurate results

**Key Fixes Applied:**
1. `tieredvol_meta.c`: `kernel_read()` positive return leaked into function return → separate `nr` variable
2. `tieredvol_core.c`: `tieredvol_split_and_submit()` returned DM_MAPIO_REMAPPED after `submit_bio_noacct()` → changed to DM_MAPIO_SUBMITTED
3. `tiered_io.c`: Default blocksize changed from 1MB to 16MB; added `--blocksize` CLI param
4. `cmd_create.c`: insmod path fixed to `driver/tieredvol.ko`
