# Partition Splitting — 切塊演算法

本文檔說明 TieredVol Scheduler 的核心切塊演算法：根據每顆碟的速度與容量，計算每個 cycle 每顆碟拿幾塊（chunk）。

> **原型限制**：Weight 在初始化時透過 benchmark 產生靜態值（含 SLC cache 預熱），不可在執行時變更。更改 weight 會使所有 logical-to-physical mapping 失效，除非執行資料遷移。

---

## 資料結構

所有 struct 定義在 `src/tieredvol_types.h`。以下是說明：

### TV_DISK

碟資訊：id, fd, 容量, 可用容量, 速度, 權重, 物理 offset, 碟名。

初始化流程：
```
掃描所有磁碟
    ↓
取得容量
    ↓
取得可用容量
    ↓
Benchmark 一次
    ↓
存 speed
```

---

## 問題

標準 LVM striping / md RAID0 使用統一 stripe size，每顆碟每 cycle 拿**相同數量的 chunk**。快碟被迫等慢碟：

```
NVMe 2000 MB/s: [256KB] → 0.128ms → 等待中...
SATA 500 MB/s:  [256KB] → 0.512ms → 進行中...
NVMe 每 cycle 空等 75% 的時間。
```

需要一種方式，讓快碟拿更多 chunk，慢碟拿較少 chunk，使大家同時完成。

---

## 核心概念：加權 Chunk

```
NVMe  2000 MB/s → 4 chunks per cycle (4 × 256KB = 1024KB)
SATA   500 MB/s → 1 chunk per cycle  (1 × 256KB = 256KB)

時間：
  NVMe: 1024KB / 2000 MB/s = 0.512ms
  SATA:  256KB / 500 MB/s  = 0.512ms
  → 兩者同時完成
```

**快碟拿多、慢碟拿少，整體吞吐量接近各碟速度總和。**

---

## 演算法：Step by Step

### Step 1：Benchmark 取得速度

```
Disk A: NVMe   3000 MB/s
Disk B: NVMe   1500 MB/s
Disk C: SATA   1000 MB/s
Disk D: SATA    500 MB/s
```

### Step 2：計算 Weight（除以最慢碟）

```
slowest = 500 MB/s

A: 3000 / 500 = 6
B: 1500 / 500 = 3
C: 1000 / 500 = 2
D:  500 / 500 = 1

weight = [6, 3, 2, 1]
```

每輪共 `6+3+2+1 = 12` chunks。如果 chunk=256KB，一輪 3072KB。

### Step 3：Dispatch

```
A A A A A A | B B B | C C | D
1536KB       | 768KB | 512KB | 256KB
```

大家幾乎同時完成。

---

## 奇怪的速度怎麼辦？

例如兩顆碟：

```
NVMe:  1234 MB/s
SATA:   480 MB/s

比例 = 1234 / 480 = 2.5708...
```

不可能存 2.5708333... 一定需要近似整數。

### 方法：四捨五入 + 上限

目前實作（`tv_compute_weight()` in `tieredvol_partition.c`）採用簡單的四捨五入：

```c
double w = speed / slowest;
uint32_t result = (uint32_t)(w + 0.5);   // 四捨五入
if (result > 16) result = 16;            // 上限 16
if (result < 1)  result = 1;
```

以 1234:480 為例：

| 項目 | 值 |
|------|------|
| 比例 | 1234 / 480 = 2.5708 |
| 四捨五入 | 3 |
| 實際 weight | 3:1（誤差 16%） |

如果需要更精確的整數近似（如 5:2），需要額外實作搜尋演算法（brute-force search with max_weight constraint），目前尚未實作。

### 三顆碟的情況

```
3178, 947, 653 MB/s

除以最慢：
  4.86, 1.45, 1.0

四捨五入 → [5, 1, 1]
```

---

## 容量不同怎麼辦？分段處理

### 問題

如果四顆碟容量不同：

| Disk | 容量 | 速度 |
|------|------|------|
| A | 1TB | 3000 MB/s |
| B | 1TB | 1500 MB/s |
| C | 512GB | 1000 MB/s |
| D | 2TB | 500 MB/s |

C 只有 512GB，A 和 B 有 1TB。當 C 用完後，剩下三顆碟的比例就不同了。

### 解法：依容量排序，分段

```
512GB → 1TB → 2TB
  │       │      │
段 1     段 2    段 3
```

#### 段 1（0 ~ 512GB）：四顆都有空間

```
速度：3000, 1500, 1000, 500
比例：6:3:2:1
一輪：12 chunks × 256KB = 3072KB
```

