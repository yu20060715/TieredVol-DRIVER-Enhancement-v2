# TieredVol v5.0.0 功能目錄表

TieredVol 裝置映射目標（device-mapper target）所有已實現功能的技術參考文件。v5.0.0 為 v4.6.0 的完全重寫版本。

---

## 檔案結構（v5.0）

| 檔案 | 職責 | 行數 |
|------|------|------|
| `tieredvol.h` | 標頭檔，所有結構體定義 | 258 |
| `tieredvol_core.c` | DM 生命週期、map 分派、模組 init/exit | 595 |
| `tieredvol_map.c` | 邏輯→實體映射：static/adaptive/random | 219 |
| `tieredvol_mirror.c` | 鏡像 I/O、per-CPU pending、ts ring、retry、rebuild | 571 |
| `tieredvol_log.c` | 日誌 ring、EMA 衰減計時器 | 146 |
| `tieredvol_meta.c` | 配置檔解析器 + CRC32C | 397 |
| `tieredvol_message.c` | 27 個 dmsetup message 處理器 | 780 |
| `tieredvol_sysfs.c` | sysfs 介面（7 個屬性） | 273 |

---

## 核心資料結構

### struct tieredvol_segment（tieredvol.h:20-29）

| 欄位 | 型別 | 說明 |
|------|------|------|
| `logical_begin` | `sector_t` | segment 起始 sector |
| `logical_end` | `sector_t` | segment 結束 sector |
| `disk_count` | `u32` | 該 segment 使用的碟數 |
| `disk_index[16]` | `u32` | 碟索引陣列 |
| `weight[16]` | `u32` | 加權條帶權重 |
| `stripe_size` | `u32` | 條帶大小（sectors） |
| `mirror_enabled` | `bool` | 鏡像啟用標誌 |
| `mirror_disk` | `u32` | 鏡像目標碟索引 |

### struct tieredvol_metadata（tieredvol.h:31-43）

| 欄位 | 型別 | 說明 |
|------|------|------|
| `version` | `u32` | 配置版本 |
| `chunk_size` | `u32` | 基礎 chunk 大小（bytes） |
| `segment_count` | `u32` | segment 數量 |
| `disk_count` | `u32` | 碟數量 |
| `disk_names[16][64]` | `char` | 碟裝置路徑 |
| `segments[16]` | `struct tieredvol_segment` | segment 陣列 |
| `runtime_policy` | `int` | 執行時策略（覆寫） |
| `stale_ms` | `u32` | stale 超時（ms） |
| `ema_shift` | `u32` | EMA 權重位移 |
| `wear_bias` | `u32` | 磨損偏差因子 |

### struct tv_io_stats（tieredvol.h:60-70）

| 欄位 | 型別 | 說明 |
|------|------|------|
| `in_flight_bytes[16]` | `atomic_t` | 每碟 in-flight 位元組 |
| `total_write_bytes[16]` | `u64` | 每碟總寫入位元組 |
| `total_read_bytes[16]` | `u64` | 每碟總讀取位元組 |
| `total_write_ops[16]` | `u64` | 每碟總寫入操作數 |
| `total_read_ops[16]` | `u64` | 每碟總讀取操作數 |
| `total_latency_ns[16]` | `u64` | 每碟累積延遲（ns） |
| `total_completions[16]` | `u64` | 每碟累積完成數 |
| `interval_completions[16]` | `u64` | 每碟區間完成數 |

### struct tv_adaptive_state（tieredvol.h:72-86）

| 欄位 | 型別 | 說明 |
|------|------|------|
| `ema_weight_shift` | `u32` | EMA alpha 位移（0-10） |
| `ema_load[16]` | `s64` | 每碟 EMA 載入分數 |
| `stale_after_ns` | `u64` | stale 超時（ns） |
| `stale[16]` | `bool` | 每碟 stale 標誌 |
| `stale_marked_ns[16]` | `ktime_t` | 每碟 stale 標記時間 |
| `grace_until_ns[16]` | `ktime_t` | 每碟保護期截止時間 |
| `last_finish_ns[16]` | `ktime_t` | 每碟最後 I/O 完成時間 |
| `decay_timer` | `struct timer_list` | EMA 衰減計時器 |
| `wear_bias` | `u32` | 磨損偏差因子 |
| `policy` | `int` | 當前分派策略 |
| `ema_latency_ns[16]` | `s64` | 每碟 EMA 延遲（ns） |
| `ema_iops[16]` | `s64` | 每碟 EMA IOPS |

### struct tieredvol_ctx（tieredvol.h:111-129）

| 欄位 | 型別 | 說明 |
|------|------|------|
| `ti` | `struct dm_target *` | DM target 指標 |
| `meta` | `struct tieredvol_metadata *` | 元資料 |
| `devs` | `struct dm_dev **` | DM 裝置陣列 |
| `disk_sectors` | `sector_t *` | 每碟 sector 數 |
| `config_path` | `char *` | 配置檔路徑 |
| `ndisks` | `u32` | 碟數量 |
| `min_chunk_sectors` | `sector_t` | 最小 chunk（sectors） |
| `stripe_sectors` | `sector_t` | 條帶大小（sectors） |
| `io` | `struct tv_io_stats` | I/O 統計 |
| `deg` | `struct tv_degradation` | 降級狀態 |
| `adaptive` | `struct tv_adaptive_state` | 自適應狀態 |
| `mirror` | `struct tv_mirror_stats` | 鏡像統計 |
| `rebuild` | `struct tv_rebuild_state` | 重建狀態 |
| `mirror_enabled_any` | `bool` | 任一 segment 鏡像啟用 |
| `trigger_event` | `struct work_struct` | 事件觸發工作 |
| `mirror_pw_pool` | `mempool_t *` | 鏡像 bio mempool |
| `retry_ctx_pool` | `mempool_t *` | 重試上下文 mempool |

### struct tv_pending_read_cpu / tv_pending_write_cpu（tieredvol.h:196-213）

| 欄位 | 型別 | 說明 |
|------|------|------|
| `entries[64]` | `struct bio *` | pending bio 陣列 |
| `head` | `u32` | 環形索引頭部 |
| `count` | `u32` | 當前 pending 數量 |

---

## 1. I/O 分派

