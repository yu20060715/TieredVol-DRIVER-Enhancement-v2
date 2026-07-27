# Weighted I/O Scheduler — I/O Dispatch 實作

> **⚠ Architecture Note (v5.0):** The original userspace io_uring scheduler described
> below has been **replaced by a kernel dm-target module** (`driver/tieredvol.ko`).
> Applications now use standard POSIX `write()`/`read()` on `/dev/mapper/<name>`.
> The kernel module handles bio dispatch, weighted striping, mirror/RAID1, adaptive
> load balancing, and staleness detection — all transparently. See `driver/` for the
> current implementation, and `README.md` for the updated architecture overview.
>
> This document is retained as a historical reference for the algorithmic design.

本文檔說明 TieredVol Scheduler 如何將加權切塊結果實際送出 I/O，包括 io_uring dispatch、stripe buffer、offset 映射、metadata 管理。

前置閱讀：[PARTITION_SPLITTING.md](PARTITION_SPLITTING.md)（切塊演算法）

> **原型限制**：
> - 應用程式必須透過 `tv_write()` / `tv_read()` 與 scheduler 互動，標準 POSIX `write()` 不經過 scheduler。
> - 僅支援靜態 weight（初始化時計算，不可變更）。
> - 無容錯機制（任何磁碟故障即導致整組 stripe set 損毀）。
> - Benchmark 僅用於初始化，含 SLC cache 預熱（2GB pre-write）以確保 weight 反映持久速度。

---

## 架構總覽

```
Application
     ↓
TieredVol Scheduler
  ┌─────────────────────────────┐
  │  1. 查 weight table         │
  │  2. 分配 chunks 到各碟      │
  │  3. buffer partial stripe    │
  │  4. io_uring submit         │
  │  5. 等 CQE 完成             │
  └─────────────────────────────┘
     ↓
NVMe ← 1792KB
SATA1 ← 1024KB
SATA2 ← 256KB
```

TieredVol 不是 RAID，而是一個 **I/O Scheduler**。它自己控制資料怎麼分配到各碟，不依賴 LVM striping。

---

## 三層架構

```
┌─────────────────────────────────────────┐
│  第一層：Offset Map                      │
│  輸入 logical offset                     │
│  輸出 disk index + physical offset       │
│  只做數學，不碰 I/O                      │
├─────────────────────────────────────────┤
│  第二層：Stripe Buffer                   │
│  收使用者寫入                            │
│  累積到 stripe_size                      │
│  滿了就 flush                            │
│  只管資料暫存                            │
├─────────────────────────────────────────┤
│  第三層：Dispatcher                      │
│  把 buffer 切給各碟                      │
│  建 io_uring SQE                        │
│  submit + wait                           │
│  這一層才真正碰磁碟                      │
└─────────────────────────────────────────┘
```

---

## Weight Table

從 PARTITION_SPLITTING.md 的演算法得到 weight 後，建立查表用的 prefix sum table：

```
chunk_size = 256KB
weight = [6, 3, 2, 1]   (A, B, C, D)

disk_boundary[0] = 0              (A start)
disk_boundary[1] = 6 × 256 = 1536  (B start)
disk_boundary[2] = 9 × 256 = 2304  (C start)
disk_boundary[3] = 11 × 256 = 2816 (D start)
disk_boundary[4] = 12 × 256 = 3072 (stripe end)

stripe_size = 3072KB
```

任何 offset 落在哪個範圍，就知道對應哪顆碟。

---

## Offset 映射

### 寫入：Logical → Physical

```c
// input: logical_offset (bytes), chunk_size, weight, disk_list
// output: disk_index, disk_offset

stripe_no    = (logical_offset - segment_begin) / stripe_size;
offset_in    = (logical_offset - segment_begin) % stripe_size;

// linear scan disk_boundary（≤16 個 disk）
disk_index   = find_disk(offset_in);  // prefix sum + linear scan
disk_offset  = stripe_no * weight[disk_index] * chunk_size
               + (offset_in - disk_boundary[disk_index]);
```

### 具體範例

```
logical_offset = 4MB = 4096KB
stripe_size = 3072KB

stripe_no = 4096 / 3072 = 1
offset_in = 4096 % 3072 = 1024

disk_boundary = [0, 1536, 2304, 2816, 3072]
1024 落在 [0, 1536) → disk A (index 0)

disk_offset = 1 × 6 × 256KB + 1024 = 1536KB + 1024KB = 2560KB
```

### 讀取：Physical → Logical

```c
// 已知 disk_index 和 disk_offset，反推 logical_offset
// 相同邏輯，反向計算即可
```

---

## Stripe Buffer（Partial Stripe 處理）

應用程式不一定寫 stripe_size 的整數倍。例如 stripe_size = 3072KB，但應用寫 100KB。

