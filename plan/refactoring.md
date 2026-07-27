# TieredVol v5.0 重構計劃

## 目標
將混亂的巨型代碼庫重構為清晰的模組化架構。每階段完成後必須通過所有測試。

## 基線
- 81/81 單元測試 (test_common: 34, test_mapper: 14, test_partition: 19, test_metadata: 14)
- 49/49 整合測試 (test_comprehensive.sh)
- Kernel module: tieredvol_core.c (1843行), tieredvol_map.c (208行), tieredvol_meta.c (321行)
- Userspace: 12 個源文件 → tiered_setup

---

## Phase 0: 刪除死代碼
**目標:** 移除孤立的 `src/tiered_io.c`

| 文件 | 動作 |
|------|------|
| `src/tiered_io.c` | **刪除** (358行，有 main() 但 Makefile 從未編譯) |

**驗證:** `make clean && make test` → 81/81
**風險:** 零
**狀態:** [x] 完成 (commit 91a25dd)

---

## Phase 1: 拆分 tieredvol_core.c
**目標:** 1843行巨型文件 → 5個聚焦子文件

### 新文件結構

| 文件 | ~行數 | 職責 |
|------|-------|------|
| `tieredvol_core.c` | 380 | DM 生命週期: ctr/dtr/map/end_io/status/io_hints/ioctl/iterate, trigger_event, module init/exit |
| `tieredvol_message.c` | 450 | tieredvol_message() dispatch + tv_metadata_save_kernel() |
| `tieredvol_mirror.c` | 370 | 鏡像: tv_mirror_end_io, pending read/write tracking, retry work, rebuild thread |
| `tieredvol_log.c` | 100 | 日誌 ring buffer + tv_decay_timer_fn + EMA + per-CPU stats |
| `tieredvol_sysfs.c` | 200 | 12個 sysfs show/store + tv_sysfs_init/tv_sysfs_exit |

### tieredvol_log.c 包含的函式
- `tv_log()` (原 line 58-79, 改為 extern)
- `tv_read_count()`, `tv_read_sectors()`, `tv_read_bytes()` (原 line 88-113)
- `tv_decay_timer_fn()` (原 line 117-167)
- 數據: log_size, tv_log_fifo, tv_log_lock, tv_log_level, tv_map_count/sectors/bytes (per-CPU)

### tieredvol_mirror.c 包含的函式
- `tv_mirror_end_io()` (原 line 169-182)
- `tv_pending_add()`, `tv_pending_find_and_remove()` (原 line 220-267)
- `tv_pw_add()`, `tv_pw_remove()`, `tv_pw_is_pending()` (原 line 269-332)
- `tv_read_retry_work()` (原 line 342-379)
- `tv_rebuild_end_io()` (原 line 994-999)
- `tv_rebuild_thread()` (原 line 1001-1143)
- 數據: tv_pending_reads[], tv_pending_writes[] 及相關 lock/head/count

### tieredvol_sysfs.c 包含的函式
- 所有 sysfs show/store (原 line 1639-1781)
- tv_attrs[], tv_attr_group (原 line 1782-1803)
- `tv_sysfs_init()`, `tv_sysfs_exit()` (原 line 1805-1826)

### tieredvol_message.c 包含的函式
- `tv_metadata_save_kernel()` (原 line 880-987, static)
- `tieredvol_message()` (原 line 1145-1581)

### tieredvol_core.c 保留的函式
- `trigger_event()` (line 81-86)
- `tieredvol_map()` (line 381-492)
- `tieredvol_end_io()` (line 494-552)
- `tieredvol_ctr()` (line 554-742)
- `tieredvol_dtr()` (line 744-771)
- `tieredvol_prepare_ioctl()`, `tieredvol_io_hints()`, `tieredvol_iterate_devices()` (line 773-809)
- `tieredvol_status()` (line 811-873)
- `tieredvol_target` struct (line 1583-1597)
- `tieredvol_init()`, `tieredvol_exit()` (line 1599-1635, 1828-1843)
- 數據: tv_active_ctx