#### 段 2（512GB ~ 1TB）：C 用完了

```
剩：A(3000), B(1500), D(500)
比例：6:3:1
一輪：10 chunks × 256KB = 2560KB
```

#### 段 3（1TB ~ 2TB）：A、B 都用完了

```
只剩 D → 直接線性寫入
```

### TV_SEGMENT

分段資訊：邏輯 offset 範圍, 碟數量, 碟 index 陣列, 權重陣列, stripe size。

定義見 `src/tieredvol_types.h`。

#### 範例

四顆碟容量：Disk0 4TB, Disk1 2TB, Disk2 2TB, Disk3 1TB

```
Segment0: 0~1TB     → disk [0,1,2,3]  weight [7,4,2,1]
Segment1: 1~2TB     → disk [0,1,2]    weight [7,4,2]
Segment2: 2~4TB     → disk [0]        weight [7]
```

建立完成後，Runtime 不再修改。

### 實作流程

```
1. 取得每顆碟的容量和速度
2. 按容量排序
3. 計算每個分段的起始/結束 offset
4. 每個分段內，重新計算 weight
5. dispatch 時判斷 offset 落在哪个分段，使用該分段的 weight
```

---

## 動態比例（進階）

不一定要用固定 weight。每次 dispatch 時，動態計算每顆碟「理論上應該拿多少資料」。

### 範例：2400KB 需要寫入

```
速度：NVMe 1234 MB/s, SATA 480 MB/s
總速度：1714 MB/s

NVMe 應拿：2400 × 1234/1714 = 1728KB
SATA 應拿：2400 × 480/1714  = 672KB

對齊到 256KB chunk：
  NVMe → 1792KB (7 chunks)
  SATA → 768KB (3 chunks)
```

### 優點

- 不需要事先算固定 weight
- 每一輪都可以依實際速度重新分配
- 如果某顆碟速度下降（溫度、SLC Cache 用盡），下一輪自動調整

### 缺點

- 每輪都要算一次，稍微多一點 CPU
- 不同輪的分配比例可能不同，增加了複雜度

### 建議

**先實作固定 weight（Step 2 ~ Step 3）**，簡單可靠。動態比例留作未來優化。

---

## Metadata

Metadata 結構定義在 `src/tieredvol_types.h`（TV_METADATA）。

儲存格式：
```
# /etc/tieredvol/fastpool.scheduler
[weighted_striping]
version=1
chunk_size=1048576
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
# ... 其他 segments
```

---

## Runtime Mapping API

```c
// 定義在 src/tieredvol_types.h

TV_MAP tv_map_logical(uint64_t logical, TV_METADATA *meta);
```

輸入：logical offset
輸出：TV_MAP（disk index, physical offset, length）

Mapping 流程：
```
輸入: logical offset
    ↓
先找 Segment（linear scan，≤16 個 segment）
    ↓
stripe_no = (logical - segment_begin) / stripe_size
offset_in = (logical - segment_begin) % stripe_size
    ↓
binary search boundary → 找到 disk id
    ↓
physical_offset = stripe_no × (weight × chunk) + offset_in_disk
    ↓
輸出: fd, physical offset, length
```

---

## 整合到 TieredVol

```
sudo tiered_setup --create --name fastpool --disks nvme0n1:500,sda:500,sdb:500 --scheduler

程式內部：
  1. benchmark → 取得速度
  2. 計算 weight（speed / slowest）
  3. 依容量分段（build segments）
  4. 儲存 metadata 到 /etc/tieredvol/*.scheduler
  5. dispatch 時查 weight → 每顆碟拿幾 chunks
```

---

## 總結

| 問題 | 解法 |
|------|------|
| 快碟等慢碟 | 加權 chunk：快碟拿多、慢碟拿少 |
| 奇怪的速度比例 | 四捨五入 + 上限 16（tv_compute_weight） |
| 碟容量不同 | 依容量排序，分段處理（TV_SEGMENT） |
| 動態速度變化 | 每輪動態計算比例（第二版 DRR） |
| 資料結構 | TV_DISK（碟資訊）、TV_SEGMENT（分段）、TV_METADATA（持久化） |
| Offset 映射 | prefix sum + linear scan → TV_MAP |

---

## 參考

- Weighted striping 概念：參考儲存領域已有的 weighted allocation 概念
- Ratio 近似演算法：四捨五入 + 上限（tv_compute_weight in tieredvol_partition.c）
- 相關文件：[WEIGHTED_IO_SCHEDULER.md](WEIGHTED_IO_SCHEDULER.md)（I/O dispatch 實作）
