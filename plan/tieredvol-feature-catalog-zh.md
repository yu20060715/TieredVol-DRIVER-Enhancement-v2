# TieredVol v4.6.0 功能目錄表

TieredVol 裝置映射目標（device-mapper target）所有已實現功能的技術參考文件。

---

## 1. I/O 分派

I/O 分派層負責將邏輯位元組偏移量轉換為實體碟分配。TieredVol 支援三種分派策略，每種策略在對應的 segment 中選碟並計算實體 sector 偏移量。

### #1 靜態加權分派（Static Weighted Dispatch）

> 根據預計算的加權條帶邊界，確定性地將邏輯偏移量映射到碟。

**實現方法：** `tv_map_logical()`，位於 `driver/tieredvol_map.c:4-65`。根據 segment 權重建構前綴和邊界陣列，線性掃描定位目標碟。實體偏移量計算公式：`stripe_no * weight[disk] * CHUNK_SIZE + (offset_in - boundary[disk])`。

**核心 API：** `bio_set_dev()`, `bdev_nr_sectors()`

**參考文獻：**
1. `drivers/md/dm-stripe.c` — 經典條帶化目標的加權分配
2. `drivers/md/dm-switch.c` — 基於 region 的 bio 分派與 sector 重映射

---

### #2 自適應 EMA 分派（Adaptive EMA Dispatch）

> 使用指數移動平均（EMA）載入分數加寫入磨損懲罰，為每個 bio 選擇最空閒的碟，自動跳過 stale 碟。

**實現方法：** `tv_map_logical_adaptive()`，位於 `driver/tieredvol_map.c:67-156`。遍歷候選碟，計算 `load = ema_load[d] + wear_bias * total_write_bytes[d] / total_writes`，選最小值。若所有候選碟均為 stale，則回退到任意有效碟。

**核心 API：** `get_random_u32()`, 原子 EMA 更新（`tv_decay_timer_fn()`）

**參考文獻：**
1. `block/kyber-iosched.c` — 基於 token 的動態深度調整
2. `block/mq-deadline.c` — 請求排序與基於時間的過期機制

---

### #3 隨機分派（Random Dispatch）

> 在 segment 內均勻隨機選碟。

**實現方法：** `tv_map_logical_random()`，位於 `driver/tieredvol_map.c:158-201`。使用 `get_random_u32() % seg->disk_count` 進行均勻選取。

**核心 API：** `get_random_u32()`

**參考文獻：**
1. `drivers/md/dm-switch.c` — 隨機路徑選擇回退

---

### #4 Bio Sector 重映射（Bio Sector Remapping）

> 透過重寫 `bi_bdev` 和 `bi_iter.bi_sector`，將 bio 從 DM 虛擬裝置重定向到正確的實體碟。

**實現方法：** `tieredvol_map()`，位於 `driver/tieredvol_core.c:156-238`。呼叫 `bio_set_dev(bio, ctx->devs[cur.disk]->bdev)` 並設定 `bio->bi_iter.bi_sector = cur.offset >> SECTOR_SHIFT`。

**核心 API：** `bio_set_dev()`, `SECTOR_SHIFT`

**參考文獻：**
1. `drivers/md/dm-crypt.c` — Bio clone + 重定向到加密層
2. `drivers/md/dm-linear.c` — 最簡單的 bio 重映射目標

---

### #5 無效碟錯誤（Invalid Disk Error）

> 當計算出的碟索引超出範圍時，返回 bio 錯誤。

**實現方法：** `tieredvol_map()`，位於 `driver/tieredvol_core.c:182-189`。檢查 `cur.disk < 0 || cur.disk >= ctx->ndisks`，呼叫 `bio_io_error(bio)` 並返回 `DM_MAPIO_SUBMITTED`。

**核心 API：** `bio_io_error()`

**參考文獻：**
1. `drivers/md/dm.c` — DM 核心錯誤處理模式

---

### #6 寫入鏡像（Write Mirroring）

> Clone 寫入 bio 並提交到指定的鏡像碟，實現資料冗餘。