### tieredvol.h 新增宣告
```c
// tieredvol_log.c exports
void tv_log(u8 level, u8 disk_idx, u8 event_type, const char *fmt, ...);
u64 tv_read_count(void);
u64 tv_read_sectors(void);
u64 tv_read_bytes(void);
extern struct kfifo tv_log_fifo;
extern spinlock_t tv_log_lock;
extern u8 tv_log_level;
extern unsigned int log_size;

// tieredvol_mirror.c exports
void tv_pw_add(struct block_device *bdev, sector_t sector, unsigned int size);
void tv_pw_remove(struct block_device *bdev, sector_t sector, unsigned int size);
bool tv_pw_is_pending(struct block_device *bdev, sector_t sector, unsigned int size);
void tv_pending_add(struct block_device *bdev, sector_t sector, unsigned int size, int mirror_disk);
int tv_pending_find_and_remove(struct block_device *bdev, sector_t sector, unsigned int size);

// tieredvol_sysfs.c exports
void tv_sysfs_init(void);
void tv_sysfs_exit(void);

// tieredvol_message.c exports
int tieredvol_message(struct dm_target *ti, unsigned int argc,
                      char **argv, char *result, unsigned int maxlen);

// Global active context
extern struct tieredvol_ctx *tv_active_ctx;
```

### Kbuild 更新
```
obj-m := tieredvol.o
tieredvol-objs := tieredvol_core.o tieredvol_map.o tieredvol_meta.o \
                  tieredvol_log.o tieredvol_mirror.o tieredvol_sysfs.o \
                  tieredvol_message.o
```

**驗證:** `make module` + `make test` → 81/81 + `test_comprehensive.sh` → 49/49
**風險:** 中（純代碼搬移）
**狀態:** [x] 完成 (Phase 1+2 combined)

---

## Phase 2: 拆分 tieredvol_ctx god-struct
**目標:** 42欄位 god-struct → 5個子結構

### 新子結構
```c
struct tv_io_stats {
    atomic_t in_flight_bytes[TV_MAX_DISKS];
    u64 total_write_bytes[TV_MAX_DISKS];
    u64 total_read_bytes[TV_MAX_DISKS];
    u64 total_write_ops[TV_MAX_DISKS];
    u64 total_read_ops[TV_MAX_DISKS];
};

struct tv_adaptive_state {
    u32 ema_weight_shift;
    u64 ema_load[TV_MAX_DISKS];
    u64 stale_after_ns;
    bool stale[TV_MAX_DISKS];
    u64 stale_marked_ns[TV_MAX_DISKS];
    u64 grace_until_ns[TV_MAX_DISKS];
    u64 last_finish_ns[TV_MAX_DISKS];
    u64 last_interval_bytes[TV_MAX_DISKS];
    struct timer_list decay_timer;
    u32 wear_bias;
    enum tv_policy policy;
};

struct tv_mirror_stats {
    u64 mirror_write_bytes;
    u64 mirror_write_ops;
    u64 mirror_errors;
};

struct tv_rebuild_state {
    struct task_struct *thread;
    int seg_idx;
    u64 offset;
    u64 total;
    atomic_t running;
    struct completion done_r;
    struct completion done_w;
};

struct tv_degradation {
    atomic_t *error_count;
    u32 error_threshold;
    bool degraded[TV_MAX_DISKS];
};
```

### tieredvol_ctx 重寫
```c
struct tieredvol_ctx {
    struct dm_target *ti;
    struct tieredvol_metadata meta;
    struct dm_dev **devs;
    sector_t *disk_sectors;
    char config_path[256];
    int ndisks;
    sector_t min_chunk_sectors;
    sector_t stripe_sectors;
    bool adaptive_enabled;
    struct tv_io_stats io;
    struct tv_degradation deg;
    struct tv_adaptive_state adaptive;
    struct tv_mirror_stats mirror;
    struct tv_rebuild_state rebuild;
    struct work_struct trigger_event;
};
```

