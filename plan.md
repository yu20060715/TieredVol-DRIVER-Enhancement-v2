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

### 2.1 動態權重調整（核心功能）
- **來源**：`dm-ps-service-time.c` (lines 136-211)
- **做法**：借 `st_compare_load()` 公式
- **核心公式**：`service_time = (in_flight + incoming) / relative_throughput`
- **優化**：用交叉相乘避免除法：`sz1 * pi2->relative_throughput < sz2 * pi1->relative_throughput`
- **效果**：根據即時負載自動調整資料分佈
- **難度**：中

### 2.2 Per-disk In-flight Tracking
- **來源**：`dm-ps-service-time.c` (lines 197-215)
- **做法**：`atomic_add(nr_bytes)` 在 map() 時，`atomic_sub(nr_bytes)` 在 end_io() 時
- **效果**：追蹤每顆碟的即時負載
- **難度**：低-中

### 2.3 Timer 監控
- **來源**：`dm-delay.c` (lines 29-58, 267-282)
- **做法**：`timer_setup()` + `INIT_WORK()` + `alloc_workqueue()`
- **teardown**：`timer_shutdown_sync()` + `destroy_workqueue()`
- **效果**：定期檢查並調整權重
- **難度**：中

### 2.4 sysfs 即時控制
- **來源**：`dm-delay.c` presuspend/resume pattern
- **做法**：`module_param` + sysfs callback
- **效果**：可即時開關動態調整、查看統計
- **難度**：中

### 2.5 EMA 延遲追蹤
- **來源**：`dm-ps-historical-service-time.c` (lines 105-118)
- **做法**：`fixed_ema()` 追蹤歷史 service time
- **公式**：`ema = last * weight + next * (1 - weight)`
- **效果**：平滑延遲數據，避免抖動
- **難度**：中

### 2.6 Staleness 偵測
- **來源**：`dm-ps-historical-service-time.c` (lines 310-323)
- **做法**：`stale_after` + `last_finish` 時間戳
- **邏輯**：如果某碟 N 秒內沒完成任何 request → 標記 stale
- **效果**：偵測故障或效能嚴重下降的磁碟
- **難度**：中

---

## 三、第三層：野心大的（需要寫新 code，1-2週）

### 3.1 Mirror/RAID1 模式
- **來源**：`dm-stripe.c` 錯誤處理 pattern + dm-crypt workqueue pattern
- **做法**：寫入時同時寫兩顆碟，讀時選快的那顆
- **效果**：資料冗餘，一顆碟壞了不丟資料
- **難度**：高

### 3.2 線上重新平衡
- **來源**：dm-linear resize pattern
- **做法**：改權重時自動搬資料到新配置
- **需求**：metadata 支援搬運追蹤
- **效果**：不停機調整資料分佈
- **難度**：高

### 3.3 寫入快取
- **來源**：dm-writecache.c pattern
- **做法**：RAM buffer 緩衝寫入，定期 flush 到碟
- **效果**：突發寫入效能大幅提升
- **難度**：高

### 3.4 SSD 磨損均衡
- **來源**：bcache wear leveling pattern
- **做法**：追蹤每顆碟的總寫入量，均勻分配寫入
- **效果**：延長 SSD 壽命
- **難度**：高

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

---

## 五、執行順序

1. **第一層**：先做 1.1-1.7（簡單增強，1天搞定）
2. **第二層**：再做 2.1-2.6（動態權重是重點，1-2週）
3. **第三層**：看需求決定要不要做 3.1-3.4（野心大的，1-2月）

---

## 六、注意事項

- 所有功能必須 **向後相容**，不能破壞現有的 config 格式
- 動態權重調整要能 **開關**（sysfs 或 module_param）
- 錯誤處理不能影響正常 I/O 效能（<1% overhead）
- 所有新功能都要有 **單元測試**
- 所有借鑒的 code 必須 **標註來源**