### 方案 A：Buffer（推薦）

TieredVol 內部維護 pipeline pool（`TV_STRIPE_BUF` ring）：

```
寫入 100KB → buffer: [100KB / stripe_size]
寫入 200KB → buffer: [300KB / stripe_size]
寫入 468KB → buffer 滿 → flush → dispatch
```

使用 `TV_STRIPE_BUF` struct（定義在 `src/tiered_sched.h`）。實作見 `src/tiered_sched.c`。

### flush_stripe：真正 dispatch

根據 weight table，把 buffer 切成各碟的 chunk，建 io_uring SQE，一次 submit。

完整實作見 `src/tiered_sched.c` 的 `tv_flush()`。

---

## I/O Dispatch（io_uring）

### 為什麼用 io_uring？

- 同時送出多個 I/O request，不等第一個完成
- 零拷貝（zero-copy）支援
- 效能比 AIO / `pread/pwrite` 好很多
- Linux 5.1+ 支援

### Dispatch 流程

```
1. flush_stripe 被呼叫
2. 依 weight table 把 buffer 切成各碟的 chunk
3. 對每顆碟準備一個 SQE（Submission Queue Entry）
4. io_uring_submit() — 一次送出所有 I/O
5. io_uring_wait_cqe() — 等全部完成
6. 處理 CQE（檢查錯誤）
7. 重置 buffer，準備下一輪
```

### 錯誤處理

```c
for (int i = 0; i < (int)seg->disk_count; i++) {
    struct io_uring_cqe *cqe;
    io_uring_wait_cqe(&sched->ring, &cqe);
    int res = cqe->res;
    io_uring_cqe_seen(&sched->ring, cqe);

    if (res < 0) {
        // I/O 失敗
        // 方案 1: retry 3 次
        // 方案 2: 標記碟為 bad，重新計算 weight
        // 方案 3: 回報錯誤給應用程式
    }
}
```

---

## Metadata

只需要保存：

```
chunk_size:     1MB
weight:         [6, 3, 2, 1]
disk_list:      [nvme0n1, sda, sdb, sdc]
stripe_size:    3072KB  (= sum(weight) × chunk_size)
```

不需要記錄每個 block 的位置。所有映射都可以從 weight + offset 計算得出。

### 儲存格式

```
# /etc/tieredvol/fastpool.scheduler
[weighted_striping]
version=1
chunk_size=262144
segment_count=3
disk_count=4
disk0_name=nvme0n1
disk1_name=sda
disk2_name=sdb
disk3_name=sdc
seg0_begin=0
seg0_end=1073741824
seg0_count=4
seg0_disks=0,1,2,3
seg0_weight=7,4,2,1
seg0_stripe=3670016
seg1_begin=1073741824
seg1_end=2147483648
seg1_count=3
seg1_disks=0,1,2
seg1_weight=7,4,2
seg1_stripe=3407872
seg2_begin=2147483648
seg2_end=3221225472
seg2_count=1
seg2_disks=0
seg2_weight=7
seg2_stripe=1835008
```

### Restore

重開機後，需手動重新建立 scheduler volume。`tieredvol-restore.sh` 會嘗試從 `.scheduler` 檔案還原 dm-linear targets，但由於缺少 carve size 資訊，可能需要重新建立。建議直接使用 `tiered_setup --create --scheduler` 重新建立。

---

## 資料結構

所有 struct 定義在 `src/tiered_sched.h`。

### TV_SCHED

Scheduler 核心：io_uring ring, metadata 指標, 碟陣列, stripe buffer。

### TV_STRIPE_BUF

Stripe buffer：data 指標, 使用量, 邏輯起始 offset, in_flight 計數, CQEs pending。

buffer 固定大小 = stripe_size（例如 3072KB）。Scheduler 內建 pipeline pool（64 個 buffer 交替使用）。

---

## Application Write 流程

使用者呼叫：
```
write(fd, data, 100KB)
```

Scheduler：
```
copy → buffer
buffer: 100KB
不送 I/O。
```

第二次：
```
write(fd, data, 300KB)
buffer: 400KB
```

第三次：
```
write(fd, data, 2772KB)
buffer: 3072KB → 滿了 → 開始 Flush
```

---

## Flush 流程

### Step 1：Split Stripe

依 weight 切分 buffer：
```
weight = [7, 4, 2, 1]
chunk = 256KB

disk0: 7 × 256KB = 1792KB
disk1: 4 × 256KB = 1024KB
disk2: 2 × 256KB = 512KB
disk3: 1 × 256KB = 256KB
```

pointer 切分：
```
buffer 0~1792KB    → disk0
buffer 1792~2816KB → disk1
buffer 2816~3328KB → disk2
buffer 3328~3584KB → disk3
```

### Step 2：Build SQE