I/O 分派層負責將邏輯位元組偏移量轉換為實體碟分配。TieredVol 支援三種分派策略，每種策略在對應的 segment 中選碟並計算實體 sector 偏移量。分派策略實現在 `tieredvol_map.c`，主分派入口在 `tieredvol_core.c`。

### #1 靜態加權分派（Static Weighted Dispatch）

> 根據預計算的加權條帶邊界，確定性地將邏輯偏移量映射到碟。

**實現方法：** `tv_map_logical()`，位於 `tieredvol_map.c:4-65`。根據 segment 權重建構前綴和邊界陣列，線性掃描定位目標碟。實體偏移量計算公式：`stripe_no * weight[disk] * CHUNK_SIZE + (offset_in - boundary[disk])`。

**核心 API：** `bio_set_dev()`, `bdev_nr_sectors()`

**參考文獻：**
1. `drivers/md/dm-stripe.c` — 經典條帶化目標的加權分配
2. `drivers/md/dm-switch.c` — 基於 region 的 bio 分派與 sector 重映射

---

### #2 自適應多因子分派（Adaptive Multi-Factor Dispatch）

> 使用多因子評分（EMA 載入 + EMA 延遲 + 磨損懲罰），為每個 bio 選擇最優碟，自動跳過 stale 碟。

**實現方法：** `tv_map_logical_adaptive()`，位於 `tieredvol_map.c:132-149`。評分公式：

```
score = ema_load[d] + ema_latency_ns[d] / 1000000 + wear_bias * total_write_bytes[d] / total_writes
```

遍歷候選碟，選最小 score 值。若所有候選碟均為 stale，則回退到任意有效碟。EMA 延遲項以微秒為單位（除以 1000000 轉換 ns→us），使延遲因子與載入因子在同一數量級。

**Fallback 兩階段掃描：** 當所有候選碟的 score 都不理想（全部 stale/degraded）時，fallback 執行兩階段掃描：第一階段跳過 stale/degraded 碟，優先選非 stale 碟；若第一階段找不到（全部都是 stale/degraded），第二階段接受任何有效碟（包含 stale/degraded），確保 I/O 不會失敗。

**核心 API：** `get_random_u32()`, 原子 EMA 更新（`tv_decay_timer_fn()`）

**參考文獻：**
1. Jiao & Kim, "Asymmetric RAID: Rethinking RAID for SSD Heterogeneity" — HotStorage'24
2. `block/kyber-iosched.c` — 基於 token 的動態深度調整
3. `block/mq-deadline.c` — 請求排序與基於時間的過期機制

---

### #3 隨機分派（Random Dispatch）

> 在 segment 內均勻隨機選碟。

**實現方法：** `tv_map_logical_random()`，位於 `tieredvol_map.c:158-201`。使用 `get_random_u32() % seg->disk_count` 進行均勻選取。

**核心 API：** `get_random_u32()`

**參考文獻：**
1. `drivers/md/dm-switch.c` — 隨機路徑選擇回退

---

### #4 Bio Sector 重映射（Bio Sector Remapping）

> 透過重寫 `bi_bdev` 和 `bi_iter.bi_sector`，將 bio 從 DM 虛擬裝置重定向到正確的實體碟。

**實現方法：** `tieredvol_map()`，位於 `tieredvol_core.c:33-164`。呼叫 `bio_set_dev(bio, ctx->devs[cur.disk]->bdev)` 並設定 `bio->bi_iter.bi_sector = cur.offset >> SECTOR_SHIFT`。

**核心 API：** `bio_set_dev()`, `SECTOR_SHIFT`

**參考文獻：**
1. `drivers/md/dm-crypt.c` — Bio clone + 重定向到加密層
2. `drivers/md/dm-linear.c` — 最簡單的 bio 重映射目標

---

### #5 無效碟錯誤（Invalid Disk Error）

> 當計算出的碟索引超出範圍時，返回 bio 錯誤。

**實現方法：** `tieredvol_map()`，位於 `tieredvol_core.c:89-96`。檢查 `cur.disk < 0 || cur.disk >= ctx->ndisks`，呼叫 `bio_io_error(bio)` 並返回 `DM_MAPIO_SUBMITTED`。

**核心 API：** `bio_io_error()`

**參考文獻：**
1. `drivers/md/dm.c` — DM 核心錯誤處理模式

---

### #6 寫入鏡像（Write Mirroring）

> Clone 寫入 bio 並提交到指定的鏡像碟，實現資料冗餘。

**實現方法：** `tieredvol_map()`，位於 `tieredvol_core.c:126-160`。當 `seg->mirror_enabled` 且鏡像碟與主碟不同時，從 `ctx->mirror_pw_pool` 分配 clone bio（`mempool_alloc()`），設定自定義 `tv_mirror_end_io` 完成處理器，透過 `submit_bio()` 提交。使用 mempool 確保零 OOM。

**核心 API：** `bio_alloc_clone()`, `submit_bio()`, `bio_put()`, `mempool_alloc()`

**參考文獻：**
1. `drivers/md/dm-raid1.c` — Bio clone + 雙碟寫入 + 自定 bi_end_io

---

## 2. 負載均衡 + 自適應衰減

負載均衡使用 in-flight 位元組計數器和三重 EMA 平滑濾波器（載入、IOPS、延遲）追蹤每碟 I/O 壓力。硬體計時器以自適應間隔衰減這些計數器，為多因子自適應分派策略提供平滑的碟狀態估計。

### #7 EMA 載入計算（EMA Load Calculation）

> 使用指數移動平均計算每碟載入：`ema = ema * (1 - alpha) + snapshot * alpha`，alpha 透過 `ema_weight_shift` 可調。

**實現方法：** `tv_decay_timer_fn()`，位於 `tieredvol_log.c:81-83`。Alpha 預設為 `1 << 3 = 8`（滿值 1024，shift=3）。快照為原子 in-flight 位元組計數器，每 tick 透過 `atomic_xchg()` 歸零。

**核心 API：** `timer_list`, `atomic_xchg()`

**參考文獻：**
1. `kernel/sched/fair.c` — CFS per-CPU 載入追蹤
2. `drivers/md/dm-thin.c` — Pool 模式切換與低水位回呼

---

### #8 EMA IOPS 計算（EMA IOPS Calculation）