### 替換對照表
| Before | After |
|--------|-------|
| `ctx->in_flight_bytes[i]` | `ctx->io.in_flight_bytes[i]` |
| `ctx->total_write_bytes[i]` | `ctx->io.total_write_bytes[i]` |
| `ctx->total_read_bytes[i]` | `ctx->io.total_read_bytes[i]` |
| `ctx->total_write_ops[i]` | `ctx->io.total_write_ops[i]` |
| `ctx->total_read_ops[i]` | `ctx->io.total_read_ops[i]` |
| `ctx->error_count` | `ctx->deg.error_count` |
| `ctx->error_threshold` | `ctx->deg.error_threshold` |
| `ctx->degraded[i]` | `ctx->deg.degraded[i]` |
| `ctx->ema_weight_shift` | `ctx->adaptive.ema_weight_shift` |
| `ctx->ema_load[i]` | `ctx->adaptive.ema_load[i]` |
| `ctx->stale_after_ns` | `ctx->adaptive.stale_after_ns` |
| `ctx->stale[i]` | `ctx->adaptive.stale[i]` |
| `ctx->stale_marked_ns[i]` | `ctx->adaptive.stale_marked_ns[i]` |
| `ctx->grace_until_ns[i]` | `ctx->adaptive.grace_until_ns[i]` |
| `ctx->last_finish_ns[i]` | `ctx->adaptive.last_finish_ns[i]` |
| `ctx->last_interval_bytes[i]` | `ctx->adaptive.last_interval_bytes[i]` |
| `ctx->decay_timer` | `ctx->adaptive.decay_timer` |
| `ctx->wear_bias` | `ctx->adaptive.wear_bias` |
| `ctx->policy` | `ctx->adaptive.policy` |
| `ctx->mirror_write_bytes` | `ctx->mirror.mirror_write_bytes` |
| `ctx->mirror_write_ops` | `ctx->mirror.mirror_write_ops` |
| `ctx->mirror_errors` | `ctx->mirror.mirror_errors` |
| `ctx->rebuild_thread` | `ctx->rebuild.thread` |
| `ctx->rebuild_seg_idx` | `ctx->rebuild.seg_idx` |
| `ctx->rebuild_offset` | `ctx->rebuild.offset` |
| `ctx->rebuild_total` | `ctx->rebuild.total` |
| `ctx->rebuild_running` | `ctx->rebuild.running` |
| `ctx->rebuild_done_r` | `ctx->rebuild.done_r` |
| `ctx->rebuild_done_w` | `ctx->rebuild.done_w` |

**影響文件:** 所有 driver/ 下的 .c 文件
**驗證:** `make module` + `make test` + `test_comprehensive.sh`
**風險:** 中（機械式全局替換）
**狀態:** [x] 完成 (Phase 1+2 combined, 81/81 unit tests pass)

---

## Phase 3: Message Handler Dispatch Table
**目標:** 436行 if/else if 鏈 → dispatch table + 27個獨立函式

### 結構
```c
struct tv_msg_handler {
    const char *name;
    int min_argc;
    int max_argc;
    int (*fn)(struct dm_target *ti, unsigned int argc,
              char **argv, char *result, unsigned int maxlen);
};
```

### 27個獨立函式
每個 if-block 提取為獨立的 `static int msg_xxx(...)` 函式。
`tieredvol_message()` 變為 ~40行的 dispatch loop。

**驗證:** `make module` + `make test` + `test_comprehensive.sh`
**風險:** 低-中（純重構）
**狀態:** [x] 完成 (81/81 unit tests pass)

---

## Phase 4: Userspace 職責分離
**目標:** 修正錯誤的職責分配

### 4a: cmd_status() 移出 cmd_remove.c
| 文件 | 動作 |
|------|------|
| `src/cmd_status.c` | **新建** — cmd_status() |
| `src/cmd_status.h` | **新建** — 宣告 |
| `src/cmd_remove.c` | 刪除 cmd_status() |
| `src/cmd_remove.h` | 刪除 cmd_status 宣告 |
| `src/main.c` | 新增 #include "cmd_status.h" |