**實現方法：** `tieredvol_map()`，位於 `driver/tieredvol_core.c:206-234`。當 `seg->mirror_enabled` 且鏡像碟與主碟不同時，呼叫 `bio_alloc_clone()`（使用 `&fs_bio_set`），設定自定義 `tv_mirror_end_io` 完成處理器，透過 `submit_bio()` 提交。

**核心 API：** `bio_alloc_clone()`, `submit_bio()`, `bio_put()`

**參考文獻：**
1. `drivers/md/dm-raid1.c` — Bio clone + 雙碟寫入 + 自定 bi_end_io

---

## 2. 負載均衡 + 時間衰減

負載均衡使用 in-flight 位元組計數器和 EMA 平滑濾波器追蹤每碟 I/O 壓力。硬體計時器每秒衰減這些計數器，為自適應分派策略提供平滑的載入估計。

### #7 EMA 載入計算（EMA Load Calculation）

> 使用指數移動平均計算每碟載入：`ema = ema * (1 - alpha) + snapshot * alpha`，alpha 透過 `ema_weight_shift` 可調。

**實現方法：** `tv_decay_timer_fn()`，位於 `driver/tieredvol_core.c:92-142`。Alpha 預設為 `1 << 3 = 8`（滿值 1024，shift=3）。快照為原子 in-flight 位元組計數器，每 tick 透過 `atomic_xchg()` 歸零。

**核心 API：** `timer_list`, `atomic_xchg()`

**參考文獻：**
1. `kernel/sched/fair.c` — CFS per-CPU 載入追蹤
2. `drivers/md/dm-thin.c` — Pool 模式切換與低水位回呼

---

### #8 In-flight 位元組追蹤（In-flight Byte Tracking）

> 使用每碟原子計數器計算目前傳輸中的位元組數。

**實現方法：** `tieredvol_map()`，位於 `driver/tieredvol_core.c:193`。`atomic_add(bio->bi_iter.bi_size, &ctx->in_flight_bytes[cur.disk])`。衰減計時器每秒透過 `atomic_xchg()` 歸零。

**核心 API：** `atomic_add()`, `atomic_xchg()`

**參考文獻：**
1. `block/blk-mq.c` — blk-mq per-tag 統計

---

### #9 1 秒衰減計時器（1-second Decay Timer）

> 每 HZ 個 tick 觸發硬體計時器，衰減載入計數器並檢查 stale 偵測。

**實現方法：** `timer_setup(&ctx->decay_timer, tv_decay_timer_fn, 0)`，位於 `driver/tieredvol_core.c:317-318`。計時器在每個 tick 結束時透過 `mod_timer()` 重新武裝。

**核心 API：** `timer_setup()`, `mod_timer()`, `timer_delete_sync()`

**參考文獻：**
1. `kernel/time/timer_list.c` — timer_list API 參考

---

### #10 磨損偏差懲罰（Wear-bias Penalty）

> 將寫入放大懲罰加到載入分數：`load += wear_bias * total_write_bytes[d] / total_writes`。

**實現方法：** `tv_map_logical_adaptive()`，位於 `driver/tieredvol_map.c:106-122`。當 `wear_bias > 0` 時，每碟載入按其在總寫入中的佔比加權懲罰，抑制對高磨損碟的進一步寫入。

**核心 API：** 無（純算術運算）

**參考文獻：**
1. `block/mq-deadline.c` — 寫入懲罰啟發式

---

## 3. Stale 碟偵測 + 恢復

Stale 偵測識別已停止回應 I/O 的碟。碟在可配置超時後被標記為 stale，然後在 I/O 恢復時或冷卻期後自動恢復。恢復保護期（grace period）防止恢復後立即被重新標記為 stale。

### #11 Stale 標記（Stale Marking）

> 當 `stale_after_ns`（預設 5 秒）內無 I/O 完成時，將碟標記為 stale。