> 使用 EMA 追蹤每碟完成 IOPS，以平滑突發流量。

**實現方法：** `tv_decay_timer_fn()`，位於 `tieredvol_log.c:86-88`。快照為原子 `interval_completions` 計數器，每 tick 透過 `atomic_xchg()` 歸零後更新 EMA。

**核心 API：** `atomic_xchg()`

---

### #9 EMA 延遲測量（EMA Latency Measurement）

> 使用 EMA 追蹤每碟 I/O 延遲，結合 timestamp ring 實現無鎖測量。

**實現方法：** `tv_decay_timer_fn()`，位於 `tieredvol_log.c:91-102`。從 `total_latency_ns` 和 `total_completions` 計算區間平均延遲，更新 `ema_latency_ns[d]`。Timestamp ring（`tv_ts_ring`）在 `tieredvol_mirror.c:155-219` 中實現無鎖延遲追蹤：`tv_ts_submit()`（:170-188）記錄提交時間，`tv_ts_complete()`（:190-219）計算完成時間差。

**核心 API：** `ktime_get_boottime_ns()`

---

### #10 In-flight 位元組追蹤（In-flight Byte Tracking）

> 使用每碟原子計數器計算目前傳輸中的位元組數。

**實現方法：** `tieredvol_map()`，位於 `tieredvol_core.c:109`。`atomic_add(bio->bi_iter.bi_size, &ctx->io.in_flight_bytes[cur.disk])`。衰減計時器透過 `atomic_xchg()` 歸零。

**核心 API：** `atomic_add()`, `atomic_xchg()`

**參考文獻：**
1. `block/blk-mq.c` — blk-mq per-tag 統計

---

### #11 自適應衰減計時器間隔（Adaptive Decay Timer Interval）

> 計時器間隔根據 I/O 活動動態調整：忙碌時 100ms、閒置時 1s。

**實現方法：** `tv_decay_timer_fn()`，位於 `tieredvol_log.c:143-145`。若上一區間有任何完成事件（`total_completions[d] > 0`），使用 `TV_DECAY_FAST = HZ/10`（100ms）；否則使用 `TV_DECAY_SLOW = HZ`（1s）。透過 `mod_timer()` 重新武裝。

**核心 API：** `mod_timer()`

**參考文獻：**
1. `kernel/time/timer_list.c` — timer_list API 參考

---

## 3. Stale 碟偵測 + 恢復

Stale 偵測識別已停止回應 I/O 的碟。碟在可配置超時後被標記為 stale，然後在 I/O 恢復時或冷卻期後自動恢復。恢復保護期（grace period）防止恢復後立即被重新標記為 stale。

### #12 Stale 標記（Stale Marking）

> 當 `stale_after_ns`（預設 5 秒）內無 I/O 完成時，將碟標記為 stale。

**實現方法：** `tv_decay_timer_fn()`，位於 `tieredvol_log.c:112-123`。檢查 `now > ctx->adaptive.grace_until_ns[i]` 且 `(now - ctx->adaptive.last_finish_ns[i]) > ctx->adaptive.stale_after_ns`。設定 `ctx->adaptive.stale[i] = true` 並透過 `tv_log(TV_LOG_WARN, ...)` 記錄。

**核心 API：** `ktime_get_boottime_ns()`

**參考文獻：**
1. `drivers/md/dm-dust.c` — 壞軌模擬與啟用/停用
2. `drivers/md/dm-raid1.c` — Mirror 故障轉移與錯誤偵測

---

### #13 Stale 恢復 — I/O 觸發（Stale Recovery, I/O Triggered）

> 當 stale 碟收到新 I/O 完成時立即恢復，並啟動新保護期。

**實現方法：** `tv_decay_timer_fn()`，位於 `tieredvol_log.c:124-129`。當 `ctx->adaptive.stale[i] && snapshot > 0`，設定 `stale[i] = false` 且 `grace_until_ns[i] = now + stale_after_ns`。

**核心 API：** 無（狀態機）

**參考文獻：**
1. `drivers/md/dm-log.c` — Dirty region 追蹤與恢復

---

### #14 Stale 恢復 — 冷卻（Stale Recovery, Cooldown）

> 在 2 倍 stale 超時後自動恢復 stale 碟，即使沒有新 I/O。

**實現方法：** `tv_decay_timer_fn()`，位於 `tieredvol_log.c:130-138`。當 `(now - ctx->adaptive.stale_marked_ns[i]) > 2 * ctx->adaptive.stale_after_ns`，設定 `stale[i] = false`。

**核心 API：** 無（狀態機）

**參考文獻：**
1. `block/disk-events.c` — 磁碟事件輪詢與超時

---

### #15 保護期（Grace Period）

> 保護新恢復的碟不被立即重新標記為 stale。

**實現方法：** `tv_decay_timer_fn()`，位於 `tieredvol_log.c:126,134`。恢復時設定 `ctx->adaptive.grace_until_ns[i] = now + ctx->adaptive.stale_after_ns`。第 115 行的 stale 檢查驗證 `now > ctx->adaptive.grace_until_ns[i]` 後才標記 stale。

**核心 API：** 無（狀態機）

**參考文獻：**
1. `drivers/md/dm-raid1.c` — Mirror 恢復保護期

---

## 4. Per-disk I/O 統計

每個實體碟的累積計數器，在裝置映射目標的整個生命週期中追蹤。統計資料儲存在 `struct tv_io_stats` 中。

### #16 讀取位元組計數器（Read Bytes Counter）

> 每碟總讀取位元組數。

**實現方法：** `ctx->io.total_read_bytes[cur.disk] += bio->bi_iter.bi_size`，位於 `tieredvol_core.c:114`。

**參考文獻：** `drivers/md/dm.c` — DM 核心 bio 統計

---

### #17 讀取操作計數器（Read Ops Counter）

> 每碟總讀取操作數。

**實現方法：** `ctx->io.total_read_ops[cur.disk]++`，位於 `tieredvol_core.c:115`。

**參考文獻：** `block/blk-mq.c` — blk-mq per-tag 統計

---

### #18 寫入位元組計數器（Write Bytes Counter）

> 每碟總寫入位元組數。

