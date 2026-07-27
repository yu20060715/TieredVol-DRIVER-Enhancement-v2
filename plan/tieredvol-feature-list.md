# TieredVol v5.0.0 Feature List

---

## v5.0 重要变更

1. I/O 分派移至独立文件 `tieredvol_map.c`（曾内嵌在 `core.c` 的 `tieredvol_map()` 函数中）
2. 新增 `tieredvol_map.c`（逻辑→物理映射：static/adaptive/random）
3. 新增 `tieredvol_mirror.c`（镜像 I/O + pending 追踪 + timestamp ring + rebuild）
4. 新增 `tieredvol_log.c`（日志 ring + EMA 衰减计时器）
5. 新增 `tieredvol_message.c`（27 条消息命令，含 CRC32C）
6. 新增 `tieredvol_meta.c`（配置文件解析器 + CRC32C）
7. 新增 `tieredvol_sysfs.c`（sysfs 接口，7 个属性）
8. 新增 `tieredvol.h`（集中定义所有结构体）
9. 重构 `tieredvol_core.c` 为纯 DM 生命周期管理
10. 移除 `tieredvol_stats.h`（统计功能合并到 `tv_io_stats`）
11. 移除 `tv_read_iops/stats/latency` 字段（合并到 `tv_io_stats`）
12. 新增多因子自适应分派（EMA 载入 + 延迟 + 磨损惩罚）
13. 新增 per-CPU pending 数组（无锁镜像追踪）
14. 新增 timestamp ring（精确延迟测量）
15. 新增 mempool（零 OOM 镜像/重试上下文）
16. 新增 CRC32C 校验（非破坏性预扫描）
17. 新增自适应衰减计时器间隔（100ms/1s）
18. 新增 mirror_enabled_any 守卫标志
19. 新增 sysfs 运行时配置接口
20. 新增降级管理（错误阈值 + 自动检测 + 手动恢复）
21. 新增 rebuild 子系统（kthread + 指数退避）

---

## 1. I/O 分派

### #1 静态加权分派 `tv_map_logical()`
- 按 segment 的加权条带边界，确定性地映射逻辑偏移到物理碟
- 实现：`tieredvol_map.c:4-65`
- 内核参考：`dm-stripe.c`