**實現方法：** `tv_decay_timer_fn()`，位於 `driver/tieredvol_core.c:112-123`。檢查 `now > ctx->grace_until_ns[i]` 且 `(now - ctx->last_finish_ns[i]) > ctx->stale_after_ns`。設定 `ctx->stale[i] = true` 並透過 `tv_log(TV_LOG_WARN, ...)` 記錄。

**核心 API：** `ktime_get_boottime_ns()`

**參考文獻：**
1. `drivers/md/dm-dust.c` — 壞軌模擬與啟用/停用
2. `drivers/md/dm-raid1.c` — Mirror 故障轉移與錯誤偵測

---

### #12 Stale 恢復 — I/O 觸發（Stale Recovery, I/O Triggered）

> 當 stale 碟收到新 I/O 完成時立即恢復，並啟動新保護期。

**實現方法：** `tv_decay_timer_fn()`，位於 `driver/tieredvol_core.c:124-129`。當 `ctx->stale[i] && snapshot > 0`，設定 `stale[i] = false` 且 `grace_until_ns[i] = now + stale_after_ns`。

**核心 API：** 無（狀態機）

**參考文獻：**
1. `drivers/md/dm-log.c` — Dirty region 追蹤與恢復

---

### #13 Stale 恢復 — 冷卻（Stale Recovery, Cooldown）

> 在 2 倍 stale 超時後自動恢復 stale 碟，即使沒有新 I/O。

**實現方法：** `tv_decay_timer_fn()`，位於 `driver/tieredvol_core.c:130-138`。當 `(now - ctx->stale_marked_ns[i]) > 2 * ctx->stale_after_ns`，設定 `stale[i] = false`。

**核心 API：** 無（狀態機）

**參考文獻：**
1. `block/disk-events.c` — 磁碟事件輪詢與超時

---

### #14 保護期（Grace Period）

> 保護新恢復的碟不被立即重新標記為 stale。

**實現方法：** `tv_decay_timer_fn()`，位於 `driver/tieredvol_core.c:126,134`。恢復時設定 `ctx->grace_until_ns[i] = now + ctx->stale_after_ns`。第 115 行的 stale 檢查驗證 `now > ctx->grace_until_ns[i]` 後才標記 stale。

**核心 API：** 無（狀態機）

**參考文獻：**
1. `drivers/md/dm-raid1.c` — Mirror 恢復保護期

---

## 4. Per-disk I/O 統計

每個實體碟的累積計數器，在裝置映射目標的整個生命週期中追蹤。

### #15 讀取位元組計數器（Read Bytes Counter）

> 每碟總讀取位元組數。

**實現方法：** `ctx->total_read_bytes[cur.disk] += bio->bi_iter.bi_size`，位於 `driver/tieredvol_core.c:198`。

**參考文獻：** `drivers/md/dm.c` — DM 核心 bio 統計

---

### #16 讀取操作計數器（Read Ops Counter）

> 每碟總讀取操作數。

**實現方法：** `ctx->total_read_ops[cur.disk]++`，位於 `driver/tieredvol_core.c:199`。

**參考文獻：** `block/blk-mq.c` — blk-mq per-tag 統計

---

### #17 寫入位元組計數器（Write Bytes Counter）

> 每碟總寫入位元組數。

**實現方法：** `ctx->total_write_bytes[cur.disk] += bio->bi_iter.bi_size`，位於 `driver/tieredvol_core.c:195`。

**參考文獻：** `drivers/nvme/host/core.c` — NVMe per-controller 統計

---

### #18 寫入操作計數器（Write Ops Counter）

> 每碟總寫入操作數。

**實現方法：** `ctx->total_write_ops[cur.disk]++`，位於 `driver/tieredvol_core.c:196`。

**參考文獻：** `block/genhd.c` — `part_stat_show` per-partition 統計

---

### #19 錯誤計數器（Error Counter）

> 每碟原子錯誤計數，在 bio 完成錯誤時遞增。

**實現方法：** `ctx->error_count[disk]` 是 `atomic_t` 陣列，在 `tieredvol_ctr()` 中分配（`driver/tieredvol_core.c:299`）。透過 `atomic_read()` 在 `tieredvol_status()` 中讀取。