### 4b: 拆分 cmd_create.c
| 文件 | 動作 |
|------|------|
| `src/cmd_scheduler.c` | **新建** — create_scheduler() + cleanup_scheduler() |
| `src/cmd_scheduler.h` | **新建** — 宣告 |
| `src/cmd_create.c` | 保留 LVM 路徑，調用 cmd_scheduler |

**驗證:** `make clean && make test`
**風險:** 低
**狀態:** [x] 完成 (81/81 unit tests pass)

---

## Phase 5: 共用 Metadata Format Header
**目標:** 消除 kernel/userspace 之間的格式重複

| 文件 | 動作 |
|------|------|
| `common/tieredvol_meta_format.h` | **新建** — 格式常數 |
| `driver/tieredvol_meta.c` | 使用常數 |
| `src/tiered_metadata.c` | 使用常數 |
| `driver/tieredvol_message.c` | tv_metadata_save_kernel() 使用常數 |

**驗證:** `make clean && make test`
**風險:** 低-中
**狀態:** [ ] 待執行

---

## Phase 6: 命名統一
**目標:** 所有 userspace 檔案統一 tieredvol_ 前綴

### 重命名對照
| Before | After |
|--------|-------|
| `src/tiered_common.h` | `src/tieredvol_common.h` |
| `src/tiered_types.h` | `src/tieredvol_types.h` |
| `src/tiered_mapper.c` | `src/tieredvol_umapper.c` |
| `src/tiered_metadata.c` | `src/tieredvol_umeta.c` |
| `src/tiered_partition.c` | `src/tieredvol_partition.c` |
| `src/tiered_benchmark.c` | `src/tieredvol_benchmark.c` |
| `src/exec_helper.c` | `src/tieredvol_exec.c` |
| `src/exec_helper.h` | `src/tieredvol_exec.h` |
| `src/setup_discover.c` | `src/tieredvol_discover.c` |
| `src/setup_discover.h` | `src/tieredvol_discover.h` |
| `src/setup_bench.c` | `src/tieredvol_bench.c` |
| `src/setup_bench.h` | `src/tieredvol_bench.h` |
| `src/warmup.c` | `src/tieredvol_warmup.c` |
| `src/warmup.h` | `src/tieredvol_warmup.h` |

### 不重命名
- `src/main.c` — 入口點
- `src/cmd_*.c` / `src/cmd_*.h` — cmd_ 前綴正確

### 全部 #include 更新
- Makefile 所有 SCHED_OBJS / SETUP_OBJS / test targets
- 所有 src/ 和 tests/ 中的 #include

**驗證:** `make clean && make test`
**風險:** 中
**狀態:** [x] 完成 (81/81 unit tests pass)

---

## Phase 7: 測試輸出路徑
**目標:** 測試二進位從專案根目錄移到 tests/ 目錄

- 更新 Makefile: test targets → tests/test_xxx
- 更新 .gitignore
- 更新 test target 路徑

**驗證:** `make clean && make test`
**風險:** 低
**狀態:** [ ] 待執行

---

## Phase 8: 腳本 + 文檔清理
**目標:** 最終清理

- 更新 README.md 文件結構圖
- 更新 MIGRATION.md 加入重構說明
- 驗證 scripts/ 路徑正確
- driver/.gitignore 檢查

**驗證:** `make clean && make test`
**風險:** 低
**狀態:** [ ] 待執行

---

## 執行順序
```
Phase 0 → Phase 1 → Phase 2 → Phase 3 → Phase 4 → Phase 5 → Phase 6 → Phase 7 → Phase 8
```

## 每階段必做
1. `make module` — kernel module 編譯
2. `make clean && make test` — 81/81 單元測試
3. `test_comprehensive.sh` — 49/49 整合測試（Phase 1-3 需要）
4. `git add -A && git commit` — 記錄變更
5. `git push origin main` — 推送到 v2
6. 更新本文件的狀態欄位