### #2 自适应多因子分派 `tv_map_logical_adaptive()`
- 多因子评分选择最优碟，自动跳过 stale 碟
- 评分 = ema_load + ema_latency_us + wear_penalty
- Fallback 两阶段扫描：先跳过 stale/degraded，全部都是才接受任何碟
- 实现：`tieredvol_map.c:132-161`
- 内核参考：`kyber-iosched.c`, `mq-deadline.c`
- 学术参考：Asymmetric RAID (HotStorage'24)

### #3 随机分派 `tv_map_logical_random()`
- segment 内均匀随机选碟
- 实现：`tieredvol_map.c:158-201`

### #4 Bio sector 重映射 `tieredvol_map()`
- 通过 `bio_set_dev()` + `bi_iter.bi_sector` 重定向到正确物理碟
- 实现：`tieredvol_core.c:33-164`
- 内核参考：`dm-crypt.c`, `dm-linear.c`

### #5 无效碟错误 `tieredvol_map()`
- 碟索引越界时返回 bio_io_error
- 实现：`tieredvol_core.c:89-96`
- 内核参考：`dm.c`

### #6 写入镜像
- clone 写入 bio 提交到镜像碟，零 OOM（mempool）
- 实现：`tieredvol_core.c:126-160`
- 内核参考：`dm-raid1.c`

---

## 2. 负载均衡

### #7 EMA 载入计算
- `ema = ema * (1 - alpha) + snapshot * alpha`
- 实现：`tieredvol_log.c:81-83`
- 内核参考：`kernel/sched/fair.c`

### #8 EMA IOPS 计算
- 平滑突發 IOPS
- 实现：`tieredvol_log.c:86-88`

### #9 EMA 延迟测量
- 结合 timestamp ring 精确追踪
- 实现：`tieredvol_log.c:91-102`, `tieredvol_mirror.c:155-219`

### #10 In-flight 字节追踪
- 每碟原子计数器
- 实现：`tieredvol_core.c:109`
- 内核参考：`blk-mq.c`

### #11 自适应衰减计时器间隔
- 高活跃 → 100ms，低活跃 → 1s
- 实现：`tieredvol_log.c:143-145`
- 内核参考：`kernel/time/timer_list.c`

---

## 3. Stale 碟检测

### #12 Stale 标记
- 超时无 I/O 完成时标记
- 实现：`tieredvol_log.c:112-123`
- 内核参考：`dm-dust.c`, `dm-raid1.c`

### #13 Stale 恢复 — I/O 触发
- 新 I/O 完成时立即恢复
- 实现：`tieredvol_log.c:124-129`
- 内核参考：`dm-log.c`

### #14 Stale 恢复 — 冷却
- 2 倍超时后自动恢复
- 实现：`tieredvol_log.c:130-138`
- 内核参考：`disk-events.c`

### #15 保护期
- 防止恢复后立即重标记
- 实现：`tieredvol_log.c:126,134`
- 内核参考：`dm-raid1.c`

---

## 4. Per-disk I/O 统计

### #16 读取字节计数器
- 实现：`tieredvol_core.c:114`

### #17 读取操作计数器
- 实现：`tieredvol_core.c:115`

### #18 写入字节计数器
- 实现：`tieredvol_core.c:111`

### #19 写入操作计数器
- 实现：`tieredvol_core.c:112`

### #20 错误计数器
- 原子计数，bio 完成错误时递增
- 内核参考：`dm.c`, `dm-raid1.c`

---

## 5. DM Message 命令（27条）

### `reset_stats`（0）
- 清除所有 I/O 统计
- 实现：`tieredvol_message.c`

### `show_stats`（1）
- 返回每碟读写 ops/bytes
- 实现：`tieredvol_message.c`

### `show_inflight`（3）
- 返回每碟 in-flight 字节数
- 实现：`tieredvol_message.c`

### `show_io_stats`（4）
- 返回完整 per-disk I/O 统计
- 实现：`tieredvol_message.c`

### `reset_io_stats`（5）
- 归零所有 7 个计数器
- 实现：`tieredvol_message.c`

### `adaptive_on`（6）
- 启用自适应分派
- 实现：`tieredvol_message.c`

### `adaptive_off`（7）
- 切换回静态分派
- 实现：`tieredvol_message.c`

### `set_policy <name>`（8）
- 设置分派策略 static/adaptive/random
- 实现：`tieredvol_message.c`

### `set_ema_shift <shift>`（9）
- 设置 EMA alpha shift（0-10）
- 实现：`tieredvol_message.c`

### `set_stale_ms <ms>`（10）
- 设置 stale 超时（毫秒）
- 实现：`tieredvol_message.c`

### `show_adaptive`（11）
- 返回策略/EMA/latency/stale/wear 状态
- 实现：`tieredvol_message.c:304-327`

### `show_wear`（12）
- 返回 wear bias 和每碟写入字节
- 实现：`tieredvol_message.c`

### `set_wear_bias <bias>`（13）
- 设置磨損懲罰因子（0-1024）
- 实现：`tieredvol_message.c`

### `reset_wear`（14）
- 归零磨損字節計數器
- 实现：`tieredvol_message.c`

### `show_mirror`（15）
- 返回镜像状态
- 实现：`tieredvol_message.c`

### `set_mirror <seg> <disk>`（16）
- 为 segment 启用镜像
- 实现：`tieredvol_message.c`
- `ctx->mirror_enabled_any = true`（`tieredvol_mirror.c:398`）

### `show_log`（17）
- 非破坏性读取日志（kfifo_out+kfifo_in）
- 实现：`tieredvol_message.c:475-514`

### `clear_log`（18）
- 重置日志 ring
- 实现：`tieredvol_message.c`

### `set_loglevel <0-3>`（19）
- 设置日志等级 OFF/ERROR/WARN/INFO
- 实现：`tieredvol_message.c`

### `show_errors`（20）
- 返回每碟错误计数
- 实现：`tieredvol_message.c`

### `reset_errors`（21）
- 归零所有错误计数
- 实现：`tieredvol_message.c`

### `set_error_threshold <n>`（22）
- 设置触发降级的错误阈值
- 实现：`tieredvol_message.c`

### `show_degraded`（23）
- 返回降级状态
- 实现：`tieredvol_message.c`

### `clear_degraded`（24）
- 手动清除降级标志
- 实现：`tieredvol_message.c`

### `start_rebuild [max_bytes]`（25）
- 启动背景重建线程
- 实现：`tieredvol_message.c`

### `stop_rebuild`（26）
- 停止背景重建线程
- 实现：`tieredvol_message.c`

### `show_rebuild`（27）
- 返回重建进度
- 实现：`tieredvol_message.c`

---

## 6. DM 生命周期

### #48 构造函数 `tieredvol_ctr()`
- 解析参数，加载元数据，获取 DM 设备，分配 mempool，启动衰减计时器
- 实现：`tieredvol_core.c`

### #49 析构函数 `tieredvol_dtr()`
- 停止计时器/线程，释放设备和内存
- 实现：`tieredvol_core.c`

### #50 IO hints
- 向 DM 框架报告最优 I/O 大小
- 实现：`tieredvol_core.c`

### #51 Iterate devices
- 遍历所有底层 DM 设备
- 实现：`tieredvol_core.c`

### #52 Prepare ioctl
- 返回第一个底层 bdev 供 ioctl 透传
- 实现：`tieredvol_core.c`

### #53 Flush/Discard 传播
- 将 flush/discard 传播到所有底层碟
- 实现：`tieredvol_core.c`

### #54 模块初始化/退出
- 注册/注销 DM 目标
- 实现：`tieredvol_core.c:534-577`

---

## 7. 状态报告

### #55 STATUSTYPE_INFO
- 策略/镜像/错误/每碟 active+rd/wr 计数
- 实现：`tieredvol_core.c`

### #56 STATUSTYPE_TABLE
- 底层碟设备名列表
- 实现：`tieredvol_core.c`

### #57 STATUSTYPE_IMA
- IMA 占位符
- 实现：`tieredvol_core.c`

---

## 8. 元数据 + CRC32C

### #58 内核态文件配置
- filp_open + kernel_read
- 实现：`tieredvol_meta.c`
- 内核参考：`dm-thin-metadata.c`, `dm-log.c`

### #59 Version/Chunk/Segment/Disk 解析
- 含镜像安全性验证（mirror_disk != 主碟）
- 实现：`tieredvol_meta.c:150-187, 377-387`

### #60 CRC32C 校验
- 非破坏性预扫描 + position-based CRC
- 实现：`tieredvol_meta.c:98-133, 183-219`

---

## 9. 结构化日志

### #61 环形缓冲区
- kfifo + raw_spinlock，512 条目
- 实现：`tieredvol_log.c:27`
- 内核参考：`kernel/samples/kfifo/record-example.c`

### #62 日志等级
- 动态详细程度控制 OFF/ERROR/WARN/INFO
- 实现：`tieredvol_message.c`
- 内核参考：`kernel/trace/trace.c`

### #63 DM 查询
- show_log（非破坏性）/ clear_log（重置）
- 实现：`tieredvol_message.c:475-514`
- 内核参考：`dm-dust.c`, `dm-log-writes.c`

---

## 10. 镜像 + Pending 追踪

### #64 Per-CPU 读取 Pending 数组
- 无锁 ring buffer 追踪镜像读取 bio
- 实现：`tieredvol_mirror.c:22-79`

### #65 Per-CPU 写入 Pending 数组
- 无锁 ring buffer 追踪镜像写入 bio
- 实现：`tieredvol_mirror.c:209-213`

### #66 Timestamp Ring
- 每碟 256 条目，精确延迟测量
- 溢出时覆写最旧 entry（不丢弃），确保高 IOPS 下 EMA 不失真
- 使用 raw_spinlock_t（end_io handler 在 atomic context）
- 实现：`tieredvol_mirror.c:161-225`

### #67 Mempool
- 零 OOM 镜像 bio 和重试上下文
- 实现：`tieredvol_core.c:288-296`, `tieredvol_mirror.c:372`

### #68 mirror_enabled_any 守卫标志
- 全局布尔标志，快速跳过镜像逻辑
- 实现：`tieredvol.h:125`, `tieredvol_core.c:279-285`, `tieredvol_mirror.c:398`

---

## 11. 降级管理

### #69 Per-disk 原子错误计数器
- 实现：`tieredvol_core.c`

### #70 可配置错误阈值
- 实现：`tieredvol_message.c`

### #71 自动降级检测
- 实现：`tieredvol_core.c`

### #72 降级模式标志
- 实现：`tieredvol_core.c`

### #73 降级模式恢复
- 实现：`tieredvol_message.c`

---

## 12. 重建管理

### #74 kthread 背景重建
- 实现：`tieredvol_message.c`

### #75 指数退避重试
- 实现：`tieredvol_mirror.c`

### #76 重建进度追踪
- 实现：`tieredvol_message.c`

---

## 13. Sysfs 接口

### #77 policy（只读）
- `/sys/kernel/tieredvol/policy`
- 返回 static/adaptive/random

### #78 stale_ms（可读写）
- `/sys/kernel/tieredvol/stale_ms`
- 读返回毫秒，写设置 ns

### #79 wear_bias（可读写）
- `/sys/kernel/tieredvol/wear_bias`
- 读写验证 0-1024

### #80 ema_shift（可读写）
- `/sys/kernel/tieredvol/ema_shift`
- 读写验证 0-10

### #81 loglevel（可读写）
- `/sys/kernel/tieredvol/loglevel`
- 读写验证 0-3

### #82 disk_count（只读）
- `/sys/kernel/tieredvol/disk_count`
- 返回 ndisks

### #83 status（只读）
- `/sys/kernel/tieredvol/status`
- 返回状态摘要

---

## 单元测试

```
test/test.sh           38/38  ✓
test/test_message.sh   40/40  ✓
test/test_sysfs.sh      3/3   ✓
```

## 集成测试

```
test/test_integration.sh   81/81  ✓
```

---

## 参考文献

### 学术论文
- Jiao & Kim, "Asymmetric RAID: Rethinking RAID for SSD Heterogeneity" — HotStorage'24

### 内核源码
| 文件 | 引用 |
|------|------|
| `drivers/md/dm-stripe.c` | #1 |
| `drivers/md/dm-switch.c` | #1, #2, #3 |
| `drivers/md/dm-crypt.c` | #4, #48 |
| `drivers/md/dm-raid1.c` | #6, #12, #15, #20, #35 |
| `drivers/md/dm-dust.c` | #12, #28, #63 |
| `drivers/md/dm-thin.c` | #7, #58 |
| `drivers/md/dm-linear.c` | #4 |
| `drivers/md/dm.c` | #5, #16, #20 |
| `drivers/md/dm-log.c` | #13, #58 |
| `drivers/md/dm-log-writes.c` | #61, #63 |
| `drivers/md/dm-thin-metadata.c` | #58 |
| `drivers/md/dm-table.c` | #59 |
| `block/kyber-iosched.c` | #2 |
| `block/mq-deadline.c` | #2, #10 |
| `block/blk-mq.c` | #10, #17 |
| `kernel/time/timer_list.c` | #11 |
| `kernel/sched/fair.c` | #7 |
| `kernel/trace/ring_buffer.c` | #61 |
| `kernel/trace/trace.c` | #62 |
| `kernel/samples/kfifo/record-example.c` | #61 |

### 开源项目
| 项目 | 引用 |
|------|------|
| emlog (nicupavel) | #61 |
| sysprog21/kfifo-examples | #61 |
| mdadm | #59 |