**核心 API：** `atomic_read()`, `atomic_set()`

**參考文獻：**
1. `drivers/md/dm.c` — DM 錯誤計數
2. `drivers/md/dm-raid1.c` — Mirror 錯誤追蹤

---

## 5. Per-CPU 全域統計

使用 `DEFINE_PER_CPU` 的無鎖全域計數器，實現高吞吐量 I/O 追蹤且無快取行競爭。

### #20 Per-CPU 映射計數（Per-CPU Map Count）

> 計算所有 CPU 的 bio 映射操作總數。

**實現方法：** `DEFINE_PER_CPU(u64, tv_map_count)`，位於 `driver/tieredvol_core.c:25`。透過 `this_cpu_inc(tv_map_count)` 遞增（第 201 行）。

**核心 API：** `this_cpu_inc()`, `DEFINE_PER_CPU()`

**參考文獻：**
1. `include/linux/percpu_counter.h` — percpu_counter API

---

### #21 Per-CPU Sector 計數（Per-CPU Sector Count）

> 計算所有 CPU 分派的 sector 總數。

**實現方法：** `DEFINE_PER_CPU(u64, tv_map_sectors)`，位於 `driver/tieredvol_core.c:26`。透過 `this_cpu_add(tv_map_sectors, bio_sectors(bio))` 累加（第 202 行）。

**核心 API：** `this_cpu_add()`, `bio_sectors()`

**參考文獻：**
1. `kernel/sched/fair.c` — CFS per-CPU 載入追蹤

---

### #22 Per-CPU 位元組計數（Per-CPU Byte Count）

> 計算所有 CPU 分派的位元組總數。

**實現方法：** `DEFINE_PER_CPU(u64, tv_map_bytes)`，位於 `driver/tieredvol_core.c:27`。透過 `this_cpu_add(tv_map_bytes, bio->bi_iter.bi_size)` 累加（第 203 行）。

**核心 API：** `this_cpu_add()`

**參考文獻：**
1. `include/linux/mm_types.h` — vm_stat per-CPU 計數器

---

### #23 跨 CPU 聚合（Cross-CPU Aggregation）

> 跨所有可能的 CPU 聚合 per-CPU 計數器以取得全域總計。

**實現方法：** `tv_read_count()`, `tv_read_sectors()`, `tv_read_bytes()`，位於 `driver/tieredvol_core.c:63-88`。每個函數迭代 `for_each_possible_cpu(cpu)` 並累加 `per_cpu(counter, cpu)`。

**核心 API：** `for_each_possible_cpu()`, `per_cpu()`

**參考文獻：**
1. `drivers/md/dm-thin.c` — Pool per-CPU deferred set

---

## 6. DM Message 指令

透過 `dmsetup message` 的執行時控制介面。所有指令在 `tieredvol_message()` 中分派（`driver/tieredvol_core.c:520-808`）。

### #24 `reset_stats`

> 歸零所有 per-CPU 統計（映射計數、sector 計數、位元組計數）。

**實現方法：** `driver/tieredvol_core.c:523-532`。迭代 `for_each_possible_cpu(cpu)` 並將每個計數器設為 0。

---

### #25 `show_stats`

> 透過 dmesg 返回映射次數、平均每次映射位元組數和總位元組數。

**實現方法：** `driver/tieredvol_core.c:533-542`。計算 `avg = total_bytes / maps_count`。

---

### #26 `status`

> 返回每碟名稱及其在 segment 中的權重。

**實現方法：** `driver/tieredvol_core.c:543-568`。掃描所有 segment 尋找每碟的權重。

---

### #27 `show_inflight`

> 返回每碟目前 in-flight 位元組數。

**實現方法：** `driver/tieredvol_core.c:569-581`。讀取 `atomic_read(&ctx->in_flight_bytes[i])`。

---

### #28 `adaptive_on`

> 將分派策略切換為自適應（基於 EMA 的負載均衡）。

**實現方法：** `driver/tieredvol_core.c:582-589`。設定 `ctx->policy = TV_POLICY_ADAPTIVE`。

