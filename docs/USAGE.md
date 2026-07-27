# TieredVol 使用教學

## 系統需求

- Linux（已測試 Ubuntu 24.04, kernel 6.14）
- `lvm2` `dmsetup` `liburing-dev` `gcc` `make`
- Root 權限（sudo）

### 安裝依賴

```bash
# Debian / Ubuntu
sudo apt install lvm2 liburing-dev gcc make

# Fedora / RHEL
sudo dnf install lvm2 liburing-devel gcc make

# Arch
sudo pacman -S lvm2 liburing gcc make
```

### 編譯

```bash
cd TieredVol
make
```

---

## CLI 模式

所有 CLI 操作需要 root 權限。

### 列出硬碟

```bash
sudo tiered_setup --list
```

輸出範例：

```
DEVICE       TYPE     MODEL                        TRAN         SIZE       SPEED
------------ -------- ---------------------------- ------------ -------- --------
sda          disk     Samsung SSD 870              sata         500.1G     -
sdb          disk     WDC WD10EZEX                sata         931.5G     -
nvme0n1      disk     WD Black SN770              nvme         1.0T       -

[ROOT] = System disk, cannot be carved with dm-linear
[MOUNTED] = Mounted partition, cannot be carved with dm-linear
```

### 測速

```bash
# 依序測試（峰值速度）
sudo tiered_setup --bench --disks sdb,sdc,nvme0n1

# 依序測試（含 SLC cache 預熱，持久速度）
sudo tiered_setup --bench --disks sdb,sdc,nvme0n1 --warmup

# 依序測試
sudo tiered_setup --bench --disks sdb,sdc --sequential
```

預設 parallel 模式，多顆碟同時跑。輸出每顆碟的 Write / Read 速度（MB/s）。
加 `--warmup` 會先寫 2GB 填滿 SLC cache 再測速，得到持久速度。

### 建立 Volume

```bash
sudo tiered_setup --create --name fastpool --disks sdb:300,sdc:200 --fs ext4 --mount /mnt/fast
```

參數說明：
| 參數 | 說明 | 必填 |
|------|------|------|
| `--name` | Volume 名稱 | 是 |
| `--disks` | 硬碟列表（格式：`碟名:GB,碟名:GB`） | 是 |
| `--fs` | 檔案系統（ext4/xfs/btrfs/none） | 否，預設 ext4 |
| `--mount` | 掛載點 | 否 |
| `--stripesize` | stripe 大小（KB） | 否，自動判斷 |

不指定容量時，使用硬碟全部空間（扣 1GB）。

#### 建立流程

程式會依序執行以下步驟：

1. **碟驗證** — 檢查每顆碟的狀態：
   - 系統碟（`[ROOT]`）→ 跳過
   - 已掛載碟（`[MOUNTED]`）→ 跳過
   - **已切過的碟** → 報錯退出，提示先執行 `--remove` 解除（並警告資料會消失）
   - 碟容量不足 → 報錯退出
2. **Configuration 顯示** — 列出每顆碟的名稱、carve 大小、剩餘空間、速度、比例
3. **雙重確認** — 顯示警告訊息，要求輸入 `YES` 才能繼續（其他輸入直接退出）
4. **清理舊 target** — 自動清除上次的 dm-linear、VG、LV
5. **建立 dm-linear** — 從每顆碟的 sector 0 切出指定大小
6. **建立 LVM** — pvcreate → vgcreate → lvcreate striped
7. **格式化** — mkfs.ext4/xfs/btrfs
8. **掛載** — mount 到指定路徑

**任何步驟失敗會自動回滾**，清理所有已完成的裝置。

#### 剩餘空間顯示

Configuration 表格會顯示每顆碟 carve 後的剩餘空間：

```
  DEVICE       CARVE    REMAIN   SPEED      TIER       RATIO
  ------------ -------- -------- ---------- ---------- ----------
  sdb          1000GB   0GB      2000       FAST       66.7%
  sdc          500GB    500GB    1000       SLOW       33.3%
```

注意：每顆碟只能 carve 一次（dm-linear 從 sector 0 開始）。剩餘空間不能再次 carve。