每顆碟建立一個 SQE：
```c
struct io_uring_sqe *sqe;

sqe0: disk0, 1792KB, offset = map_logical_offset(...)
sqe1: disk1, 1024KB, offset = map_logical_offset(...)
sqe2: disk2, 512KB, offset = map_logical_offset(...)
sqe3: disk3, 256KB, offset = map_logical_offset(...)
```

### Step 3：Submit

全部建立完成，一次送出：
```c
io_uring_submit(&ring);
```

不要一顆一顆 submit。

### Step 4：Completion

等待全部完成：
```c
while (done < disk_count) {
    io_uring_wait_cqe(&ring, &cqe);
    if (cqe->res < 0) {
        // 失敗處理
    }
    io_uring_cqe_seen(&ring, cqe);
    done++;
}

buffer.used = 0;  // 下一個 stripe
```

---

## 讀取流程

與寫入相反：
```
1. 計算 logical_offset → disk_index + disk_offset（map_logical_offset）
2. io_uring submit: read(disk, buf, chunk_size, disk_offset)
3. 等全部完成
4. 依 offset 組回 buffer
```

---

## 實作踩坑

### 坑 1：不要每個小寫入都 flush

不然會變超多小 I/O，效率很差。要等到：
- buffer 滿了
- 或者使用者要求 flush
- 或者關閉檔案

才真的送出。

### 坑 2：不要讓每顆碟各自亂跑 thread

這樣會出現：
- 競態
- 順序亂掉
- metadata 不一致
- debug 很痛苦

比較好的方式是：
- 一個 scheduler thread
- 一個 io_uring ring
- 多個 async request

### 坑 3：不要把 weight 做得太大

例如 3000:500 直接變 3000:500，會很難管理。要先 normalize：
- 除以最慢
- 限制最大 weight（≤16）
- 近似成小整數（6:1, 5:2, 4:1）

### 坑 4：不要把動態測速做成前台操作

如果要重新 benchmark，最好：
- 背景執行
- 不阻塞寫入
- 不頻繁做

否則整個 volume 會很卡。

### 坑 5：io_uring + O_DIRECT + dm-linear 會丟 CQE

在 io_uring 搭配 O_DIRECT 且底層為 dm-linear 時，部分 CQE 可能丢失（res=0 但實際未完成寫入）。可能原因：
- dm-linear 的 boundary 對齊問題
- O_DIRECT 要求 alignment（512B / 4KB），未對齊的寫入會被靜默忽略
- io_uring 的 ring overflow

建議：
- 使用 `io_uring_register_buf_ring()` 做 buffer registration
- 每次 submit 後 double-check CQE count
- 保持 chunk_size 為 1MB（確保 512B alignment）
- 加 30s timeout 防止 hang

---

## DRR（Deficit Round Robin）— 第二版優化

第一版用固定 weight，第二版可升級為 DRR credit 機制。

### 核心概念

每顆碟有一個 `credit`：
- 每輪依速度加 credit（`credit += weight × chunk_size`）
- credit 滿到一個 chunk，就派一個 chunk
- 派出去之後扣掉

### 好處

- 不怕速度是 1234 / 480 這種醜比例
- SSD 速度變動時也好調整
- 不需要先求最簡比
- credit 機制讓分配更平滑

### 每輪怎麼走

```
chunk_size = 256KB
weight = [6, 3, 2, 1]

每輪開始：
  credit[A] += 6 × 256KB = 1536KB
  credit[B] += 3 × 256KB = 768KB
  credit[C] += 2 × 256KB = 512KB
  credit[D] += 1 × 256KB = 256KB

派工：
  A credit 夠 256KB → 派 256KB → 扣 256KB
  B credit 夠 256KB → 派 256KB → 扣 256KB
  ...以此類推
```

### 建議

第一版先做固定 weight，第二版再加 DRR credit。

---

## 效能

### 為什麼 LVM striping 做不到 1000+500+500=1800？

標準 LVM striping / RAID0 使用統一 stripe size，每顆碟每 cycle 拿**相同數量的 chunk**。快碟被迫等慢碟，整體速度 ≈ 最慢碟的速度。

### Weighted Striping 的優勢

```
Disk0: NVMe  3100 MB/s → weight=7 → 7 chunks = 1792KB
Disk1: SATA  1700 MB/s → weight=4 → 4 chunks = 1024KB
Disk2: SATA   800 MB/s → weight=2 → 2 chunks = 512KB
Disk3: SATA   450 MB/s → weight=1 → 1 chunk  = 256KB

一輪：14 chunks × 256KB = 3584KB
大家幾乎同時完成 → 整體吞吐量接近各碟速度總和
```

### 實測方法

```bash
# Scheduler 模式（加權條帶化）
sudo tiered_io --name fastpool --bench --size 128MB

# Direct path 模式（LVM/filesystem 對比）
sudo tiered_io --path /mnt/test --bench --size 128MB
```