---

### #29 `adaptive_off`

> 將分派策略切換為靜態（基於加權邊界）。

**實現方法：** `driver/tieredvol_core.c:590-597`。設定 `ctx->policy = TV_POLICY_STATIC`。

---

### #30 `set_policy <name>`

> 設定分派策略為 static、adaptive 或 random。

**實現方法：** `driver/tieredvol_core.c:598-612`。驗證 `argv[1]` 是否為 "static"、"adaptive"、"random"。

---

### #31 `set_ema_shift <shift>`

> 設定 EMA 權重位移（0-10）。Alpha = `1 << shift`（滿值 1024）。

**實現方法：** `driver/tieredvol_core.c:613-624`。透過 `kstrtou32()` 驗證 shift <= 10。**已修復 Bug：** 原本檢查 `argc == 1` 但讀取 `argv[1]`，導致 kernel oops。已修正為 `argc == 2`。

**核心 API：** `kstrtou32()`

---

### #32 `set_stale_ms <ms>`

> 設定 stale 偵測超時（毫秒）。

**實現方法：** `driver/tieredvol_core.c:625-635`。轉換為 ns：`ctx->stale_after_ns = (u64)ms * 1000000ULL`。

---

### #33 `show_adaptive`

> 返回策略、EMA 位移、stale 超時、磨損偏差，以及每碟載入/磨損/stale 狀態。

**實現方法：** `driver/tieredvol_core.c:636-656`。輸出全面的狀態字串。

---

### #34 `show_wear`

> 返回磨損偏差和每碟總寫入位元組數。

**實現方法：** `driver/tieredvol_core.c:657-671`。

---

### #35 `show_io_stats`

> 返回每碟讀取/寫入操作數和位元組數。

**實現方法：** `driver/tieredvol_core.c:672-688`。

---

### #36 `reset_io_stats`

> 歸零所有 per-disk I/O 統計（讀取/寫入位元組數/操作數）。

**實現方法：** `driver/tieredvol_core.c:689-701`。

---

### #37 `set_wear_bias <bias>`

> 設定磨損懲罰因子（0-1024）。值越高，自適應分派對高磨損碟的懲罰越重。

**實現方法：** `driver/tieredvol_core.c:702-712`。驗證 `bias <= 1024`。

---

### #38 `reset_wear`

> 歸零每碟寫入位元組數（磨損計數器）。

**實現方法：** `driver/tieredvol_core.c:713-722`。

---

### #39 `show_mirror`

> 返回鏡像寫入操作數/位元組數、錯誤計數，以及每 segment 鏡像配置。

**實現方法：** `driver/tieredvol_core.c:723-746`。

---

### #40 `set_mirror <seg> <disk>`

> 為 segment 啟用鏡像，指定目標碟。

**實現方法：** `driver/tieredvol_core.c:747-763`。驗證 `seg_idx < segment_count` 且 `disk_idx < ndisks`。設定 `seg->mirror_enabled = true` 且 `seg->mirror_disk = disk_idx`。

---

## 7. DM Target 生命週期

管理 tieredvol 目標生命週期的標準裝置映射目標回呼函數。

### #41 構造函數（Constructor, ctr）

> 分配上下文、從配置檔案載入元資料、取得 DM 裝置，並啟動衰減計時器。

**實現方法：** `tieredvol_ctr()`，位於 `driver/tieredvol_core.c:246-408`。流程：
1. 解析參數（期望 1 個參數：配置檔案路徑）
2. `kzalloc(sizeof(*ctx))` — 分配上下文
3. `tv_metadata_load_kernel()` — 透過 `filp_open()` + `kernel_read()` 解析 key=value 配置檔案
4. `kcalloc(ndisks, sizeof(*ctx->devs))` — 分配裝置陣列
5. `dm_get_device()` — 取得每個 DM 裝置
6. 計算所有 segment 的 `min_chunk_sectors`
7. `dm_set_target_max_io_len()` — 設定最大 I/O 大小
8. `timer_setup()` + `mod_timer()` — 啟動衰減計時器

