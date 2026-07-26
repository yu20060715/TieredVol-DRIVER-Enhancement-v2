# Asym-RAID vs TieredVol v4.6.0 功能比較分析

參考文獻：Z. Jiao and B. S. Kim, "Asymmetric RAID: Rethinking RAID for SSD Heterogeneity," HotStorage'24.

---

## 1. 異質感知資料分配（ILP）

**Asym-RAID 怎麼做：**
用 ILP（整數線性規劃）算出「每條 stripe 應該分給哪顆碟、佔多少容量」，目標是最大化邏輯容量。例如碟 A 有 256GB、碟 B 有 128GB，ILP 會算出最優的 stripe 分配比例。

**TieredVol 怎麼做：**
用 `weight_partition()` 在模組載入時，把整個容量按 weight 比例切成 segment。例如碟 A 權重 60、碟 B 權重 40，碟 A 得到 60% 容量、碟 B 得到 40% 容量。

**誰比較好：**
- ILP 優化的是**容量利用率**，能找到數學上的最優解
- weight partition 優化的是**效能**，權重高的碟分到更多 I/O
- 兩者其實**互補**：ILP 決定「怎麼分蛋糕」，weight 決定「誰吃多少」

**建議：**
可以結合兩者——用 ILP 決定最優容量分配，再用 weight 決定 I/O 分配。但會增加複雜度。如果預算有限，weight partition 已經夠用，因為場景（Edge AI、衛星等）主要痛點不是容量最大化，而是效能不均勻。

---

## 2. 雙維邏輯地址空間 + Stripe State Table

**Asym-RAID 怎麼做：**
在 SSD 的 LBA 與使用者看到的 LBA 之間，插入一層「內部邏輯區塊層」。用 Stripe State Table（SST）做查表，O(1) 轉換，每筆 25 bytes，~0.1% 空間開銷。

**TieredVol 怎麼做：**
用 `stripe_size` 直接計算 offset：`disk_offset = (stripe_number × weight[i]) / weight_sum × stripe_size`。純數學計算，O(1)，零額外空間。

**誰比較好：**
- 兩者都是 O(1)，功能等價
- TieredVol 更簡單（零額外空間），但 Asym-RAID 的表可以支援更複雜的映射（例如多對多）

**建議：**
擇一即可。場景不需要複雜映射，weight partition + 直接計算已經足夠。如果未來要做 RAID5 parity，再考慮引入 SST。

---

## 3. 效能感知 Stripe Group + 差異化地址匯出

**Asym-RAID 怎麼做：**
把碟分成多個 stripe group（例如高效 group、低效 group），每個 group 有獨立的地址空間。然後把這些地址空間**差異化匯出**給上層，讓檔案系統能選擇把熱資料放在高效 group。

**TieredVol 怎麼做：**
用 segment + weight 分組，但**只匯出一個區塊裝置**。上層看不到哪個 segment 是高效的。

**誰比較好：**
- Asym-RAID 的差異化匯出是**獨特貢獻**，讓 FS 能做更智慧的資料放置
- TieredVol 做不到這點，因為它對上層是透明的

**建議：**
這是一個**互補**的功能。如果要實現它，需要改動比較多（新增多個 LV 匯出）。但對於 4 個場景（Edge AI、衛星、車載、工廠），FS 層級的智慧放置**不是必要條件**——痛點是底層的動態偵測和熱插拔。

---

## 4. Learned Index Model

**Asym-RAID 怎麼做：**
用 piecewise linear model（一種簡單的 ML 模型）取代傳統查表做地址轉換。模型先用 SSD 實測的 I/O 效能訓練，預測最優的 stripe 分配。

**TieredVol 怎麼做：**
完全沒有 ML，用靜態 weight 配置。

**誰比較好：**
- Learned model 是創新手法，但需要訓練數據、推論開銷
- TieredVol 的靜態 weight 更簡單、確定性更高

**建議：**
這是 Asym-RAID 的學術亮點，但不是計畫重點。計畫目標是動態偵測和熱插拔，不是地址轉換的優化。

---

## 5. Adaptive Data Layout（動態碟異質適應）

**Asym-RAID 怎麼做：**
列為 ongoing work，尚未實作。理論上要偵測碟速度變化並重新配置 ILP 模型。

**TieredVol 怎麼做：**
有 staleness detection（timer-based）和 wear tracking（per-disk write bytes）。但偵測邏輯比較簡單——只是計數，沒有即時速度監控。

**誰比較好：**
- Asym-RAID 沒有
- TieredVol 有基礎但不夠強
- **兩者都需要加強**

**建議：**
這是計畫的**核心目標**。要做的比兩者都強：
1. uevent listener 偵測碟的新增/移除
2. sysfs monitor 偵測速度退化
3. DM message 觸發動態重新配置

---

## 6. Hot-plug（熱插拔）

**Asym-RAID 怎麼做：**
完全沒提到。ILP 模型是離線計算，無法在運行時變更。

**TieredVol 怎麼做：**
沒有。移除碟會導致 bio 遺失、系統 hang 住。

**誰比較好：**
- **兩者都沒有**

**建議：**
這是計畫的**第二個核心目標**。對於 4 個場景（衛星、車載、工廠），熱插拔是剛需。要實現：
1. Online Add：新增碟時動態擴展 segment
2. Online Remove：移除碟前遷移 stripe

---

## 7. 結構化診斷日誌

**Asym-RAID 怎麼做：**
沒有。沒有任何運行時診斷機制。

**TieredVol 怎麼做：**
只有 `pr_info`/`pr_err`，沒有結構化日誌。

**誰比較好：**
- **兩者都沒有**

**建議：**
這是計畫的**第三個核心目標**。要實現：
1. Ring buffer 記錄 I/O 事件
2. Log level 動態調整
3. dmsetup query 即時查詢

---

## 總結：誰有什麼、缺什麼

| 功能 | Asym-RAID | TieredVol | 關係 |
|------|:---------:|:---------:|:----:|
| ILP 容量最大化 | ✅ | ❌ | 互補 |
| SST 地址映射 | ✅ | ✅（直接計算） | 擇一 |
| Stripe Group 分組 | ✅ | ✅（segment） | 擇一 |
| 差異化地址匯出 | ✅ | ❌ | 互補 |
| Learned Index Model | ✅ | ❌ | 互補 |
| Adaptive 負載均衡 | ❌ ongoing | ✅ | TieredVol 領先 |
| Staleness Detection | ❌ | ✅（基礎） | TieredVol 領先 |
| Wear Tracking | ❌ | ✅ | TieredVol 領先 |
| Hot-plug | ❌ | ❌ | **都缺** |
| 結構化日誌 | ❌ | ❌ | **都缺** |
| Kernel dm-target | ❌ | ✅ | TieredVol 獨有 |
| Runtime DM message | ❌ | ✅（15+ cmd） | TieredVol 獨有 |

---

## 計畫定位

Asym-RAID 解決了靜態異質性的最優分配問題（ILP + learned model），但假設碟的效能為不變。本計畫在 TieredVol kernel dm-target 基礎上，補足 Asym-RAID 未處理的三個缺口：動態偵測、熱插拔、結構化日誌。
