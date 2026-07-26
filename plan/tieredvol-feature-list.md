# TieredVol v4.6.0 已實現功能清單 + 開源專案參考

---

## 1. I/O 派送（6 種策略 + bio 操作）

| # | 功能 | 說明 | 測試？ |
|---|------|------|:------:|
| 1 | **Static weighted dispatch** | 預計算的加權 stripe 邊界，確定性派送 | ✅ |
| 2 | **Adaptive EMA dispatch** | 每個 bio 選最空閒碟（EMA 載入分數 + wear 懲罰），跳過 stale 碟 | ✅ |
| 3 | **Random dispatch** | 隨機選碟（`get_random_u32()`） | ✅ |
| 4 | **Bio sector remapping** | `bio_set_dev()` + 重寫 `bi_iter.bi_sector` | ✅ |
| 5 | **Invalid disk error** | disk 超出範圍時 `bio_io_error()` | ✅ |
| 6 | **Write mirroring** | bio clone + 提交到 mirror 碟 + 自定 `bi_end_io` | ✅ |

**開源參考：**
- `drivers/md/dm-switch.c` — 動態路徑切換，region-based bio 派送
  - https://github.com/torvalds/linux/blob/master/drivers/md/dm-switch.c
- `drivers/md/dm-stripe.c` — 經典 striped target，加權分配
  - https://github.com/torvalds/linux/blob/master/drivers/md/dm-stripe.c
- `drivers/md/dm-crypt.c` — bio clone + 重定向到加密層
  - https://github.com/torvalds/linux/blob/master/drivers/md/dm-crypt.c
- `drivers/md/dm-raid1.c` — bio clone + 雙碟寫入 + 自定 bi_end_io
  - https://github.com/torvalds/linux/blob/master/drivers/md/dm-raid1.c

---

## 2. 負載均衡 + 時間衰減

| # | 功能 | 說明 | 測試？ |
|---|------|------|:------:|
| 7 | **EMA load 計算** | 每秒 timer tick，`ema = ema*(1-alpha) + snapshot*alpha`，alpha=8/1024 | ✅ |
| 8 | **In-flight byte tracking** | 原子計數器，map 時 +1，timer tick 時 `atomic_xchg` 歸零 | ✅ |
| 9 | **1-second decay timer** | `timer_list` 每 HZ 觸發一次 | ✅ |
| 10 | **Wear-bias penalty** | `wear_bias × total_write_bytes / total_writes` 加到 load score | ✅ |

**開源參考：**
- `block/kyber-iosched.c` — 基於 token 的動態深度調整（類似 EMA 概念）
  - https://github.com/torvalds/linux/blob/master/block/kyber-iosched.c
- `block/mq-deadline.c` — 請求排序 + 時間衰減（FIFO 過期機制）
  - https://github.com/torvalds/linux/blob/master/block/mq-deadline.c
- `drivers/md/dm-thin.c` — pool 模式切換 + low watermark callback
  - https://github.com/torvalds/linux/blob/master/drivers/md/dm-thin.c
- `kernel/time/timer_list.c` — `timer_list` API 參考
  - https://github.com/torvalds/linux/blob/master/kernel/time/timer_list.c

---

## 3. Stale 碟偵測 + 恢復

| # | 功能 | 說明 | 測試？ |
|---|------|------|:------:|
| 11 | **Stale 標記** | 超過 `stale_after_ns`（預設 5 秒）無 I/O 的碟標記 stale | ✅ |
| 12 | **Stale 恢復（I/O 觸發）** | stale 碟收到新 I/O 時立即 un-stale + 新 grace period | ✅ |
| 13 | **Stale 恢復（冷卻）** | 2× `stale_after_ns` 後自動恢復 | ✅ |
| 14 | **Grace period** | 新恢復的碟有 grace period 保護 | ✅ |

**開源參考：**
- `drivers/md/dm-dust.c` — 壞軌模擬 + 動態啟用/停用（類似 stale 概念）
  - https://github.com/torvalds/linux/blob/master/drivers/md/dm-dust.c