**核心 API：** `kzalloc()`, `kcalloc()`, `dm_get_device()`, `dm_set_target_max_io_len()`, `timer_setup()`

---

### #42 析構函數（Destructor, dtr）

> 停止衰減計時器、刷新待處理工作、釋放 DM 裝置，並釋放所有記憶體。

**實現方法：** `tieredvol_dtr()`，位於 `driver/tieredvol_core.c:410-425`。流程：
1. `timer_delete_sync()` — 停止衰減計時器
2. `flush_work()` — 完成待處理的 trigger_event 工作
3. `dm_put_device()` — 釋放每個 DM 裝置
4. `kfree()` — 釋放所有分配

**核心 API：** `timer_delete_sync()`, `flush_work()`, `dm_put_device()`, `kfree()`

---

### #43 I/O 提示（IO Hints）

> 向 DM 框架報告區區大小、區塊大小和最佳 I/O 大小。

**實現方法：** `tieredvol_io_hints()`，位於 `driver/tieredvol_core.c:438-448`。設定 `logical_block_size = 512`, `physical_block_size = 512`, `chunk_sectors = min_chunk_sectors`, `io_opt = stripe_sectors`。

**核心 API：** `struct queue_limits`

---

### #44 迭代裝置（Iterate Devices）

> 返回所有底層 DM 裝置，用於狀態報告和 ioctl 透傳。

**實現方法：** `tieredvol_iterate_devices()`，位於 `driver/tieredvol_core.c:450-463`。迭代 `ctx->devs[0..ndisks-1]` 並對每個裝置呼叫回呼函數。

**核心 API：** `iterate_devices_callout_fn`, `bdev_nr_sectors()`

---

### #45 準備 Ioctl（Prepare Ioctl）

> 返回第一個底層裝置的區塊裝置，用於 ioctl 透傳。

**實現方法：** `tieredvol_prepare_ioctl()`，位於 `driver/tieredvol_core.c:427-436`。設定 `*bdev = ctx->devs[0]->bdev`。

**核心 API：** `struct block_device`

---

### #46 Flush/Discard 傳播（Flush/Discard Propagation）

> 將 flush 和 discard 命令傳播到所有底層碟。

**實現方法：** `tieredvol_ctr()`，位於 `driver/tieredvol_core.c:390-392`。設定 `ti->num_flush_bios = ctx->ndisks` 和 `ti->num_discard_bios = ctx->ndisks`。同時設定 `ti->flush_bypasses_map = true`。

---

### #47 模組初始化/退出（Module Init/Exit）

> 向內核註冊 DM 目標並建立工作佇列。

**實現方法：** `tieredvol_init()`，位於 `driver/tieredvol_core.c:828-847`。呼叫 `dm_register_target()` 和 `alloc_workqueue("tieredvol_wq", WQ_UNBOUND | WQ_HIGHPRI, 0)`。退出時呼叫 `dm_unregister_target()` 和 `destroy_workqueue()`。

**核心 API：** `dm_register_target()`, `dm_unregister_target()`, `alloc_workqueue()`

---

## 8. 狀態報告

透過 `dmsetup status` 的狀態輸出，提供目標執行時狀態的可見性。

### #48 STATUSTYPE_INFO

> 返回策略、鏡像統計、錯誤計數，以及每碟 active/degraded 狀態和讀取/寫入計數器。

**實現方法：** `tieredvol_status()`，位於 `driver/tieredvol_core.c:465-518`（STATUSTYPE_INFO case）。格式：`policy=N mirror=ops/bytes err=N Ddisk:rd=ops/bytes wr=ops/bytes`。

---

### #49 STATUSTYPE_TABLE

> 返回底層碟裝置名稱列表。

**實現方法：** `driver/tieredvol_core.c:500-513`（STATUSTYPE_TABLE case）。空格分隔的碟名。

---

### #50 STATUSTYPE_IMA

> IMA（完整性測量架構）佔位符 — 返回空字串。

**實現方法：** `driver/tieredvol_core.c:514-516`。設定 `result[0] = '\0'`。