**實現方法：** `ctx->io.total_write_bytes[cur.disk] += bio->bi_iter.bi_size`，位於 `tieredvol_core.c:111`。

**參考文獻：** `drivers/nvme/host/core.c` — NVMe per-controller 統計

---

### #19 寫入操作計數器（Write Ops Counter）

> 每碟總寫入操作數。

**實現方法：** `ctx->io.total_write_ops[cur.disk]++`，位於 `tieredvol_core.c:112`。

**參考文獻：** `block/genhd.c` — `part_stat_show` per-partition 統計

---

### #20 錯誤計數器（Error Counter）

> 每碟原子錯誤計數，在 bio 完成錯誤時遞增。

**實現方法：** `ctx->error_count[disk]` 是 `atomic_t` 陣列，在 `tieredvol_ctr()` 中分配。透過 `atomic_read()` 在 `tieredvol_status()` 和降級偵測中讀取。

**核心 API：** `atomic_read()`, `atomic_set()`, `atomic_inc()`

**參考文獻：**
1. `drivers/md/dm.c` — DM 錯誤計數
2. `drivers/md/dm-raid1.c` — Mirror 錯誤追蹤

---

## 5. DM Message 指令

透過 `dmsetup message` 的執行時控制介面。所有指令在 `tv_message()` 中分派（`tieredvol_message.c`，27 個處理器）。

### #21 `reset_stats`（指令 0）

> 清除所有 per-disk I/O 統計（操作數、位元組數、延遲、完成計數）。

**實現方法：** `tieredvol_message.c`。歸零 `tv_io_stats` 中的所有 7 個計數器陣列。

---

### #22 `show_stats`（指令 1）

> 返回每碟讀取/寫入操作數和位元組數。

**實現方法：** `tieredvol_message.c`。透過 `DMINFO()` 輸出每碟的 `total_read_ops/bytes` 和 `total_write_ops/bytes`。

---

### #23 `show_inflight`（指令 3）

> 返回每碟目前 in-flight 位元組數。

**實現方法：** `tieredvol_message.c`。讀取 `atomic_read(&ctx->io.in_flight_bytes[i])`。

---

### #24 `show_io_stats`（指令 4）

> 返回完整的 per-disk I/O 統計，包括延遲和完成計數。

**實現方法：** `tieredvol_message.c`。輸出 `total_write/read_bytes`, `total_write/read_ops`, `total_latency_ns`, `total_completions`。

---

### #25 `reset_io_stats`（指令 5）

> 歸零所有 7 個 per-disk I/O 統計計數器。

**實現方法：** `tieredvol_message.c`。歸零 `in_flight_bytes`, `total_write/read_bytes`, `total_write/read_ops`, `total_latency_ns`, `total_completions`, `interval_completions`。

---

### #26 `adaptive_on`（指令 6）

> 將分派策略切換為自適應（基於多因子評分的負載均衡）。

**實現方法：** `tieredvol_message.c`。設定 `ctx->adaptive.policy = TV_POLICY_ADAPTIVE`。

---

### #27 `adaptive_off`（指令 7）

> 將分派策略切換為靜態（基於加權邊界）。

**實現方法：** `tieredvol_message.c`。設定 `ctx->adaptive.policy = TV_POLICY_STATIC`。

---

### #28 `set_policy <name>`（指令 8）

> 設定分派策略為 static、adaptive 或 random。

**實現方法：** `tieredvol_message.c`。驗證 `argv[1]` 是否為 "static"、"adaptive"、"random"。

---

### #29 `set_ema_shift <shift>`（指令 9）

> 設定 EMA 權重位移（0-10）。Alpha = `1 << shift`（滿值 1024）。

**實現方法：** `tieredvol_message.c`。透過 `kstrtou32()` 驗證 shift <= 10。

**核心 API：** `kstrtou32()`

---

### #30 `set_stale_ms <ms>`（指令 10）

> 設定 stale 偵測超時（毫秒）。

**實現方法：** `tieredvol_message.c`。轉換為 ns：`ctx->adaptive.stale_after_ns = (u64)ms * 1000000ULL`。

---

### #31 `show_adaptive`（指令 11）

> 返回策略、EMA 位移、stale 超時、磨損偏差，以及每碟載入/延遲/磨損/stale 狀態。延遲以微秒顯示。

**實現方法：** `tieredvol_message.c:304-327`。輸出包含 `lat=XXus` 格式的延遲欄位，將 `ema_latency_ns[d] / 1000000` 轉換為微秒。

---

### #32 `show_wear`（指令 12）

> 返回磨損偏差和每碟總寫入位元組數。

**實現方法：** `tieredvol_message.c`。輸出 `wear_bias` 和每碟 `total_write_bytes[d]`。

---

### #33 `set_wear_bias <bias>`（指令 13）

> 設定磨損懲罰因子（0-1024）。值越高，自適應分派對高磨損碟的懲罰越重。

**實現方法：** `tieredvol_message.c`。驗證 `bias <= 1024`。

---

### #34 `reset_wear`（指令 14）

> 歸零每碟寫入位元組數（磨損計數器）。

**實現方法：** `tieredvol_message.c`。將所有 `total_write_bytes[d]` 設為 0。

---

### #35 `show_mirror`（指令 15）

> 返回鏡像寫入操作數/位元組數、錯誤計數，以及每 segment 鏡像配置。

**實現方法：** `tieredvol_message.c`。輸出 `mirror_stats` 和 segment 鏡像設定。

---

### #36 `set_mirror <seg> <disk>`（指令 16）

> 為 segment 啟用鏡像，指定目標碟。

**實現方法：** `tieredvol_message.c`。驗證 `seg_idx < segment_count` 且 `disk_idx < ndisks`。設定 `seg->mirror_enabled = true` 且 `seg->mirror_disk = disk_idx`。設定 `ctx->mirror_enabled_any = true`（`tieredvol_mirror.c:398`）。

---

### #37 `show_log`（指令 17）

> 非破壞性讀取日誌 ring：使用 `kfifo_out()` + `kfifo_in()` 複製條目後還原，不消費任何條目。

**實現方法：** `tieredvol_message.c:487-526`。在自旋鎖下使用 `kfifo_out()` 排出條目到臨時陣列，格式化輸出後透過 `kfifo_in()` 寫回 ring，保持原始內容不變。