- `drivers/md/dm-raid1.c` — mirror failover + 錯誤偵測 + 自動切換
  - https://github.com/torvalds/linux/blob/master/drivers/md/dm-raid1.c
- `drivers/md/dm-log.c` — dirty region tracking（類似 stale tracking）
  - https://github.com/torvalds/linux/blob/master/drivers/md/dm-log.c
- `block/disk-events.c` — 磁碟事件輪詢 + 狀態追蹤
  - https://github.com/torvalds/linux/blob/master/block/disk-events.c

---

## 4. Per-disk I/O 統計

| # | 功能 | 說明 | 測試？ |
|---|------|------|:------:|
| 15 | Read bytes counter | `total_read_bytes[disk]` | ✅ |
| 16 | Read ops counter | `total_read_ops[disk]` | ✅ |
| 17 | Write bytes counter | `total_write_bytes[disk]` | ✅ |
| 18 | Write ops counter | `total_write_ops[disk]` | ✅ |
| 19 | Error counter | `error_count[disk]`（atomic_t） | ✅ |

**開源參考：**
- `drivers/md/dm.c` — DM 核心的 bio 統計（`dm_stats_message`）
  - https://github.com/torvalds/linux/blob/master/drivers/md/dm.c
- `block/blk-mq.c` — blk-mq 的 per-tag 統計
  - https://github.com/torvalds/linux/blob/master/block/blk-mq.c
- `drivers/nvme/host/core.c` — NVMe 的 per-controller 統計
  - https://github.com/torvalds/linux/blob/master/drivers/nvme/host/core.c
- `block/genhd.c` — `part_stat_show` per-partition 統計展示
  - https://github.com/torvalds/linux/blob/master/block/genhd.c

---

## 5. Per-CPU 全域統計

| # | 功能 | 說明 | 測試？ |
|---|------|------|:------:|
| 20 | Per-CPU map count | `this_cpu_inc` | ✅ |
| 21 | Per-CPU sector count | `this_cpu_add` | ✅ |
| 22 | Per-CPU byte count | `this_cpu_add` | ✅ |
| 23 | Cross-CPU aggregation | `tv_read_count/sectors/bytes()` 迭代所有 CPU | ✅ |

**開源參考：**
- `include/linux/percpu_counter.h` — `percpu_counter` API（批量聚合）
  - https://github.com/torvalds/linux/blob/master/include/linux/percpu_counter.h
- `kernel/sched/fair.c` — CFS 的 per-CPU 載入追蹤
  - https://github.com/torvalds/linux/blob/master/kernel/sched/fair.c
- `drivers/md/dm-thin.c` — pool 的 per-CPU deferred set
  - https://github.com/torvalds/linux/blob/master/drivers/md/dm-thin.c
- `include/linux/mm_types.h` — `vm_stat` per-CPU 計數器
  - https://github.com/torvalds/linux/blob/master/include/linux/mm_types.h

---

## 6. DM Message 指令（15+ 個）

| # | 指令 | 說明 | 測試？ |
|---|------|------|:------:|
| 24 | `reset_stats` | 歸零 per-CPU 統計 | ✅ |
| 25 | `show_stats` | 回傳 maps count, avg bytes, total bytes | ✅ |
| 26 | `status` | 回傳各碟名稱 + weight | ✅ |
| 27 | `show_inflight` | 回傳各碟 in-flight bytes | ✅ |
| 28 | `adaptive_on` | 切換到 adaptive policy | ✅ |
| 29 | `adaptive_off` | 切換到 static policy | ✅ |
| 30 | `set_policy <name>` | 設定 static/adaptive/random | ✅ |
| 31 | `set_ema_shift` | 設定 EMA shift（⚠️ **已修 bug**：argc 檢查） | ✅ |
| 32 | `set_stale_ms <ms>` | 設定 stale 偵測超時 | ✅ |
| 33 | `show_adaptive` | 回傳 policy + EMA + stale + wear 資訊 | ✅ |
| 34 | `show_wear` | 回傳 wear_bias + 各碟 write bytes | ✅ |
| 35 | `show_io_stats` | 回傳各碟 read/write ops/bytes | ✅ |
| 36 | `reset_io_stats` | 歸零各碟 I/O 統計 | ✅ |
| 37 | `set_wear_bias <bias>` | 設定 wear 懲罰因子 | ✅ |
| 38 | `reset_wear` | 歸零各碟 write bytes | ✅ |
| 39 | `show_mirror` | 回傳 mirror 統計 | ✅ |
| 40 | `set_mirror <seg> <disk>` | 設定 mirror 目標 | ✅ |