---

## 9. 元資料解析

從 key=value 文字檔案載入 tieredvol 拓撲的內核態配置檔案解析器。

### #51 內核態檔案配置（Kernel File-based Config）

> 使用 `filp_open()` + `kernel_read()` 從內核態讀取配置檔案。

**實現方法：** `tv_metadata_load_kernel()`，位於 `driver/tieredvol_meta.c:93-272`。以 `filp_open(path, O_RDONLY, 0)` 開啟檔案，透過 `kernel_read()` 讀取最多 1MB，然後逐行解析。

**核心 API：** `filp_open()`, `kernel_read()`, `i_size_read()`, `vmalloc()`, `vfree()`

**參考文獻：**
1. `drivers/md/dm-thin-metadata.c` — 複雜的元資料管理（B-tree + superblock）
2. `drivers/md/dm-log.c` — 磁碟日誌狀態持久化

---

### #52 Version/Chunk/Segment/Disk 解析

> 解析並驗證 version、chunk_size、segment_count、disk_count 和碟名稱。

**實現方法：** `driver/tieredvol_meta.c:150-187`。使用 `parse_u32()` / `parse_u64()` 輔助函數。驗證 `disk_count <= TV_MAX_DISKS` 和 `segment_count <= TV_MAX_SEGS`。

**核心 API：** `kstrtoul()`, `kstrtoull()`

---

### #53 Segment Disks/Weight CSV 解析

> 解析逗號分隔的 u32 陣列，用於 segment 碟索引和權重。

**實現方法：** `parse_csv_u32()`，位於 `driver/tieredvol_meta.c:54-70`。使用 `strsep(&s, ",")` 分詞。同時透過 `parse_num_prefix()` 解析 `seg_begin`、`seg_end`、`seg_stripe`、`seg_mirror`。

**核心 API：** `strsep()`, `parse_num_prefix()`

**參考文獻：**
1. `fs/configfs/configfs.c` — 內核態-使用者態配置介面
2. `drivers/md/dm-table.c` — DM 表解析

---

## 10. 熱插拔 + 動態偵測（未來目標）

> **狀態：已規劃，尚未實現。** 這些功能被識別為核心目標，但需要大量工程投入。建議方案使用 DM 表重新載入（dmsetup suspend/resume）實現安全重配置。

### #54 線上新增（Online Add）

> 新增碟時動態擴展 segment。

**規劃方案：** `dmsetup suspend` → 重建元資料 → `dmsetup resume`。參考：`mdadm/Grow_Add_device()`。

**參考文獻：**
1. `mdadm/Grow.c` — 線性陣列熱新增（`Grow_Add_device`）
2. `drivers/md/dm.c` — DM 裝置動態建立（`dev_create`）

---

### #55 線上移除（Online Remove）

> 移除碟前遷移條帶資料。

**規劃方案：** 內核執行緒將資料從源碟複製到存活碟，然後 DM 表重新載入。參考：`mdadm --remove`。

**參考文獻：**
1. `mdadm/Grow.c` — 含資料遷移的線上移除

---

### #56 uevent 監聽器（uevent Listener）

> 透過 netlink uevent 偵測碟的新增/移除事件。

**規劃方案：** 註冊 netlink 監聽器以接收 `block` 子系統事件。

**參考文獻：**
1. `lib/kobject_uevent.c` — uevent netlink 廣播

---

### #57 sysfs 監控（sysfs Monitor）

> 透過 sysfs 輪詢偵測碟速度退化。

**規劃方案：** 輪詢 `/sys/block/<dev>/stat` 以偵測效能變化。

**參考文獻：**
1. `block/disk-events.c` — 磁碟事件輪詢框架

---

## 11. 結構化診斷日誌

內核態環形緩衝區，記錄 I/O、stale、鏡像和配置事件，附帶時間戳和嚴重等級。事件透過 `dmsetup message` 查詢。

### #58 環形緩衝區（Ring Buffer）

> 固定大小的 `kfifo` 環形緩衝區（512 條目），帶自旋鎖保護，記錄帶時間戳的日誌條目。