### 查看狀態

```bash
sudo tiered_setup --status
```

顯示 DM targets 和已儲存的 config 檔案。

### 刪除 Volume

```bash
sudo tiered_setup --destroy --name fastpool
# 或
sudo tiered_setup --remove --name fastpool
```

支援刪除 LVM striped volume 和 weighted I/O scheduler volume。程式會自動偵測 `.scheduler` 檔案，如果是 scheduler volume 則只清理 dm-linear targets。

### 建立 Weighted I/O Scheduler Volume

```bash
sudo tiered_setup --create --name fastpool \
    --disks nvme0n1:1000,sda:500 \
    --scheduler
```

加上 `--scheduler` 參數後，不會建立 LVM volume，而是：
1. 建立 dm-linear targets
2. 依磁碟速度計算 weight
3. 建立 segment 資料
4. 儲存 metadata 到 `/etc/tieredvol/fastpool.scheduler`

之後用 `tiered_io` 工具進行 I/O 操作。

---

## I/O 工具 (tiered_io) — DEPRECATED

> **⚠ Removed in v5.0:** The `tiered_io` binary has been removed. The kernel
> dm-target module now handles all I/O dispatch transparently. Use standard
> `write()`/`read()` on `/dev/mapper/<name>` for data operations, and
> `fio` for benchmarking. See `BENCHMARK.md` for benchmark commands.

`tiered_io` was the Weighted I/O Scheduler's I/O entry point. It is no longer part of the build.

### 查看 Volume 資訊

```bash
sudo tiered_io --name fastpool --info
```

輸出 metadata 和 segment 資訊：

```
Metadata: v1, chunk=1MB, 2 disks, 2 segments
  Disk[0] nvme0n1
  Disk[1] sda
  Segment[0]: 0 - 53687091200 (2 disks, stripe=3MB)
    disk[0] weight=2 chunk=2MB
    disk[1] weight=1 chunk=1MB
  Segment[1]: 53687091200 - 107374182400 (1 disks, stripe=1MB)
    disk[1] weight=1 chunk=1MB
```

### 寫入 Benchmark（Scheduler 模式）

```bash
# 峰值速度（SLC cache 還在）
sudo tiered_io --name fastpool --bench --size 128MB

# 持久速度（SLC cache 預熱後）
sudo tiered_io --name fastpool --bench --size 128MB --warmup

# 大 size + O_DIRECT + 持久速度
sudo tiered_io --name fastpool --bench --size 1GB --warmup
```

透過 weighted striping 寫入指定大小的資料，測量吞吐量。
加 `--warmup` 先寫 20%% 的 volume（最多4GB）填滿 SLC cache 再測，得到持久速度。
預設使用 O_DIRECT（繞過 page cache，得到真實磁碟速度）。加 `--no-direct` 可關閉。

輸出範例：
```
Benchmark: 134217728 bytes (128.0 MB) in 0.234 seconds
Throughput: 547.0 MB/s
Stripes flushed: 667
```

### 完整 Benchmark Suite（Scheduler 模式）

```bash
# 執行所有 benchmark 模式
sudo tiered_io --name fastpool --bench-all
```

`--bench-all` 會依序執行 512MB、5120MB、10240MB 的 benchmark，每個 size 各跑兩輪（no warmup + with warmup）。

### 寫入 Benchmark（Direct Path 模式，用於 LVM/filesystem 對比）

```bash
# 峰值速度
sudo tiered_io --path /mnt/test --bench --size 128MB

# 持久速度
sudo tiered_io --path /mnt/test --bench --size 128MB --warmup

# 完整 benchmark suite
sudo tiered_io --path /mnt/test --bench-all
```

Direct path 模式使用 `pwrite()` 直接寫入指定路徑，適用於 LVM volume 或任何檔案系統。
**使用相同的 TV_CHUNK_SIZE（目前 1MB）block size 和 O_DIRECT，確保與 scheduler 模式公平比較。**

### 信號處理

`tiered_io` 支援 SIGTERM 和 SIGINT（Ctrl+C）優雅中斷：