**核心 API：** `kfifo_out()`, `kfifo_in()`, `raw_spin_lock_irqsave()`

---

### #38 `clear_log`（指令 18）

> 重置日誌 ring 緩衝區。

**實現方法：** `tieredvol_message.c`。在自旋鎖下呼叫 `kfifo_reset()`。

---

### #39 `set_loglevel <0-3>`（指令 19）

> 設定日誌詳細程度：OFF(0) / ERROR(1) / WARN(2) / INFO(3)。

**實現方法：** `tieredvol_message.c`。設定全域 `tv_log_level`。

---

### #40 `show_errors`（指令 20）

> 返回每碟錯誤計數。

**實現方法：** `tieredvol_message.c`。透過 `atomic_read(&ctx->error_count[d])` 讀取。

---

### #41 `reset_errors`（指令 21）

> 歸零所有 per-disk 錯誤計數。

**實現方法：** `tieredvol_message.c`。將所有 `error_count[d]` 設為 0。

---

### #42 `set_error_threshold <n>`（指令 22）

> 設定觸發降級模式的錯誤閾值。

**實現方法：** `tieredvol_message.c`。設定 `ctx->deg.error_threshold = n`。

---

### #43 `show_degraded`（指令 23）

> 返回降級模式狀態：是否已降級、觸發原因、錯誤閾值。

**實現方法：** `tieredvol_message.c`。輸出 `ctx->deg` 狀態。

---

### #44 `clear_degraded`（指令 24）

> 手動清除降級模式標誌。

**實現方法：** `tieredvol_message.c`。設定 `ctx->deg.is_degraded = false`。

---

### #45 `start_rebuild [max_bytes]`（指令 25）

> 啟動背景重建執行緒，可選每次迭代最大位元組數。

**實現方法：** `tieredvol_message.c`。建立 kthread `tv_rebuild_thread()`，使用指數退避重試。若已在重建中則返回錯誤。可選 `max_bytes` 參數限制每次迭代處理量。

---

### #46 `stop_rebuild`（指令 26）

> 停止背景重建執行緒。

**實現方法：** `tieredvol_message.c`。設定停止標誌並等待 kthread 退出。

---

### #47 `show_rebuild`（指令 27）

> 返回重建狀態：進行中/已完成/已停止、已處理位元組、總位元組。

**實現方法：** `tieredvol_message.c`。輸出 `ctx->rebuild` 狀態。

---

## 6. DM Target 生命週期

管理 tieredvol 目標生命週期的標準裝置映射目標回呼函數。

### #48 構造函數（Constructor, ctr）

> 分配上下文、從配置檔案載入元資料、取得 DM 裝置、分配 mempool，並啟動衰減計時器。

**實現方法：** `tieredvol_ctr()`，位於 `tieredvol_core.c`。流程：
1. 解析參數（期望 1 個參數：配置檔案路徑）
2. `kzalloc(sizeof(*ctx))` — 分配上下文
3. `tv_metadata_load_kernel()` — 透過 `filp_open()` + `kernel_read()` 解析 key=value 配置檔案
4. `kcalloc(ndisks, sizeof(*ctx->devs))` — 分配裝置陣列
5. `dm_get_device()` — 取得每個 DM 裝置
6. 計算所有 segment 的 `min_chunk_sectors`
7. `dm_set_target_max_io_len()` — 設定最大 I/O 大小
8. `mempool_create()` — 建立 mirror_pw_pool 和 retry_ctx_pool
9. `timer_setup()` + `mod_timer()` — 啟動衰減計時器

**核心 API：** `kzalloc()`, `kcalloc()`, `dm_get_device()`, `dm_set_target_max_io_len()`, `timer_setup()`, `mempool_create()`

---

### #49 析構函數（Destructor, dtr）

> 停止衰減計時器、停止重建執行緒、刷新待處理工作、釋放 DM 裝置，並釋放所有記憶體。

**實現方法：** `tieredvol_dtr()`，位於 `tieredvol_core.c`。流程：
1. `timer_delete_sync()` — 停止衰減計時器
2. `kthread_stop()` — 停止重建執行緒（若運行中）
3. `flush_work()` — 完成待處理的 trigger_event 工作
4. `dm_put_device()` — 釋放每個 DM 裝置
5. `mempool_destroy()` — 銷毀 mempool
6. `kfree()` — 釋放所有分配

**核心 API：** `timer_delete_sync()`, `kthread_stop()`, `flush_work()`, `dm_put_device()`, `mempool_destroy()`, `kfree()`

---

### #50 I/O 提示（IO Hints）

> 向 DM 框架報告區區大小、區塊大小和最佳 I/O 大小。

**實現方法：** `tieredvol_io_hints()`，位於 `tieredvol_core.c`。設定 `logical_block_size = 512`, `physical_block_size = 512`, `chunk_sectors = min_chunk_sectors`, `io_opt = stripe_sectors`。

**核心 API：** `struct queue_limits`

---

### #51 迭代裝置（Iterate Devices）

> 返回所有底層 DM 裝置，用於狀態報告和 ioctl 透傳。

**實現方法：** `tieredvol_iterate_devices()`，位於 `tieredvol_core.c`。迭代 `ctx->devs[0..ndisks-1]` 並對每個裝置呼叫回呼函數。

**核心 API：** `iterate_devices_callout_fn`, `bdev_nr_sectors()`

---

### #52 準備 Ioctl（Prepare Ioctl）

> 返回第一個底層裝置的區塊裝置，用於 ioctl 透傳。

**實現方法：** `tieredvol_prepare_ioctl()`，位於 `tieredvol_core.c`。設定 `*bdev = ctx->devs[0]->bdev`。

**核心 API：** `struct block_device`

---

### #53 Flush/Discard 傳播（Flush/Discard Propagation）

> 將 flush 和 discard 命令傳播到所有底層碟。

**實現方法：** `tieredvol_ctr()`。設定 `ti->num_flush_bios = ctx->ndisks` 和 `ti->num_discard_bios = ctx->ndisks`。同時設定 `ti->flush_bypasses_map = true`。

---

### #54 模組初始化/退出（Module Init/Exit）

> 向內核註冊 DM 目標並建立工作佇列。