兩種模式使用相同的 TV_CHUNK_SIZE（目前 1MB）block size 和 O_DIRECT，確保公平比較。

---

## 實作架構

### 模組拆分

對應到 TieredVol 的目錄結構（v5.0 重構後）：

```
driver/                          # Kernel dm-target module
├── tieredvol.h                  # Shared kernel types + API
├── tieredvol_core.c             # DM lifecycle: ctr/dtr/map/status/ioctl
├── tieredvol_map.c              # Logical ↔ Physical offset mapping (kernel)
├── tieredvol_meta.c             # Metadata loading from config file
├── tieredvol_message.c          # dmsetup message dispatch table (28 handlers)
├── tieredvol_mirror.c           # Mirror/RAID1: bio clone, rebuild thread
├── tieredvol_log.c              # Kfifo ring buffer + EMA decay + per-CPU stats
└── tieredvol_sysfs.c            # /sys/kernel/tieredvol/ attributes

src/                             # Userspace tool (tiered_setup)
├── main.c                       # CLI entry point
├── tieredvol_common.h           # Input validation (name, fs, mount)
├── tieredvol_types.h            # Shared type definitions (TV_DISK, TV_SEGMENT, etc.)
├── tieredvol_umapper.c          # Logical ↔ Physical offset mapping (userspace)
├── tieredvol_partition.c        # Weight + segment calculation
├── tieredvol_umeta.c            # Metadata save/load (INI format)
├── tieredvol_benchmark.c        # Initialization benchmark
├── tieredvol_warmup.c/h         # SLC cache warm-up
├── tieredvol_exec.c/h           # External command execution
├── tieredvol_discover.c/h       # Disk discovery (lsblk, sysfs)
├── tieredvol_bench.c/h          # Setup benchmark logic
├── cmd_create.c/h               # Volume creation (kernel dm + LVM)
├── cmd_remove.c/h               # Volume removal
├── cmd_status.c/h               # Target status queries
└── cmd_scheduler.c/h            # Scheduler (DM) volume creation

common/
└── tieredvol_meta_format.h      # Shared format constants (kernel + userspace)
```

### API

The kernel module exposes a dm-target interface. Users interact via standard POSIX I/O on `/dev/mapper/<name>` and configure via `dmsetup message`:

```c
// Kernel: dm_target ops (driver/tieredvol.h)
int tieredvol_ctr(struct dm_target *ti, unsigned int argc, char **argv);
void tieredvol_dtr(struct dm_target *ti);
int tieredvol_map(struct dm_target *ti, struct bio *bio);
int tieredvol_message(struct dm_target *ti, unsigned int argc,
                      char **argv, char *result, unsigned int maxlen);

// Kernel: offset mapping (driver/tieredvol.h)
struct tieredvol_map tv_map_logical(u64 logical, struct tieredvol_metadata *meta,
                                    u32 chunk_size);
struct tieredvol_map tv_map_logical_adaptive(u64 logical, ...);
```

完整實作見 `driver/` 目錄下各 `.c` 文件。

### 為什麼用 io_uring？

- 同時送出多個 I/O request，不等第一個完成
- 零拷貝（zero-copy）支援
- 效能比 AIO / `pread/pwrite` 好很多
- Linux 5.1+ 支援

---

## 與現有 TieredVol 的整合

### CLI 模式

```bash
# 使用 scheduler 模式建立 volume（--fs 和 --mount 會被忽略）
sudo tiered_setup --create --name fastpool \
    --disks nvme0n1:500,sda:500 \
    --scheduler
```

### 開機還原

tieredvol-restore.sh 讀取 `/etc/tieredvol/*.conf` 檔案重建 LVM striping volumes，並嘗試從 `.scheduler` 檔案還原 scheduler volumes 的 dm-linear targets。

---

## 實作難度

| 項目 | 難度 | 說明 |
|------|------|------|
| Weight table 建立 | 簡單 | prefix sum + linear scan |
| Offset 映射 | 簡單 | 數學計算 |
| Stripe buffer | 中等 | Ring buffer + flush 機制 |
| io_uring dispatch | 中等 | 需要 liburing 或直接 syscall |
| Metadata 儲存 | 簡單 | 文字檔格式 |
| 錯誤處理 | 中等 | I/O 失敗 retry / 碟壞重算 weight |
| 部分讀取優化 | 中等 | 只送需要的碟的 I/O |

---

## 參考

- io_uring: `io_uring_setup`, `io_uring_enter`, liburing
- Linux block layer: `drivers/block/`
- 切塊演算法：[PARTITION_SPLITTING.md](PARTITION_SPLITTING.md)
- Weighted striping 概念：參考儲存領域已有的 weighted allocation 概念（如 ZFS metadata allocation）
