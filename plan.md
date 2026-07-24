# TieredVol Enhancement Roadmap

> 目標：把 TieredVol 缺的東西通通加進去，野心大一點。
> 所有功能標注來源，能借鑒就借鑒。

---

## 一、第一層：簡單增強（複製貼上，1天一個）

### 1.1 TRIM/Discard 支援
- **來源**：`dm-stripe.c` (lines 155-170)
- **做法**：加 `ti->num_discard_bios = ndisks` + `stripe_map_range()` 邏輯
- **效果**：允許 `fstrim` 命令，SSD 可以回收空間
- **難度**：低（複製貼上）

### 1.2 Secure Erase 支援
- **來源**：`dm-stripe.c` (同上)
- **做法**：加 `ti->num_secure_erase_bios = ndisks`
- **效果**：安全擦除支援
- **難度**：低

### 1.3 Write Zeroes 支援
- **來源**：`dm-stripe.c` (同上)
- **做法**：加 `ti->num_write_zeroes_bios = ndisks`
- **效果**：高效零填充，比寫入快
- **難度**：低

### 1.4 錯誤處理 (end_io)
- **來源**：`dm-stripe.c` (lines 268-300)
- **做法**：加 `end_io` callback + `atomic_t error_count` per disk + `trigger_event` workqueue
- **效果**：偵測磁碟錯誤，觸發 DM table event
- **難度**：低

### 1.5 磁碟狀態報告 'A'/'D'
- **來源**：`dm-stripe.c` (lines 225-245)
- **做法**：status 回報每顆碟的 active/dead 狀態
- **效果**：`dmsetup status` 可以看到每顆碟的健康狀態
- **難度**：低

### 1.6 prepare_ioctl
- **來源**：`dm-linear.c` (lines 113-122)
- **做法**：加 `prepare_ioctl` callback，當大小完全匹配時直接 passthrough ioctl
- **效果**：允許 ioctl 直接穿透到實體裝置
- **難度**：低

### 1.7 DM Target Features
- **來源**：`dm-linear.c` (line 95-97)
- **做法**：加 `DM_TARGET_PASSES_INTEGRITY | DM_TARGET_ATOMIC_WRITES | DM_TARGET_PASSES_CRYPTO`
- **效果**：支援完整性校驗、原子寫入、加密穿透
- **難度**：低

---

## 二、第二層：中等難度（借 pattern，3-5天一個）

### 2.1 動態權重調整（核心功能） ✅ DONE
- **來源**：`dm-ps-service-time.c` (lines 136-211)
- **做法**：借 `st_compare_load()` 公式
- **核心公式**：`service_time = (in_flight + incoming) / relative_throughput`
- **優化**：用交叉相乘避免除法：`sz1 * pi2->relative_throughput < sz2 * pi1->relative_throughput`
- **實際做法**：`tv_map_logical_adaptive()` — 每個 segment 內選 EMA load 最低的 disk，全部 stale 時 fallback 到 static weights
- **難度**：中

### 2.2 Per-disk In-flight Tracking ✅ DONE
- **來源**：`dm-ps-service-time.c` (lines 197-215)
- **做法**：DM strips bi_opf/bi_bdev between map() and end_io()，hash table 和 bi_opf encoding 都行不通
- **實際做法**：map()-only `atomic_add(in_flight_bytes)`，timer callback 用 `atomic_xchg` snapshot 每秒
- **效果**：近似 per-interval bytes sent per disk，用於 EMA 載入量
- **難度**：低-中

### 2.3 Timer 監控 ✅ DONE
- **來源**：`dm-delay.c` (lines 29-58, 267-282)
- **做法**：`timer_setup()` + 1 秒 interval (`TV_DECAY_INTERVAL = HZ`)
- **teardown**：`timer_delete_sync()` in dtr
- **效果**：每秒 snapshot in_flight → EMA → staleness check
- **難度**：中

### 2.4 sysfs 即時控制 ✅ DONE (via dmsetup message)
- **來源**：`dm-delay.c` presuspend/resume pattern
- **做法**：用 `dmsetup message` 接口而非 sysfs（更實際）
- **Commands**: `show_inflight`, `adaptive_on`, `adaptive_off`, `show_adaptive`, `set_ema_shift <N>`, `set_stale_ms <N>`
- **效果**：可即時開關動態調整、查看統計
- **難度**：中

### 2.5 EMA 延遲追蹤 ✅ DONE
- **來源**：`dm-ps-historical-service-time.c` (lines 105-118)
- **做法**：`fixed_ema()` 追蹤歷史 service time
- **公式**：`ema = ema * (1 - alpha) + snapshot * alpha`，alpha = 1 << ema_weight_shift / 1024
- **效果**：平滑載入數據，避免抖動
- **難度**：中

### 2.6 Staleness 偵測 ✅ DONE
- **來源**：`dm-ps-historical-service-time.c` (lines 310-323)
- **做法**：`stale_after_ns` + `last_finish_ns` 時間戳 + cooldown recovery
- **邏輯**：N 秒無 I/O → stale → adaptive 跳過 → cooldown 2x 後自動恢復
- **Grace period**：恢復後 grace_until = now + stale_after 避免 oscillation
- **難度**：中

---

## 三、第三層：野心大的（需要寫新 code，1-2週）