**實現方法：** `tieredvol_init()`，位於 `tieredvol_core.c:534-577`。呼叫 `dm_register_target()` 和 `alloc_workqueue("tieredvol_wq", WQ_UNBOUND | WQ_HIGHPRI, 0)`。退出時呼叫 `dm_unregister_target()` 和 `destroy_workqueue()`。

**核心 API：** `dm_register_target()`, `dm_unregister_target()`, `alloc_workqueue()`

---

## 7. 狀態報告

透過 `dmsetup status` 的狀態輸出，提供目標執行時狀態的可見性。

### #55 STATUSTYPE_INFO

> 返回策略、鏡像統計、錯誤計數，以及每碟 active/degraded 狀態和讀取/寫入計數器。包含 `status` message 指令的磁碟名稱和權重功能。

**實現方法：** `tieredvol_status()`，位於 `tieredvol_core.c`（STATUSTYPE_INFO case）。格式：`policy=N mirror=ops/bytes err=N Ddisk:rd=ops/bytes wr=ops/bytes`。

---

### #56 STATUSTYPE_TABLE

> 返回底層碟裝置名稱列表。

**實現方法：** `tieredvol_core.c`（STATUSTYPE_TABLE case）。空格分隔的碟名。

---

### #57 STATUSTYPE_IMA

> IMA（完整性測量架構）佔位符 — 返回空字串。

**實現方法：** `tieredvol_core.c`。設定 `result[0] = '\0'`。

---

## 8. 元資料解析

從 key=value 文字檔案載入 tieredvol 拓撲的內核態配置檔案解析器。v5.0 新增 CRC32C 校驗和非破壞性預掃描。

### #58 內核態檔案配置（Kernel File-based Config）

> 使用 `filp_open()` + `kernel_read()` 從內核態讀取配置檔案。

**實現方法：** `tv_metadata_load_kernel()`，位於 `tieredvol_meta.c`。以 `filp_open(path, O_RDONLY, 0)` 開啟檔案，透過 `kernel_read()` 讀取最多 1MB，然後逐行解析。

**核心 API：** `filp_open()`, `kernel_read()`, `i_size_read()`, `vmalloc()`, `vfree()`

**參考文獻：**
1. `drivers/md/dm-thin-metadata.c` — 複雜的元資料管理（B-tree + superblock）
2. `drivers/md/dm-log.c` — 磁碟日誌狀態持久化

---

### #59 Version/Chunk/Segment/Disk 解析

> 解析並驗證 version、chunk_size、segment_count、disk_count、碟名稱、segment 碟索引/權重 CSV，以及鏡像安全性。

**實現方法：** `tieredvol_meta.c:150-187`。使用 `parse_u32()` / `parse_u64()` 輔助函數。驗證 `disk_count <= TV_MAX_DISKS` 和 `segment_count <= TV_MAX_SEGS`。透過 `parse_csv_u32()`（:54-70）解析逗號分隔的 u32 陣列。鏡像安全性驗證（:377-387）確保 `mirror_disk < disk_count` 且鏡像碟不等於主碟。

**核心 API：** `kstrtoul()`, `kstrtoull()`, `strsep()`

**參考文獻：**
1. `fs/configfs/configfs.c` — 內核態-使用者態配置介面
2. `drivers/md/dm-table.c` — DM 表解析

---

### #60 CRC32C 校驗（CRC32C Validation）

> 使用 CRC32C 對配置內容進行完整性校驗，支援非破壞性預掃描。

**實現方法：** `tv_compute_config_crc()`，位於 `tieredvol_meta.c:98-133`。計算 `crc32c(0, config_start, config_len)`。CRC 預掃描（:183-219）在解析前先定位 CRC 段，使用 `save_nl`/`restore_nl` 和 `save_eq`/`restore_eq` 保存/恢復解析狀態，避免破壞性修改原始配置字串。

**核心 API：** `crc32c()`

---

## 9. 結構化診斷日誌

內核態環形緩衝區，記錄 I/O、stale、鏡像和配置事件，附帶時間戳和嚴重等級。事件透過 `dmsetup message` 查詢。v5.0 使用 `raw_spinlock` 替代 `spinlock`。

### #61 環形緩衝區（Ring Buffer）

> 固定大小的 `kfifo` 環形緩衝區（512 條目），帶 `raw_spinlock` 保護，記錄帶時間戳的日誌條目。

**實現方法：** `tieredvol_log.c:27`。`DECLARE_KFIFO(tv_log_fifo, struct tv_log_entry, TV_LOG_SIZE)` 搭配 `raw_spinlock_t tv_log_lock`。每次 `tv_log()` 呼叫透過 `raw_spin_lock_irqsave()` 取得自旋鎖，寫入 `struct tv_log_entry`（64 位元組：timestamp_ns, level, disk_idx, event_type, msg[48]），然後釋放。溢出時覆寫最舊條目。

**資料結構：** `struct tv_log_entry`，位於 `tieredvol.h`。

**核心 API：** `DECLARE_KFIFO()`, `kfifo_put()`, `kfifo_get()`, `kfifo_reset()`, `raw_spin_lock_irqsave()`

**參考文獻：**
1. emlog (nicupavel) — 帶溢出覆寫的環形緩衝區架構
   https://github.com/nicupavel/emlog
2. `kernel/samples/kfifo/record-example.c` — kfifo 使用模式
3. `kernel/trace/ring_buffer.c` — 高效能無鎖環形緩衝區

---

### #62 日誌等級（Log Level）

> 動態詳細程度控制：OFF(0) / ERROR(1) / WARN(2) / INFO(3)。

**實現方法：** `tieredvol_message.c`。`set_loglevel <0-3>` 設定全域 `tv_log_level`。`tv_log()` 函數檢查 `if (level > tv_log_level) return;`。

**核心 API：** `kstrtou32()`

**參考文獻：**
1. `kernel/trace/trace.c` — 帶日誌等級控制的追蹤事件管理

---

### #63 DM 查詢（show_log / clear_log）

> 透過 `show_log`（非破壞性讀取，使用 kfifo_out+kfifo_in）和 `clear_log`（重置環形緩衝區）進行即時日誌查詢。