**開源參考：**
- `drivers/md/dm-dust.c` — 20+ 個 message 指令（addbadblock/removebadblock/enable/disable/queryblock）
  - https://github.com/torvalds/linux/blob/master/drivers/md/dm-dust.c
- `drivers/md/dm-switch.c` — `process_set_region_mappings` 動態 region 重新映射
  - https://github.com/torvalds/linux/blob/master/drivers/md/dm-switch.c
- `drivers/md/dm-raid1.c` — mirror 指令（add/remove/flush/stats）
  - https://github.com/torvalds/linux/blob/master/drivers/md/dm-raid1.c
- `drivers/md/dm-thin.c` — thin pool 指令（create_thin/delete_thin/set_transaction_id）
  - https://github.com/torvalds/linux/blob/master/drivers/md/dm-thin.c

---

## 7. DM Target 生命週期

| # | 功能 | 說明 |
|---|------|------|
| 41 | Constructor (ctr) | 分配 context、讀取 metadata、開啟 DM 裝置 | ✅ |
| 42 | Destructor (dtr) | 刪除 timer、flush work、釋放記憶體 | ✅ |
| 43 | IO hints | 回報 block size、chunk size、io_opt | ✅ |
| 44 | Iterate devices | 回傳所有底層裝置 | ✅ |
| 45 | Prepare ioctl | 回傳第一個裝置的 bdev | ✅ |
| 46 | Flush/Discard propagation | `num_flush_bios = ndisks` | ✅ |
| 47 | Module init/exit | 註冊 DM target + workqueue | ✅ |

**開源參考：**
- `drivers/md/dm-crypt.c` — 完整的 ctr/dtr/map 生命週期 + mempool 管理
  - https://github.com/torvalds/linux/blob/master/drivers/md/dm-crypt.c
- `drivers/md/dm-thin.c` — 複雜的 ctr（pool 建立）+ dtr（資源釋放）
  - https://github.com/torvalds/linux/blob/master/drivers/md/dm-thin.c
- `drivers/md/dm-stripe.c` — 簡潔的 ctr/dtr 參考
  - https://github.com/torvalds/linux/blob/master/drivers/md/dm-stripe.c
- `drivers/md/dm-linear.c` — 最簡單的 DM target 範例
  - https://github.com/torvalds/linux/blob/master/drivers/md/dm-linear.c

---

## 8. Status 報告（dmsetup status）

| # | 功能 | 說明 |
|---|------|------|
| 48 | STATUSTYPE_INFO | policy + mirror + error + per-disk A/D + read/write |
| 49 | STATUSTYPE_TABLE | 碟名列表 |
| 50 | STATUSTYPE_IMA | IMA placeholder |

**開源參考：**
- `drivers/md/dm-raid1.c` — 詳細的 status 輸出（per-mirror state + sync 進度）
  - https://github.com/torvalds/linux/blob/master/drivers/md/dm-raid1.c
- `drivers/md/dm-thin.c` — 模式感知的 status（PM_WRITE/PM_READ_ONLY/PM_FAIL）
  - https://github.com/torvalds/linux/blob/master/drivers/md/dm-thin.c
- `drivers/md/dm-dust.c` — 狀態感知的 status（fail_read_on_bad_block）
  - https://github.com/torvalds/linux/blob/master/drivers/md/dm-dust.c

---

## 9. Metadata 解析（Kernel）

| # | 功能 | 說明 |
|---|------|------|
| 51 | Kernel file-based config | `filp_open()` + `kernel_read()` + key=value 解析 |
| 52 | Version/chunk/segment/disk parsing | 完整參數驗證 |
| 53 | Segment disks/weight CSV parsing | 逗號分隔 u32 陣列 |