- 收到信號後，等待目前 in-flight 的 I/O 完成
- 清理 io_uring ring 和 stripe buffer
- 正確關閉所有檔案描述元
- 輸出已完成的 stripes 數量和部分吞吐量統計

```bash
# 優雅中斷（Ctrl+C）
sudo tiered_io --name fastpool --bench --size 1GB
^C
# → 等待 in-flight I/O 完成後退出

# 背景執行 + timeout
sudo timeout 30s tiered_io --name fastpool --bench --size 1GB
```

### 寫入資料

```bash
dd if=/dev/zero bs=1M count=10 | sudo tiered_io --name fastpool --write --offset 0 --len 10485760
```

從 stdin 讀取資料，寫入 scheduler volume 的指定 offset。

| 參數 | 說明 | 必填 |
|------|------|------|
| `--offset` | 起始邏輯 offset（bytes） | 否，預設 0 |
| `--len` | 寫入長度（bytes） | 是 |

### 讀取資料

```bash
sudo tiered_io --name fastpool --read --offset 0 --len 1024 | xxd
```

從 scheduler volume 讀取資料，輸出到 stdout。

| 參數 | 說明 | 必填 |
|------|------|------|
| `--offset` | 起始邏輯 offset（bytes） | 否，預設 0 |
| `--len` | 讀取長度（bytes） | 是 |

### 完整測試流程

```bash
# 1. 建立 scheduler volume
sudo tiered_setup --create --name testpool \
    --disks nvme0n1:100,sda:100 \
    --scheduler

# 2. 確認 metadata
sudo tiered_io --name testpool --info

# 3. 寫入 benchmark
sudo tiered_io --name testpool --bench --size 128MB

# 4. 寫入 + 讀回驗證
dd if=/dev/urandom bs=1M count=1 | sudo tiered_io --name testpool --write --offset 0 --len 1048576
sudo tiered_io --name testpool --read --offset 0 --len 1048576 | md5sum

# 5. 清理
sudo tiered_setup --destroy --name testpool
```

---

## 注意事項

- **系統碟無法使用** — 掛載 `/` 的硬碟會標記 `[ROOT]` 並鎖定
- **已掛載硬碟無法使用** — 標記 `[MOUNTED]` 的硬碟會鎖定
- **已切過的碟** — 如果硬碟已經被 carve 過（存在 `tv_*_carve` dm target），程式會報錯並提示先解除
- **carve 部分會被覆蓋** — dm-linear 從 sector 0 開始，carve 部分的資料將永久消失。操作前請備份
- **每顆碟只能 carve 一次** — dm-linear 從 sector 0 開始，第二次 carve 會覆蓋第一次
- 需要 root 權限執行所有操作
- 建立 volume 時任何步驟失敗會**自動回滾**

---

## 重開機保留

TieredVol 的 volume 預設**不會在重開機後自動恢復**。要實現開機自動重建，需要安裝 systemd service。

### 安裝

```bash
cd TieredVol
make
sudo make install
sudo systemctl daemon-reload
sudo systemctl enable tieredvol-restore
```

安裝後，每次建立 volume 時，systemd service 會在開機時自動讀取 `/etc/tieredvol/*.conf` 並重建。

### 手動測試

```bash
# 模擬還原（不真的動手）
sudo tieredvol-restore.sh --dry-run

# 正式還原
sudo tieredvol-restore.sh

# 查看日誌
journalctl -u tieredvol-restore
```

### 管理

```bash
sudo systemctl status tieredvol-restore   # 查看狀態
sudo systemctl disable tieredvol-restore  # 停用開機自動還原
sudo systemctl enable tieredvol-restore   # 重新啟用
```

### 運作原理

```
開機 → systemd 啟動 tieredvol-restore.service
     → 讀取 /etc/tieredvol/*.conf
     → 依序重建：dm-linear → pvcreate → vgcreate → lvcreate → mount
```

每個 volume 的 config 檔包含所有必要參數（碟名、容量、檔案系統、掛載點、stripe 大小），restore script 會逐一讀取並重建。