**實現方法：** `tieredvol_message.c`。
- `show_log`（:487-526）：在 raw_spinlock 下使用 `kfifo_out()` 排出條目到臨時陣列，以格式 `LOG {ERR|WRN|INF} {I/O|STALE|RCVR|MIRR|CONF}: <msg>` 將每條目列印到 dmesg，然後透過 `kfifo_in()` 寫回 ring，保持內容不變。
- `clear_log`：在 raw_spinlock 下呼叫 `kfifo_reset()`。

**核心 API：** `kfifo_out()`, `kfifo_in()`, `kfifo_reset()`, `raw_spin_lock_irqsave()`, `DMINFO()`

**參考文獻：**
1. `drivers/md/dm-dust.c` — DM message 查詢/結果模式
2. `drivers/md/dm-log-writes.c` — 結構化 I/O 事件日誌

---

## 10. 鏡像 + Pending 追蹤

v5.0 引入 per-CPU pending 陣列實現無鎖鏡像追蹤，timestamp ring 實現精確延遲測量，以及 mempool 確保零 OOM。

### #64 Per-CPU 讀取 Pending 陣列（Per-CPU Pending Read Arrays）

> 每 CPU 一個 ring buffer，追蹤等待完成的鏡像讀取 bio，避免全域鎖競爭。

**實現方法：** `tv_pending_add()`，位於 `tieredvol_mirror.c:22-41`。使用 `DEFINE_PER_CPU(struct tv_pending_read_cpu, tv_pending_reads)` 定義 per-CPU 陣列。每個 CPU 維護 `entries[64]` 環形緩衝區，`head` 和 `count` 追蹤狀態。`tv_pending_find_and_remove()`（:43-79）在 bio 完成時從對應 CPU 的 pending 陣列中查找並移除。

**核心 API：** `DEFINE_PER_CPU()`, `this_cpu_read()`, `this_cpu_write()`

---

### #65 Per-CPU 寫入 Pending 陣列（Per-CPU Pending Write Arrays）

> 每 CPU 一個 ring buffer，追蹤等待完成的鏡像寫入 bio。

**實現方法：** 類似 #64，使用 `DEFINE_PER_CPU(struct tv_pending_write_cpu, tv_pending_writes)`，位於 `tieredvol_mirror.c:209-213`。

**核心 API：** `DEFINE_PER_CPU()`

---

### #66 Timestamp Ring（Timestamp Ring for Latency Measurement）

> 每碟 256 條目的環形緩衝區，記錄 bio 提交時間戳，用於精確延遲測量。

**實現方法：** `struct tv_ts_ring`（`tieredvol_mirror.c:161-165`），包含 `entries[256]`、`head`、`count`。`tv_ts_submit()`（:170-188）在 bio 提交時記錄 `ktime_get_boottime_ns()` 時間戳。`tv_ts_complete()`（:190-219）在 bio 完成時取出對應時間戳，計算延遲差並更新 `total_latency_ns[d]` 和 `total_completions[d]`。

**溢出處理：** 當 ring 滿（count == 256）時，`tv_ts_submit()` 覆寫最舊 entry（advance head），不丟棄新 entry。確保高 IOPS 下 latency EMA 不會因丟失資料而失真。

**鎖類型：** `tv_ts_lock_arr` 使用 `raw_spinlock_t`（非 `spinlock_t`），因為 `tv_ts_complete()` 在 bio end_io handler 中被呼叫，可能在 atomic context（不可睡眠）。

**核心 API：** `ktime_get_boottime_ns()`

---

### #67 Mempool（Mempool for Mirror Bio and Retry Contexts）

> 使用 mempool 管理鏡像 bio clone 和重試上下文，保證零 OOM 分配失敗。

**實現方法：**
- `ctx->mirror_pw_pool`：在 `tieredvol_ctr()` 中建立（`tieredvol_core.c:103`），用於鏡像寫入 bio clone 分配。
- `ctx->retry_ctx_pool`：在 `tieredvol_mirror.c:372` 建立，用於 I/O 失敗時的重試上下文。

**核心 API：** `mempool_create()`, `mempool_alloc()`, `mempool_free()`, `mempool_destroy()`

---

### #68 mirror_enabled_any 守衛標誌（mirror_enabled_any Guard Flag）

> 全域布林標誌，快速檢查是否有任何 segment 啟用了鏡像，避免不必要的鏡像邏輯執行。

**實現方法：** `ctx->mirror_enabled_any`，位於 `tieredvol.h:125`。在 `tieredvol_map()` 中（`tieredvol_core.c:279-285`）作為快速路徑檢查：若 `!ctx->mirror_enabled_any` 則跳過鏡像邏輯。在 `set_mirror` 指令中（`tieredvol_mirror.c:398`）設定為 true。

**核心 API：** 無（布林標誌）

---

## 11. 降級管理

降級子系統追蹤每碟錯誤計數，當錯誤超過可配置閾值時自動進入降級模式。提供錯誤重置、閾值設定、降級狀態查詢和手動恢復功能。

### #69 Per-disk 原子錯誤計數器（Per-disk Atomic Error Counter）

> 使用 `atomic_t` 陣列追蹤每碟累積錯誤數，在 bio 完成回呼中遞增。

**實現方法：** `ctx->error_count[disk]`，在 `tieredvol_ctr()` 中透過 `kcalloc()` 分配並以 `atomic_set(&ctx->error_count[i], 0)` 初始化。在 `tv_mirror_end_io()` 和其他錯誤路徑中透過 `atomic_inc()` 遞增。

**核心 API：** `atomic_set()`, `atomic_inc()`, `atomic_read()`

---

### #70 可配置錯誤閾值（Configurable Error Threshold）

> 可透過 `set_error_threshold <n>` 設定的錯誤閾值，超過時觸發降級模式。

**實現方法：** `ctx->deg.error_threshold`，透過 message 指令 #42（`set_error_threshold`）設定。在 `tieredvol_status()` 回呼中檢查每碟錯誤計數是否超過閾值。

**核心 API：** 無（配置值）

---

### #71 自動降級偵測（Auto-degradation Detection）

> 當任一碟的錯誤計數超過閾值時，自動進入降級模式。

**實現方法：** `tieredvol_status()` 中的降級檢查邏輯。遍歷所有碟的 `atomic_read(&ctx->error_count[d])`，若超過 `ctx->deg.error_threshold` 則設定 `ctx->deg.is_degraded = true` 並透過 `tv_log(TV_LOG_WARN, ...)` 記錄。