**開源參考：**
- `drivers/md/dm-thin-metadata.c` — 複雜的 metadata 管理（B-tree + superblock）
  - https://github.com/torvalds/linux/blob/master/drivers/md/dm-thin-metadata.c
- `drivers/md/dm-log.c` — disk log 狀態持久化
  - https://github.com/torvalds/linux/blob/master/drivers/md/dm-log.c
- `fs/configfs/configfs.c` — kernel-userspace 配置介面
  - https://github.com/torvalds/linux/blob/master/fs/configfs/configfs.c
- `drivers/md/dm-table.c` — DM table 解析（裝置路徑 + offset）
  - https://github.com/torvalds/linux/blob/master/drivers/md/dm-table.c

---

## 10. 熱插拔 + 動態偵測（NSC 計畫新增目標）

| # | 功能 | 說明 | 參考專案 |
|---|------|------|----------|
| 54 | **Online Add** | 新增碟時動態擴展 segment | mdadm `Grow_Add_device()` |
| 55 | **Online Remove** | 移除碟前遷移 stripe | mdadm `--remove` |
| 56 | **uevent listener** | netlink 偵測碟的新增/移除 | `lib/kobject_uevent.c` |
| 57 | **sysfs monitor** | 偵測速度退化 | `block/disk-events.c` |

**開源參考：**
- `mdadm/Grow.c` — 線性陣列的熱新增（`Grow_Add_device`）
  - https://github.com/md-raid-utilities/mdadm/blob/main/Grow.c
- `drivers/md/dm.c` — DM 裝置的動態建立/刪除（`dev_create`/`dev_remove`）
  - https://github.com/torvalds/linux/blob/master/drivers/md/dm.c
- `lib/kobject_uevent.c` — uevent netlink 廣播
  - https://github.com/torvalds/linux/blob/master/lib/kobject_uevent.c
- `block/disk-events.c` — 磁碟事件輪詢框架
  - https://github.com/torvalds/linux/blob/master/block/disk-events.c

---

## 11. 結構化診斷日誌（NSC 計畫新增目標）

| # | 功能 | 說明 | 參考專案 |
|---|------|------|----------|
| 58 | **Ring buffer** | 記錄 I/O 事件 | `kernel/trace/ring_buffer.c` |
| 59 | **Log level** | 動態調整詳細程度 | ftrace event filtering |
| 60 | **dmsetup query** | 即時查詢日誌 | DM message framework |

**開源參考：**
- `kernel/trace/ring_buffer.c` — 高效能 ring buffer 實作
  - https://github.com/torvalds/linux/blob/master/kernel/trace/ring_buffer.c
- `kernel/trace/trace.c` — trace event 管理 + log level 控制
  - https://github.com/torvalds/linux/blob/master/kernel/trace/trace.c
- `drivers/md/dm-dust.c` — DM message 結果回傳（`result` buffer）
  - https://github.com/torvalds/linux/blob/master/drivers/md/dm-dust.c
- `include/linux/ring_buffer.h` — ring buffer API 頭檔
  - https://github.com/torvalds/linux/blob/master/include/linux/ring_buffer.h

---

## 測試覆蓋率

| 類別 | 功能數 | 有測試 |
|------|:------:|:------:|
| I/O dispatch | 6 | 6 |
| Load balancing | 4 | 4 |
| Stale detection | 4 | 4 |
| Per-disk stats | 5 | 5 |
| Per-CPU stats | 4 | 4 |
| DM messages | 17 | 17 |
| DM lifecycle | 7 | 7 |
| Status | 3 | 3 |
| Metadata | 3 | 3 |
| **Total** | **53** | **53** |

---

## ⚠️ 已知 Bug

- ~~`set_ema_shift`：`argc == 1` 但讀取 `argv[1]`，會 kernel oops~~ ✅ 已修復（改為 `argc == 2`）
- ~~`bio_alloc_clone` mirror write：傳 `NULL` bioset 導致 kernel NULL pointer dereference~~ ✅ 已修復（改為 `&fs_bio_set`）