**實現方法：** `driver/tieredvol_core.c:29-54`。`DECLARE_KFIFO(tv_log_fifo, struct tv_log_entry, TV_LOG_SIZE)` 搭配 `DEFINE_SPINLOCK(tv_log_lock)`。每次 `tv_log()` 呼叫透過 `spin_lock_irqsave()` 取得自旋鎖，寫入 `struct tv_log_entry`（64 位元組：timestamp_ns, level, disk_idx, event_type, msg[48]），然後釋放。溢出時覆寫最舊條目。

**資料結構：** `struct tv_log_entry`，位於 `driver/tieredvol.h:98-104`。

**核心 API：** `DECLARE_KFIFO()`, `kfifo_put()`, `kfifo_get()`, `kfifo_reset()`, `spin_lock_irqsave()`

**參考文獻：**
1. emlog (nicupavel) — 帶溢出覆寫的環形緩衝區架構
   https://github.com/nicupavel/emlog
2. `kernel/samples/kfifo/record-example.c` — kfifo 使用模式
3. `kernel/trace/ring_buffer.c` — 高效能無鎖環形緩衝區

---

### #59 日誌等級（Log Level）

> 動態詳細程度控制：OFF(0) / ERROR(1) / WARN(2) / INFO(3)。

**實現方法：** `driver/tieredvol_core.c:799-807`。`set_loglevel <0-3>` 設定全域 `tv_log_level`。`tv_log()` 函數在第 39 行檢查 `if (level > tv_log_level) return;`。

**核心 API：** `kstrtou32()`

**參考文獻：**
1. `kernel/trace/trace.c` — 帶日誌等級控制的追蹤事件管理

---

### #60 DM 問詢（show_log / clear_log）

> 透過 `show_log`（將條目傾印到 dmesg）和 `clear_log`（重置環形緩衝區）進行即時日誌查詢。

**實現方法：** `driver/tieredvol_core.c:764-798`。
- `show_log`：在自旋鎖下排空 kfifo，以格式 `LOG {ERR|WRN|INF} {I/O|STALE|RCVR|MIRR|CONF}: <msg>` 將每條目列印到 dmesg。
- `clear_log`：在自旋鎖下呼叫 `kfifo_reset()`。

**核心 API：** `kfifo_get()`, `kfifo_reset()`, `DMINFO()`

**參考文獻：**
1. dm-dust — DM message 查詢/結果模式
2. dm-log-writes — 結構化 I/O 事件日誌

---

## 附錄：參考文獻

### 學術論文

| 論文 | 發表場合 | 相關性 |
|------|----------|--------|
| Jiao & Kim, "Asymmetric RAID: Rethinking RAID for SSD Heterogeneity" | HotStorage'24 | 原始 Asym-RAID 設計；TieredVol 在此基础上擴展動態偵測 |

### Linux 內核源碼

| 檔案 | 連結 | 引用功能 |
|------|------|----------|
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

### 開源專案

| 專案 | 連結 | 引用功能 |
|------|------|----------|
| emlog (nicupavel) | https://github.com/nicupavel/emlog | #58 |
| sysprog21/kfifo-examples | https://github.com/sysprog21/kfifo-examples | #58 |
| mdadm | https://github.com/md-raid-utilities/mdadm | #54, #55 |

---

## 測試覆蓋率摘要

| 類別 | 功能數 | 已測試 |
|------|:------:|:------:|
| I/O 分派 | 6 | 6 |
| 負載均衡 | 4 | 4 |
| Stale 偵測 | 4 | 4 |
| Per-disk 統計 | 5 | 5 |
| Per-CPU 統計 | 4 | 4 |
| DM 指令 | 17 | 17 |
| DM 生命週期 | 7 | 7 |
| 狀態報告 | 3 | 3 |
| 元資料 | 3 | 3 |
| 結構化日誌 | 3 | 3 |
| **總計** | **56** | **56** |

> 註：4 個熱插拔功能（#54-57）已規劃但尚未實現。