**核心 API：** `atomic_read()`

---

### #72 降級模式標誌（Degraded Mode Flag）

> 原子標誌，指示系統是否處於降級模式。

**實現方法：** `ctx->deg.is_degraded`，為 `bool` 或 `atomic_t` 標誌。在 `tv_log()` 輸出和 `tieredvol_status()` 中作為條件判斷。可透過 `clear_degraded` 指令手動重置。

**核心 API：** 無（狀態標誌）

---

### #73 降級模式恢復（Degraded Mode Recovery）

> 透過 `clear_degraded` 指令手動清除降級模式標誌。

**實現方法：** message 指令 #44（`clear_degraded`）。設定 `ctx->deg.is_degraded = false` 並記錄日誌。自動降級會在下次閾值檢查時重新觸發（若錯誤仍存在）。

**核心 API：** 無（狀態機）

---

## 12. 重建管理

重建子系統提供背景資料重建功能，使用 kthread 和指數退避重試機制。

### #74 kthread 背景重建（kthread-based Background Rebuild）

> 使用內核執行緒在背景進行資料重建，不阻塞 I/O 路徑。

**實現方法：** `start_rebuild` 指令（#45）建立 `kthread_run(tv_rebuild_thread, ...)`。重建執行緒以可配置的 `max_bytes` 每次迭代處理資料。`stop_rebuild` 指令（#46）設定停止標誌並透過 `kthread_stop()` 等待退出。

**核心 API：** `kthread_run()`, `kthread_stop()`, `kthread_should_stop()`

---

### #75 指數退避重試（Exponential Backoff Retry）

> 重建失敗時使用指數退避策略重試，避免緊密重試循環。

**實現方法：** `tv_rebuild_thread()` 中的重試邏輯。使用 `ctx->retry_ctx_pool`（`mempool`）分配重試上下文。失敗後等待時間指數增長（初始 1ms，最大 30s），然後重試。透過 `schedule_timeout_interruptible()` 實現等待。

**核心 API：** `mempool_alloc()`, `mempool_free()`, `schedule_timeout_interruptible()`

---

### #76 重建進度追蹤（Rebuild Progress Tracking）

> 追蹤重建進度：已處理位元組、總位元組、狀態（進行中/已完成/已停止）。

**實現方法：** `ctx->rebuild` 結構體（`struct tv_rebuild_state`）。包含 `is_running`、`is_complete`、`bytes_processed`、`total_bytes` 欄位。透過 `show_rebuild` 指令（#47）查詢。

**核心 API：** 無（狀態結構體）

---

## 13. Sysfs 介面

v5.0 新增 sysfs 介面，提供 /sys/kernel/tieredvol/ 下的7 個屬性，支援運行時查詢和設定。實現位於 `tieredvol_sysfs.c`（273 行）。

### #77 policy（唯讀）

> 當前分派策略：static、adaptive 或 random。

**實現方法：** `/sys/kernel/tieredvol/policy`。只讀屬性，返回 `ctx->adaptive.policy` 的文字表示。

---

### #78 stale_ms（可讀寫）

> stale 偵測超時（毫秒）。

**實現方法：** `/sys/kernel/tieredvol/stale_ms`。讀取返回 `ctx->adaptive.stale_after_ns / 1000000`。寫入設定 `ctx->adaptive.stale_after_ns`。

---

### #79 wear_bias（可讀寫）

> 磨損偏差因子（0-1024）。

**實現方法：** `/sys/kernel/tieredvol/wear_bias`。讀取返回 `ctx->adaptive.wear_bias`。寫入驗證 `<= 1024` 後設定。

---

### #80 ema_shift（可讀寫）

> EMA 權重位移（0-10）。

**實現方法：** `/sys/kernel/tieredvol/ema_shift`。讀取返回 `ctx->adaptive.ema_weight_shift`。寫入驗證 `<= 10` 後設定。

---

### #81 loglevel（可讀寫）

> 日誌詳細程度（0-3：OFF/ERROR/WARN/INFO）。

**實現方法：** `/sys/kernel/tieredvol/loglevel`。讀取返回 `tv_log_level`。寫入驗證 `<= 3` 後設定。

---

### #82 disk_count（唯讀）

> 碟數量。

**實現方法：** `/sys/kernel/tieredvol/disk_count`。只讀屬性，返回 `ctx->ndisks`。

---

### #83 status（唯讀）

> 綜合狀態字串。

**實現方法：** `/sys/kernel/tieredvol/status`。只讀屬性，返回類似 `dmsetup status` 的簡化狀態字串。

---

## 附錄：參考文獻

### 學術論文

| 論文 | 發表場合 | 相關性 |
|------|----------|--------|
| Jiao & Kim, "Asymmetric RAID: Rethinking RAID for SSD Heterogeneity" | HotStorage'24 | 原始 Asym-RAID 設計；TieredVol 在此基础上擴展多因子自適應分派 |

### Linux 內核源碼

| 檔案 | 連結 | 引用功能 |
|------|------|----------|
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

### 開源專案

| 專案 | 連結 | 引用功能 |
|------|------|----------|
| emlog (nicupavel) | https://github.com/nicupavel/emlog | #61 |
| sysprog21/kfifo-examples | https://github.com/sysprog21/kfifo-examples | #61 |
| mdadm | https://github.com/md-raid-utilities/mdadm | #59 |

---

## 測試覆蓋率摘要

| 類別 | 功能數 | 已測試 |
|------|:------:|:------:|
| I/O 分派 | 6 | 6 |
| 負載均衡 | 5 | 5 |
| Stale 偵測 | 4 | 4 |
| Per-disk 統計 | 5 | 5 |
| DM 指令 | 27 | 27 |
| DM 生命週期 | 7 | 7 |
| 狀態報告 | 3 | 3 |
| 元資料 | 3 | 3 |
| 結構化日誌 | 3 | 3 |
| 鏡像/Pending | 5 | 5 |
| 降級管理 | 5 | 5 |
| 重建管理 | 3 | 3 |
| Sysfs 介面 | 7 | 7 |
| **總計** | **83** | **83** |