### 3.1 SSD 磨損均衡 ✅ DONE
- **來源**：bcache wear leveling pattern
- **做法**：追蹤每顆碟的總寫入量，adaptive 時加入 wear penalty
- **實際做法**：`total_write_bytes[]` per disk, `wear_bias` tunable, adaptive load += wear_bias * disk_writes / total_writes
- **效果**：延長 SSD 壽命，較少寫入的碟會被優先選擇
- **難度**：高

### 3.2 寫入分佈策略 ✅ DONE
- **來源**：dm-switch.c / dm-default-key.c policy pattern
- **做法**：三種策略 — static (weighted stripe), adaptive (lowest EMA+wear), random
- **實際做法**：`enum tv_policy`, `set_policy static|adaptive|random`
- **效果**：可依需求切換分佈策略
- **難度**：中

### 3.3 Per-disk IO Stats ✅ DONE
- **來源**：dm-stats.c pattern
- **做法**：追蹤每顆碟的 read/write bytes + ops，`show_io_stats` 和 `reset_io_stats` message
- **效果**：詳細的 I/O 統計，可用於效能分析
- **難度**：低

### 3.4 Mirror/RAID1 模式
- **來源**：`dm-stripe.c` 錯誤處理 pattern + dm-crypt workqueue pattern
- **做法**：寫入時同時寫兩顆碟，讀時選快的那顆
- **效果**：資料冗餘，一顆碟壞了不丟資料
- **難度**：高（未實作）

### 3.5 線上重新平衡
- **來源**：dm-linear resize pattern
- **做法**：改權重時自動搬資料到新配置
- **需求**：metadata 支援搬運追蹤
- **效果**：不停機調整資料分佈
- **難度**：高（未實作）

### 3.6 寫入快取
- **來源**：dm-writecache.c pattern
- **做法**：RAM buffer 緩衝寫入，定期 flush 到碟
- **效果**：突發寫入效能大幅提升
- **難度**：高（未實作）

---

## 四、借鑒來源一覽

| 功能 | 來源檔案 | 行號 | 複製什麼 |
|------|---------|------|---------|
| Discard map | dm-stripe.c | 155-170 | `stripe_map_range()` + `REQ_OP_DISCARD` 處理 |
| 錯誤處理 | dm-stripe.c | 268-300 | `stripe_end_io()` + `atomic_t error_count` + `trigger_event` workqueue |
| Timer pattern | dm-delay.c | 29-58, 267-282 | `timer_setup()` + `INIT_WORK()` + `alloc_workqueue()` + teardown |
| In-flight tracking | dm-ps-service-time.c | 197-215 | `atomic_add(nr_bytes)` / `atomic_sub(nr_bytes)` |
| Service time formula | dm-ps-service-time.c | 136-211 | `st_compare_load()` — 交叉相乘避免除法 |
| EMA | dm-ps-historical-service-time.c | 105-118 | `fixed_ema()` |
| Staleness | dm-ps-historical-service-time.c | 310-323 | `stale_after` + `last_finish` |
| Wear leveling | bcache wear leveling | — | `total_write_bytes` + bias in adaptive load |
| Policy switch | dm-switch.c / dm-default-key.c | — | `enum tv_policy` + `set_policy` message |
| IO stats | dm-stats.c | — | per-disk read/write ops + bytes |

---

## 五、執行順序

1. ~~**第一層**：先做 1.1-1.7~~ ✅ DONE (v4.3.0)
2. ~~**第二層**：再做 2.1-2.6~~ ✅ DONE (v4.4.0)
3. ~~**第三層**：3.1-3.3 實用功能~~ ✅ DONE (v4.5.0)
4. 3.4-3.6 高難度功能（Mirror、Rebalance、Write Cache）— 需要 sub-bios/workqueue 架構

---

## 六、注意事項

- 所有功能必須 **向後相容**，不能破壞現有的 config 格式
- 動態權重調整要能 **開關**（sysfs 或 module_param）
- 錯誤處理不能影響正常 I/O 效能（<1% overhead）
- 所有新功能都要有 **單元測試**
- 所有借鑒的 code 必須 **標註來源**

---

## 七、實作筆記

### Tier 2: in-flight tracking 限制
- DM 在 map() 和 end_io() 之間會 **stripped bi_opf bits** 和 **restore bi_bdev**，所以無法在 end_io 中識別 per-disk I/O
- 嘗試過：hash table (bio->bi_iter hash)、bi_opf high bits encoding、sub-bios — 全部行不通
- 最終方案：map()-only atomic counters + timer atomic_xchg snapshot
- 效果：近似值，足夠做 load-balancing 決策

### Staleness oscillation
- 初始問題：cooldown 恢復後 → 無 I/O → 3s 後又 stale → 不斷循環
- 修復：加 grace period (`grace_until_ns`) — 恢復後 grace = stale_after 內不再檢查
- 生產建議：stale_after_ms 設 10-30s，避免短暫 idle 造成的 oscillation

### v4.3.0 freeze bisect 結論
- 5 個 feature group 全部獨立測試通過
- freeze 原因：尚未確認，但 v4.3.0 穩定運行

### v4.5.0 throughput by policy
- **static**: 2449 MiB/s (baseline, weighted stripe)
- **random**: 1417 MiB/s (42% of static, uneven disk distribution)
- **adaptive**: 1037 MiB/s (42% of static, per-bio EMA/wear lookup overhead)
- Adaptive overhead 來源：非本地記憶體存取 (`ctx->ema_load[]`, `ctx->total_write_bytes[]`) + branch prediction miss
- 結論：動態策略犧牲 throughput 換取 load distribution 和 wear leveling
