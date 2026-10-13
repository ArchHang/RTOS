# RTOS 测试报告（功能回归 + 边界 + HIL 复现审计）

> 工程: `E:\STM32_Project\CubeIDE\f446_rtos`（自研 RTOS, Cortex-M4F @168MHz）
> 方式: 硬件在环 (HIL) — NUCLEO-F446RE, USART2 @115200-8N1, SWD 烧录与监听脚本原子执行
> 工具链: GNU Tools for STM32 11.3.rel1, `-O2`
> 最新结果: **selftest 431 PASS / 0 FAIL / 11 INFO / 442**；**boundary 103 PASS / 0 FAIL**
> 一键复现: `hil\repro_all.ps1 -Only selftest,boundary`；原始日志 `hil\logs\current\`

---

## 0. 当前结果与验收门槛

| 套件 | 用例数 | 结果 | 固件 |
|---|---|---|---|
| `selftest` 全模块功能回归 | 442 | **431 PASS / 0 FAIL / 11 INFO** | `-Variant selftest` |
| `boundary_test` 契约边界夹具 | 103 | **103 PASS / 0 FAIL** | `-Variant boundary` |
| `benchmark` 性能微基准 | 8 项 | 见 `rtos_performance_report.md` | `-Variant bench` |
| 场景 S1–S5 | 5 场景 | 见 `rtos_performance_report.md` | `-Variant scenario` |

**任何内核改动必须满足**: `selftest` 跑到 `ST|SUMMARY` 且 `fail=0`；
`boundary` 103/0；微基准与基线逐位比对（允许 BM2/BM4 在 §布局敏感性 ±7% 内浮动）。

---

## 1. 功能回归：覆盖矩阵（selftest, 442 用例）

输出协议 `ST|<模块>|<用例>|<PASS/FAIL/INFO>|<详情>`。下表用例数按 `ST|` 行的**模块字段**统计
（`ST|SUMMARY` / `ST|DONE` 与 `ST|META|phase1|2` 两条叙事行不计），各阶段、各模块相加均为 442。

| 阶段 | 上下文 | 用例数 | 说明 |
|---|---|---|---|
| 阶段1 | `rtos_start()` 之前的 main（`ST\|META\|phase1` 起） | 73 | 调度器未启动时的 API 行为 |
| 阶段2 | selftest 任务 (prio 9)（`ST\|META\|phase2` 起） | 369 | 调度器运行中的全量功能 |
| 合计 | — | **442** | = `ST\|SUMMARY` 的 `pass=431` + `info=11` |

其中 33 条在**真实 USART2 RX 中断**里执行：固件打印 `ST|ISR|ARM` 提示上位机回发 `X`，
脚本自动应答后，中断上下文跑完整套 API 矩阵。

| 模块 | 用例 | 正常 | 边界 | 边界外/非法 | 结果 |
|---|---|---|---|---|---|
| QUEUE 队列 | 47 | FIFO、to_front、阻塞唤醒、双向阻塞 | 满/空×NO_WAIT、超时、零拷贝 | NULL×5、未 init×3、容量 0、item 0 | 全 PASS |
| MUTEX 互斥锁 | 45 | 递归×3 层、移交、优先级继承+恢复 | 递归期间他人获取、deinit 唤醒 | NULL、未 init、未持锁 give | 全 PASS |
| SEM 信号量 | 42 | 计数/二值、直接移交、优先级唤醒序 | 计数上限溢出、超时精度 | NULL、未 init、max 0、init>max、deinit | 全 PASS |
| TASK 任务 | 36 | 创建/删除/挂起/恢复/自删 | prio 31、栈 96 字、返回兜底 | NULL×6、prio 越界×2、栈 95 字、notify 非法 type×3 | 全 PASS |
| EVENT 事件组 | 34 | set/clear 粘性、ANY/ALL、CLEAR_ON_EXIT、24 位宽 | 超时返回当前位 | NULL、未 init、bits 0、非法 mode | 全 PASS |
| TIMER 定时器 | 34 | 单次/周期/重启重计时/change_period/回调内自调用 | 周期精度、幂等 stop | NULL cb、period 0、未 init | 全 PASS |
| ISR 错误码矩阵 | 33 | 允许列表全部实测 OK | delay 静默、event_wait 返回 0 | 禁止列表全部 `ERR_ISR` | 全 PASS |
| PERF 性能监视 | 31 | 快照/任务统计/栈水位/窗口推进/内存统计/截断 | 数值范围 | NULL×5、max_count 0 | 全 PASS |
| MEM 内存池 | 26 | 全分配/释放/非对齐向上取整 | 耗尽返回 NULL、计数一致 | NULL、池外指针 + 双重释放 INFO | 全 PASS |
| NOTIFY 通知 | 14 | VALUE/BIT/INCREMENT | 超时精度、自通知 | 非法 type、NULL | 全 PASS |
| HEAP 动态堆 | 13 | 对齐 8、写读、计数 | 耗尽后全回收、巨型分配失败 | alloc(0)、双重释放、野指针 | 全 PASS |
| SCHED 调度器 | 11 | 状态/tick/yield | delay 精度、delay_until 周期 | NULL 参数 + 3 SKIP | 全 PASS |
| SLICE 时间片 | 4 | 同优先级交替 a=100/b=100 | 计数均衡 | — | 全 PASS |
| OVF 栈溢出检测 | 3 | hook 触发/任务归因/魔数修复 | — | — | 全 PASS |
| META 元信息 | 1 | 版本号 | — | — | 全 PASS |
| **FIX1~FIX12** | 28 | 历史缺陷回归（tick 回绕、resume 竞态、event UAF、FPU 最小栈、heap 输入域、to_front 代办、heap 合并统计、双重删除、多锁继承、等待链重排、注册表满、timer 回调自调用） | | | 全 PASS |
| **FIX13~FIX18** | 28 | **HIL 复核新发现缺陷的回归**，见 §3（6 组缺陷、28 条断言） | | | 全 PASS |
| **FIX19** | 12 | **第二轮审计的 `rtos_sched_suspend` 契约修复回归**，见 §10.1 | | | 全 PASS |

### 1.1 ISR 错误码矩阵（真实 USART2 中断上下文）

| API（ISR 中调用） | 实测 | 判定 |
|---|---|---|
| `queue_recv/send` NO_WAIT | OK + 数据正确 | ✓ 契约 |
| `queue_recv/send` timeout>0 | `ERR_ISR` | ✓ |
| `sem_take` NO_WAIT（空） | `ERR_TIMEOUT` | ✓ |
| `sem_take` timeout>0 | `ERR_ISR` | ✓ |
| `sem_give` | OK（任务侧可取走） | ✓ |
| `mutex_take/give`（任意 timeout） | `ERR_ISR` | ✓ |
| `task_delete` / `notify_wait` | `ERR_ISR` | ✓ |
| `task_notify` | OK（victim 收到 0xAB） | ✓ |
| `task_suspend/resume`（他人） | OK | ✓ |
| `task_delay` | 静默空操作，不卡死 | ✓ |
| `event_wait` timeout>0 | 返回 0 | ✓（见 §4 R8） |
| `event_set` | 返回置位后的位 | ✓ |
| `timer_start` | OK（入队，后续真触发） | ✓ |
| `perf_get_cpu_usage` | 值域正常 | ✓ ISR 安全 |

### 1.2 关键实测数值

| 类别 | 用例 | 实测 |
|---|---|---|
| 延时精度 | `delay(100)/delay(200)/delay_until 3×100` | dt = 100/200/300（**全部 0 误差**） |
| 通知/队列超时精度 | `notify_wait(40)` / 满 send(40) / 空 recv(40) | r=−3, dt=40 |
| 信号量移交语义 | give 唤醒 taker 后计数 | cnt=0（不移入计数，直接移交）✓ |
| 优先级继承 | low(20) 持锁，high(4) 阻塞 | low 被提升至 4，give 后恢复 20 |
| 挂起阻塞任务 | suspend→resume | 阻塞 API 返回 `ERR_DELETED` ✓ |
| 时钟快照 | perf 系统快照 | 1000 Hz / 168 000 000 Hz 精确 |
| 时间片轮转 | 同优先级双任务交替 | a=100 / b=100 完全均衡 |
| 堆耗尽/回收 | alloc(1024) 循环 | 全部 free 后 free 字节完全恢复 |

### 1.3 阶段1（调度器启动前）实测行为

- `rtos_sched_get_state() == NOT_STARTED`、`tick == 0`、`get_current() == NULL` ✓
- 参数校验全部正确：NULL→`ERR_NULL`/`ERR_PARAM`、prio ≥32→`ERR_PARAM`、栈<96 字→`ERR_PARAM`、
  notify 非法 type→`ERR_PARAM` ✓
- 非阻塞路径（sem 计数/queue 收发/event set/get/mempool/heap）启动前完全可用 ✓
- 5 项危险场景按计划 SKIP（INFO）：`pre_delay_skip` / `pre_block_skip` /
  `pre_block_take_skip` / `pre_wait_skip`(event) / `pre_wait_skip`(notify)

---

## 2. 边界夹具（boundary_test, 103 断言）

覆盖"有效值 / 下边界 / 上边界 / 超出上边界 / 超出下边界 / 空值"六类。

| 分类 | 覆盖要点 | 结果 |
|---|---|---|
| SEM | `init_cnt == max_cnt` 恰好合法 / `>max_cnt` 越界 / `max=0` / `UINT32_MAX` 组合 / 空值；`give` 到 max 后溢出；超时=0 下边界；未初始化与已 deinit | 23/23 PASS |
| MUTEX | 空值 / 未持有释放 / 重复释放 / 重入 take-give 计数 / 释放后状态自洽 | 13/13 PASS |
| QUEUE | item=0 / cap=0 / 空队列 recv / 恰好填满 / 满后再发 / **溢出失败后已有数据完好且顺序正确** / 容量 1 极值队列 | 31/31 PASS |
| TASK | 栈 96 恰好合法 vs 95 越界 / 优先级 0 与 31 合法 vs 32、255 越界 / 名称空值合法且可运行 / **一串非法创建后合法创建仍成功（无堆泄漏）** | 17/17 PASS |
| NOTIFY | 空 TCB / 非法类型 / 自投递取值完整 / 超时 0 下边界 | 7/7 PASS |
| DELAY/STABLE | `delay(0)` 与 `delay(1)` 均返回 / 边界用例后 yield 50 次仍稳定 / tick 递增 | 4/4 PASS |

**稳定性关键结论**: 所有非法入参除返回正确错误码外**均未破坏对象状态** ——
每个分类都在非法调用之后追加了"对象仍可正常 take/give/send/recv"的验证，
且队列溢出后 4 条既有数据逐条取值完好，证明无越界写与状态污染。

**几何量**（`BT|GEOM`）: `sizeof_tcb=140 | heap=32768 | max_alloc=28504 | stack_max_words=7088`

> ⚠ 夹具自身瑕疵（不影响结论）：`boundary_test.c:415` 计算上界时前面 4 组用例已跑过，
> 堆并非"初始形状"，而 `BT|GEOM` 打印的 `max_alloc` 也不是初始值（初始应为 32760）。
> 注释里写的"必须在创建任何任务之前采样"与代码位置不符 —— 当前恰好成立（前序用例均已释放），
> 属偶合而非不变式。

---

## 3. 本轮 HIL 复核发现并修复的内核缺陷（10 项）

全部先复现/取证，再修，最后固化为 FIX13~FIX18 回归用例（28 条断言）。

### 3.1 【真机复现】EH-1: `rtos_event_wait` 遗留陈旧 `queue_bitmap`

**性质**: 跨对象状态污染 —— 对一个对象的操作会去读改写**另一个无关对象**的内存。

**取证**（D4 场景, `-Variant diag -Scenario 16`, 修复前）:

```
SC|D4|blocked_in_event   | value=1
SC|D4|bitmap_addr        | value=0x20009BE0
SC|D4|sem_bitmap_addr    | value=0x20009BE0   ← 与无关信号量的位图地址完全相同
SC|D4|sem_bitmap_value   | value=2048         ← 该信号量位图被写入了 bit11
SC|D4|verdict            | value=DEFECT_bitmap_not_null
```

**根因**: `rtos_internal_block_current_on` 与 `rtos_task_notify_wait` 都显式把
`wait_node.queue_bitmap` 置 NULL，唯独 `rtos_event_wait` 逐字段填写时**漏了这两行**。
于是任务"先在信号量上超时、再在事件组上阻塞"后，节点仍带着旧信号量的位图指针；
此后任何 `rtos_task_set_priority()` 都会经 `rtos_internal_requeue_wait_lists` 的位图分支
去 RMW 那个旧信号量 —— **若该信号量已 deinit 且存储被复用，就是对任意内存的读改写**。

**修复**: `rtos_event.c` 补 `queue_bitmap = NULL; queued_priority = cur->priority;`
**修复后**: `bitmap_addr=0x00000000`、`sem_bitmap_value=0`、`verdict=OK_bitmap_null`

### 3.2 【真机复现】TM-1: `rtos_timer_change_period` 缺周期上界校验

**取证**（D5 场景, 修复前）:

```
SC|D5|change_period_ret     | value=0     ← 接受了 0xFFFFFFFF
SC|D5|fires_in_100ms_after  | value=100   ← 100ms 内触发 100 次 = 1kHz 回调风暴
SC|D5|verdict               | value=DEFECT_storm
```

**根因**: `rtos_timer_create` 有 `period >= 0x80000000 → ERR_PARAM`（注释明确写着
"否则周期定时器以 1kHz 风暴执行"），而 `rtos_timer_change_period` **只检查了 period == 0**。
改周期后 `expire_tick = now + period` 回绕到过去 → 服务任务每一轮都判定到期。

**修复**: 补同口径上界校验。**修复后**: `change_period_ret=-1`、`fires=1`、`verdict=OK_bounded`

### 3.3 TM-2: 对仍活跃的定时器重新 create 会破坏活跃链表

`rtos_timer_create` 无条件 `memset`，会把仍在活跃链表上的定时器的 `is_active/next/prev`
一并清零；随后 `start` 把已入链节点再次插入 → **自环**；`timer_collect_expired` 重复摘除
并构造 `t->next = t` → `timer_reinsert_periodic` 在**临界区内**死循环（SysTick 被屏蔽，整机冻结）。
**修复**: `is_initialized && is_active` 时返回 `ERR_ALREADY`（判据取两者相与，避免读取未清零
storage 的不可信字节导致首次 create 伪失败）。

### 3.4 BM-1 / BM-2: 位图索引链的"位图 ⇔ 链表"不变式被破坏

两类破坏，**方向相反**：

| 编号 | 位置 | 后果 | 方向 |
|---|---|---|---|
| BM-1 | `process_delayed_tasks` 超时清理、任务删除、任务挂起 —— 只摘链**不清位** | 位图残留陈旧位；唤醒侧 `ctz` 取到空分组，只能靠全链扫描兜底 | 位图**多**报 |
| BM-2 | `requeue_wait_lists` 改优先级时**无条件清旧位** | 同优先级仍有等待者时把**共享位**一并抹掉 | 位图**漏**报 |

**BM-1 真机证据**: `ST|FIX13|unrelated_sem_bitmap_intact|FAIL|sem_bitmap=8192`
—— 一次 5 tick 的信号量超时后 `wait_bitmap` 残留 bit13。

**BM-2 危害更严重**: 位图漏报时全链扫描**兜底不成立**（它只在"位图指向的优先级找不到节点"
时触发），会唤醒**错误的任务**（低优先级先于高优先级）。

**修复**: 新增 `rtos_internal_wait_remove_sync_bitmap()`（摘链后扫描剩余链，确认该优先级确实
无人等待才清位），在超时/删除/挂起三处使用；`requeue_wait_lists` 改为同样的判据。

### 3.5 BM-3: 阻塞期间被改优先级的等待者，不更新持有者的继承提升值

优先级继承只在"新等待者进入"与"等待者离开"两个时刻重新评估。若等待者在阻塞期间被
`rtos_task_set_priority` 改了优先级，持有者的提升值仍停留在旧优先级 → 中优先级任务继续插队，
**优先级继承要消除的反转又回来了**。
**修复**: 新增 `rtos_internal_mutex_waiter_requeued()`，由 `rtos_task_set_priority` 调用，
反查注册表找出该等待者所在的锁并重算持有者有效优先级。

> **修复该缺陷时当场触发一次 HardFault**（PC = `rtos_internal_mutex_waiter_requeued`,
> BFAR = 0x056884B5）。根因是新代码**无校验地正向遍历等待链**，遇到一个陈旧 `next` 指针
> 即解引用野地址。已补 `wait_obj` / `list_head` 归属校验。
> **教训**: 侵入式等待节点被四个模块共用，遍历前必须验证归属。

### 3.6 BI-1: `notify_wait` 超时后 `blocked_on` 哨兵残留 → 截断后续 delay

`rtos_task_notify` 用 `blocked_on == (void*)tcb` 作为"该任务正在 notify_wait 中"的**唯一判据**，
但超时路径从不清 `blocked_on`。于是任务超时后只要再调用普通 `rtos_task_delay()`，
任意一次外部 `notify` 都会把它从延时表里摘出来提前唤醒。
**修复**: 超时清理处清 `blocked_on`。回归 FIX16 实测 `delay_ticks=200`（修复前会被截断到个位数）。

### 3.7 BD-1: `delay(0x7FFFFFFF)` 被误钳为"永不唤醒"

钳制判据写作 `ticks > 0x7FFFFFFE`，但实际语义用的是 `sched_tick_after` 的**带符号差值**，
而 `0x7FFFFFFF` 恰在该域内。原式把它多钳一格 → `wake_tick = 0xFFFFFFFF` 且**不入延时表**
→ 没有任何路径能唤醒它。
**修复**: 判据改为 `> 0x7FFFFFFF`。回归 FIX17 实测 `wake_tick=2147585699`（= now + 0x7FFFFFFF）
且确认在延时链上。

### 3.8 BS-1 / TC-1

- **BS-1**: `rtos_sched_start` 索引 `ready_heads[top]` 缺 `top >= MAX_PRIORITIES` 上界判断
  （与另两个调用点不同口径），就绪位图为空时越界读。已补加固（并使该函数不再标 `noreturn`）。
- **TC-1**: 定时器回调内 `stop/delete` **自己**完全无效 —— 服务任务顺序是
  "摘链 → 跑回调 → 重插周期定时器"，回调执行时 `is_active` 已是 0，stop/delete 的
  "在链则摘"成为空操作，随后仍然重插。
  **修复**: 新增 `rtos_timer.deactivated` 一次性标记，由 `timer_reinsert_periodic` 消费。
  回归 FIX18 实测：20ms 周期定时器在第 3 次回调里自停，300ms 内 **fires 恒为 3**（修复前约 15）。

### 3.9 审计发现但**本轮未修**的项（诚实清单）

| # | 位置 | 问题 | 为何未修 |
|---|---|---|---|
| 1 | `rtos_task.c` notify_wait / 各 IPC 阻塞入口 | 调度器启动前调用会解引用 NULL `current_tcb`（`notify_wait` 连断言都没有，直接读写地址 0） | 需统一引入 `current_tcb == NULL → RTOS_ERR_SCHED` 守卫，涉及 7 个入口；现固件以 SKIP/INFO 记录该风险 |
| 2 | `rtos_mutex_take`（启动前） | 返回 `RTOS_OK` 但 `owner = NULL` —— **报成功却没拿到锁** | 同上级守卫一并处理 |
| 3 | `rtos_timer_delete` | 只入队即返回 OK；delete→create→start 同一对象时 DELETE 先被处理，**START 被静默丢弃**（两个 API 都返回 OK） | 需要同步 delete 语义（代次计数或阻塞等待命令消费） |
| 4 | `rtos_perf_on_task_created` | 注册表满（容量 `MAX_TASKS+2`=18）时**静默丢弃**；而栈溢出巡检只遍历该注册表 → 任务栈溢出**完全无检测**；且 `RTOS_CONFIG_MAX_TASKS` 在 create 路径从未被强制执行 | 需同时修注册表容量与 create 上限 |
| 5 | 优先级继承链式传递 | 单级继承，A→B→C 三层锁不传递（与 FreeRTOS 同） | 设计文档 L10 已承认；修它会影响热路径 |
| 6 | `rtos_types.h` | `wake_tick` 注释写"0=永不超时"，而代码用 `0xFFFFFFFF` 表示永久、0 是合法唤醒时刻 | 纯文档修正，随下次改动一并处理 |

---

## 4. 设计风险与限制（供选型决策）

| 编号 | 级别 | 内容 |
|---|---|---|
| R1 | 高 | 20 字最小栈的任务无法安全调用任何阻塞 API。**当前实现已把最小栈强校验提到 96 字**，但本次复核再次确认：96 字仅够"不阻塞 + 轻调用链"，含 FPU 惰性压栈（52 字）+ 阻塞调用链时必须 ≥128 字。建议按任务实际调用深度预留 |
| R2 | 高 | 堆元数据损坏零诊断：`rtos_heap_alloc` 扫描发现 `magic` 不符时仅 `break` 返回 NULL，无任何记录。建议加损坏计数器 |
| R3 | 中 | mempool 双重释放不检测（实测 `free` 两次均返回 OK，`free_count` 可达 5 > `block_count`=4，空闲链表静默损坏） |
| R4 | 中 | mempool 不校验存储区大小：8×3B 块配 24B 区域时内部按 4B 对齐需 32B，**静默越界写 8B** |
| R5 | 中 | ~~heap 大尺寸请求整数溢出~~ **已修复并已测**（FIX5） |
| R6 | 低 | 栈溢出检测延迟可达 ~256ms（idle 每 256 圈扫描一次），且"瞬时溢出后立即自删"的任务完全检不到 |
| R7 | 中 | 调度器启动前的危险 API 无统一防护（见 §3.9 第 1、2 项） |
| R8 | 低 | `event_wait` 错误路径不可区分：NULL/未 init/wait_bits=0/非法 mode/ISR+timeout 全部返回 0，与"当前位恰好为 0"无法区分。协议级限制 |
| R9 | 记录 | 已确认的语义设计（非缺陷）：队列满+`NO_WAIT` 返回 `ERR_TIMEOUT`（而非 `ERR_OVERFLOW`，后者专用于 sem 计数超限）；满队列阻塞 send 被打开空位后数据进**队尾**；`timer_delete` 后立即 `start` 返回 OK 但命令被丢弃（异步竞态）；mempool 空闲链为**头插 LIFO**；USART2 中断 NVIC 优先级 6（≥5），符合 BASEPRI(0x50) 约束 |

---

## 5. HIL 闭环实录：真机测试相对静态审查的增量价值

本节保留 3 次关键故障的定位过程，作为方法论留档。

### 5.1 完全静默 → RAM 取证定位

现象：烧录后串口 45 秒零输出，疑似启动失败。
取证：调试器读 RAM（`rtos_kernel` @0x20009650）→ `sched_state=1(RUNNING)`、
`tick_count=1190`（2 秒后不变）、`st_pass=89 / st_fail=1 / st_total=112`。
结论：固件正常运行了 1.19 秒、执行 112 用例后死机 —— **串口静默仅因输出早于监听窗口**。
改为"烧录与监听同脚本原子执行"后捕获完整故障报告：BusFault PRECISERR，BFAR=0xA5A5A5B9（栈水印值）。

### 5.2 20 字最小栈 → 堆静默损坏（R1 的完整故障链）

捕获输出显示 `create_returning FAIL ret=-2`（堆耗尽？）—— 但此时堆仅用 ~5KB/32KB。
死机点 BusFault 解引用 `0xA5A5A5B9`（BFAR），正是**栈水印 0xA5A5A5A5 + 0x15**。

推理链：20 字栈任务调用 `rtos_task_delete(NULL)` → 调用链深度 ~112B 溢出 80B 栈 →
溢出写覆盖**本任务栈缓冲底部的 heap 块 header** → `magic` 损坏 → 后续所有 `rtos_heap_alloc`
扫描到损坏点后 `break` 返回 NULL → 全部任务创建 `ERR_NO_MEM` → 测试代码用未初始化 TCB 指针
调用 delete → BusFault。

此链条横跨三个模块（task 栈溢出 → heap 元数据 → task create），
**任何单一模块的单元测试都无法发现**，真机黑盒测试直接捕获完整现场。

### 5.3 周期定时器只触发一次 → 内核修复

修复 5.2 的测试侧问题后，暴露出稳定的 3 项 FAIL：`periodic_multi cnt=1` /
`change_period_faster cnt=1` / `new_gap gap=-438`，而单次、重启、回调内自重启全部正常
—— 高度指向"纯周期无人打扰"场景。审查服务任务主循环确认 `timeout` 计算时序缺陷
（在重插之前算，此刻链表已空 → `WAIT_FOREVER`），移动一行代码修复，重跑全绿。

### 5.4 本轮（2026-09-17）的方法论教训

| # | 现象 | 根因 | 处置 |
|---|---|---|---|
| 1 | 变体切换后跑的还是上一个场景 | **`make` 不跟踪宏变化** | 切换 `-DFIRMWARE_*` 时必须删除 `main.o`；已在 `hil\build_fw.ps1` 加构建戳自动处理 |
| 2 | 判定"警告是否消失"得到假结论 | **CubeIDE 的 `make clean` 只删一部分 `.o`**（实测残留 59 个） | 必须手动清空 `Debug/**/*.o` 后重建 |
| 3 | 跨版本性能差 11% | **参与链接的目标文件集合不同**（`objects.list` 增删） | 两侧必须保证同一 `.o` 集合；已在性能报告 §6 写入比较纪律 |
| 4 | 清理内核时误删函数原型 / 压掉 7 行 | 按**行号**做文本替换，行号因前序编辑漂移 | 改为按**内容**定位 + 每次改完立即编译并做备份差异比对 |

---

## 6. 测试资产与复现

### 6.1 固件变体（`hil\build_fw.ps1`，全部靠 `-DFIRMWARE_*` 宏切换，不改源码）

| 变体 | 内容 |
|---|---|
| `daily` | 日常演示固件（应用任务 + HardFault 测试任务 + USB） |
| `selftest` | 全模块功能回归（`Core/Src/selftest.c`） |
| `bench` | BM0–BM7 微基准（`Core/Src/benchmark.c`） |
| `boundary` | 契约边界夹具（`hil\fw\Src\boundary_test.c`） |
| `scenario` / `scenario_usb` | 场景 S1–S5（带/不带 USB 应用任务） |
| `diag` | 审计取证场景 D1/D4/D5/D6 |

### 6.2 复现步骤

```powershell
# 全部套件（构建→烧录→抓串口，无需人工交互）
powershell -ExecutionPolicy Bypass -File hil\repro_all.ps1

# 单独跑
powershell -ExecutionPolicy Bypass -File hil\repro_all.ps1 -Only selftest,boundary

# 恢复工程到 CubeIDE 原始构建状态
powershell -ExecutionPolicy Bypass -File hil\restore_project.ps1 -Rebuild
```

- **判读结束行**: `ST|SUMMARY|pass=… fail=… info=… total=…|END`、`BT|SUMMARY|…`、`BM|DONE`、`SC|DONE`
- **ISR 矩阵需上位机配合**: 脚本检测到 `ST|ISR|ARM` 后自动回发一次 `X`（实测等待 170~180 ms）
- **断言失败会打印** `ST|ASSERT|TRAP|cond=… file=… line=…` 后停机；总线错误由工程自带 HardFault
  诊断打印完整现场（见 `hardfault_debug.md`）
- **原始日志**: `hil\logs\current\`（当前基线：`log_fix19_selftest.txt` = 442 用例全量、
  `log_organized_boundary.txt`、`log_organized_bench.txt`、`log_d10_fix.txt`）与 `hil\logs\archive\`（历史轮次）

### 6.3 已知的测试覆盖缺口

| 缺口 | 说明 |
|---|---|
| USB 子系统 | `selftest.c` 中 `usb` 零命中；三份报告均无 USB 条目。OSAL 26 个函数、CDC/MSC/HID 类 API、对象池耗尽、USB ISR 与 BASEPRI 交互全部未测 |
| 最坏关中断窗口（WCET） | BM0 的 23 周期是**空临界区对**，不是上界。heap alloc 持锁全程遍历、栈巡检、tick 处理的真实持锁时长从未测量；第二轮的量化尝试（D9）**方案作废**，见 §10.3 |
| 规模退化延伸到 16/32 任务 | 受 32 KB 堆限制（每任务约 2.2 KB），止于 8 个等待者 |
| 长时间稳态与真实 tick 回绕 | 最长用例是 20 s 的 S4；tick 回绕（49.7 天）从未真实跨越（历史 FIX1 为注入式仿真） |
| 堆碎片量化 | 只有"free 字节不下溢"断言，无"最大可分配块随碎片衰减"的曲线 |
| 每任务栈高水位实测表 | 只有 selftest 自身一个任务的栈水位数据 |
| 多定时器同 tick 到期 | 同时活跃定时器最多 2 个 |
| `RTOS_CONFIG_ASSERT_ENABLED=0` 发布配置回归 | 所有测试均在断言开启下运行 |

---

## 7. 结论

1. **功能正确性**: 两轮审计共修复 11 项缺陷后，14 个模块 442 用例 **431 PASS / 0 FAIL**；核心语义
   （阻塞/唤醒/移交/继承/精度）与设计文档一致。
2. **参数校验与上下文防护**: NULL / 非法参数 / 越界值防护完整，返回码与 `rtos_types.h` 枚举严格一致；
   ISR 禁止列表 100% 正确拦截（`ERR_ISR`）。
3. **健壮性短板**: 堆/内存池元数据损坏零诊断（R2/R3/R4）；最小栈阈值需要按调用深度评估（R1）；
   启动前危险 API 无统一防护（R7）；perf 注册表满后栈溢出监测静默失效（§3.9 第 4 项）。
4. **测试自身的方法论风险**已量化并写入比较纪律：宏变化不触发重编译、`make clean` 不彻底、
   链接输入差异带来 ~11% 性能偏差。**这三条是得到假结论的最主要来源**。

---

## 8. 源码清理的验收与回滚（2026-09-17 清理轮）

对 `RTOS/Core/**` 与 `RTOS/Port/GCC/ARM_CM7/port.c` 做了一次死代码清理与规范化，
判据是"编译期告警 + 全工程标识符引用计数 + 逐文件差异比对"三者交叉。

| 项 | 清理前 | 清理后 |
|---|---|---|
| 内核 `-Wall -Wextra` 告警 | 6 | **0** |
| `port.o` `.text` | 689 B | **641 B**（删除 PendSV 耗时诊断块与包装层） |
| `rtos_queue.o` `.text` | 1619 B | **1427 B**（四处重复块合并为 4 个 helper，−11.9%） |
| `rtos_sched.o` `.text` | 1704 B | 1712 B（`rtos_sched_block` 分支合并） |
| 日常固件 `.text` / `.bss` | 196036 / 125648 B | 195856 / **125600 B** |
| PendSV 反汇编 | 16 条指令 | **16 条，逐条一致** |

**删除项**: `port.c` 中 `#if 0` 的 PendSV 耗时统计（外部链接故编译器不告警，
靠引用计数才发现，且其插桩路径本身是坏的：包装函数把 live 值放进 r4，
被 naked 体 `ldmia {r4-r11}` 换掉，实测 BusFault）；未被访问的结构体成员
（`schedule_pending`、`total_run_count`、`notify_type`、内核对象的 `in_use` ——
USB OSAL 的池**确实**在用 `in_use`，未动）；空函数 `rtos_sched_start_first_task`。

**修正的代码/注释不符**: 三处把 `__builtin_ctz` 写成 "CLZ/`__builtin_clz`"；
`rtos_sem.h`/`rtos_queue.h` 的等待链语义；`rtos_internal.h` 的 `timeout=0` 契约
（实现不特判 0，会阻塞到下一 Tick）；`rtos_types.h` 的 `wake_tick` 注释。

**回滚点**: `hil\backup_precleanup\*.before`（11 个文件清理前原文）。

> 清理过程中**两次误删被工具链和回归抓住**：一次是把 `rtos_internal_block_current_on`
> 的原型连带删掉（编译器 `implicit declaration` 立即暴露）；一次是按行号覆盖时压掉
> `main.c` 的 7 行（含 `SystemClock_Config` 声明），表现为 `.text` 从 196036 骤降到 72396
> —— 异常的尺寸变化是发现它的线索。**"改完必编译 + 与备份比对"是这类编辑的必要护栏。**

---

## 9. 历史文档的不一致（已随本轮整合消除）

整合前的文档之间存在互相矛盾的数字，是误导的主要来源。记录于此以免重犯：

| 主题 | 曾经的多种说法 | 事实 |
|---|---|---|
| 用例总数 | 369（自测报告）/ 399（优化报告）/ 402（HANDOFF）/ 419（首轮）/ 442（当前） | 随代码增长而变；**唯一可信来源是 `ST\|SUMMARY` 行的实测输出** |
| `selftest.c` 行数 | "~1400 行" | 实际 2500+ 行 |
| 最小栈阈值 | 建议 ≥128 / 实现强校验 96 / `rtos_config.h` 常量 128 | 三者曾互不相同；现实际拒绝边界为 96 字 |
| heap 大尺寸溢出（R5） | 自测报告写"未实测" | 早已修复且有 FIX5 用例 —— 报告未随修复更新 |
| 延时链复杂度 | 单向+摘除 O(n)（性能报告）/ 双向+O(1)（优化报告）/ 插入 O(n)（设计文档） | 只有设计文档准确：**插入 O(n)、摘除 O(1)** |
| tick 回绕 | 自测报告记为"TIMER 模块全 PASS" | 实际是**注入式仿真**（直接改 `tick_count`），从未真实跨越回绕点 |
---

## 10. 第二轮内核审计（2026-09-17，源码通读 + 板级验证）

对 `RTOS/Core/**` 与 `RTOS/Port/` 做了一次完整通读（约 8000 行），产出两类结果：
**代码级确认的缺陷**与**已上板验证/已验证失败**的测量结论。本节只写有证据的条目。

### 10.1 【已修复】`rtos_sched_suspend()` 之后的任务阻塞会**永久挂起**（守卫 SD-1 + FIX19）

**现象**：调用 `rtos_sched_suspend()` 之后再调用 `rtos_task_delay(N)`，该 delay **永不返回**
（诊断场景 D10 实测卡死在该调用上，串口再无输出）。

**根因（代码级确定）**：
- `rtos_sched_tick()`（rtos_sched.c:554）在 `sched_state != RTOS_SCHED_RUNNING` 时**直接 return**，
  于是 `tick_count` 停止推进、`process_delayed_tasks()` 不再执行；
- `rtos_sched_suspend()` 只是把状态置为 `SUSPENDED`，**没有**阻止阻塞 API 把任务从就绪表摘除；
- 于是挂起期间阻塞的任务既不会被 tick 唤醒，也没有任何超时来源 ⇒ 永久阻塞，
  只有 `rtos_sched_resume()` 能把它放回就绪表。

**契约冲突**：改写前的 `rtos_sched.h` 契约写的是"挂起调度器(暂停 Tick 与抢占)**用于 ISR 内或临界
长流程中防止任务切换**"——按此用法（挂起→做一段不能被切换的工作→恢复）不会阻塞，所以
当前用法下不暴露；但"挂起后调用阻塞 API"这一组合是**静默永久挂起**，没有任何错误码或断言。

**次要问题**：`rtos_sched_schedule()`（rtos_sched.c:435）**不检查 `sched_state`**，
而 `rtos_task_delay` / `rtos_internal_block_current_on` / `rtos_task_notify_wait` 等
都会在阻塞后调用它。因此"挂起"并不能像文档暗示的那样阻止切换 —— 唯一的实际效果是
tick 停走（即上面那条更严重的后果）。

**建议**：二者之一必须改，且需明确契约：
1. 让 `rtos_sched_suspend()` 期间的阻塞 API 返回 `RTOS_ERR_SCHED`（拒绝服务，最安全）；或
2. 让挂起**只**暂停抢占决策而**不**停走 tick（`rtos_sched_tick` 仍推进 `tick_count` 与到期处理），
   并把文档改成"挂起期间不切换，但延时/超时照常计时"。

> **现状：已修复（2026-09-17，守卫 SD-1）**，采用方案 1（拒绝服务）。

#### 修复内容

新增统一守卫 `rtos_internal_block_permitted()`（`rtos_internal.h`，`static inline`，
只读 `sched_state`，不进临界区）。挂起期间（`sched_state != RTOS_SCHED_RUNNING`）的行为：

| API | 修复后行为 | 落点 |
|---|---|---|
| `rtos_task_delay` / `rtos_task_delay_until` | **不阻塞，立即返回**（void 接口无法报错误码，与同函数 `RTOS_PORT_IN_ISR()` 的处理一致） | `rtos_task.c` |
| `rtos_task_notify_wait` / `rtos_sem_take` / `rtos_mutex_take` / `rtos_queue_send` / `rtos_queue_recv` | 立即返回 **`RTOS_ERR_SCHED`(-9)** | `rtos_*.c` 各自阻塞前 |
| `rtos_event_wait` | 立即返回 **0**（返回位掩码，无法报错误码；沿用本模块既有的"错误路径不可区分"约定 R8） | `rtos_event.c` |
| 非阻塞路径（`NO_WAIT`、计数充足、队列未满…） | **不受影响** | — |

守卫插入位置经过逐处核对，有两个关键点：
- **必须在修改任何状态之前返回** —— 否则会留下半初始化的 `wait_node`（已入链但无人唤醒）；
- `rtos_mutex_take` 的守卫必须在 `mutex_try_inherit()` **之前** —— 否则会白白提升持锁者优先级，
  而恢复动作发生在释放路径上，释放者可能正是被永久阻塞的那个任务。

`rtos_sched.h` 的 `rtos_sched_suspend` 契约已改写，明确"挂起期间禁止任何会阻塞的 API"
以及与 `RTOS_PORT_ENTER_CRITICAL()` 的分工（临界区屏蔽中断、挂起影响调度与 tick）。

#### 上板验证（FIX19，12 条断言，全部 PASS）

用例位置：`Core/Src/selftest.c` 的 `test_fix_sched_suspend_block()`（并入 `test_hil_regressions()`）；
同一序列另有独立取证场景 `hil/fw/Src/diag_scenarios.c` 的 D10，其 `SC|D10|verdict` 判据是
`REACHED_END` —— **能执行到结尾本身就是最关键的一条证据**，修复前同样的调用序列会永久挂死。

```
ST|FIX19|state_suspended|PASS|
ST|FIX19|sem_take_refused|PASS|ret=-9 want=-9
ST|FIX19|queue_recv_refused|PASS|ret=-9
ST|FIX19|notify_wait_refused|PASS|ret=-9
ST|FIX19|event_wait_refused|PASS|bits=0
ST|FIX19|queue_send_ok|PASS|ret=0 want=0
ST|FIX19|sem_take_nowait_timeout|PASS|ret=-3 want=-3
ST|FIX19|sem_give_ok|PASS|ret=0 want=0
ST|FIX19|sem_take_count_ok|PASS|ret=0 want=0
ST|FIX19|tick_frozen|PASS|delta=0
ST|FIX19|state_resumed|PASS|
ST|FIX19|sem_take_works_after_resume|PASS|ret=-3
```

#### 回归结果

| 套件 | 结果 |
|---|---|
| `selftest` 全量 | **431 PASS / 0 FAIL / 11 INFO / 442**（此前 419/0/11/430，新增 FIX19 的 12 条） |
| `boundary_test` | **103 PASS / 0 FAIL** |
| 微基准 BM0–BM7 | 与基线一致（BM0=23、BM1=191、BM6=168000 逐位相同；BM2/BM3/BM4/BM7 在 ±7% 布局敏感区间内） |
| 场景 S1–S5 | 全部 status=0；S2 give 92/360/361/361（1→8 等待者仍**不退化**）、S4 周期 335983 周期 = 1999.99 µs |
| 内核 `-Wall -Wextra -Wcomment` | **0 警告** |

### 10.2 【代码级确认，未上板】其余缺陷（按严重度）

| # | 位置 | 问题 | 影响 |
|---|---|---|---|
| A2 | `rtos_task.c:461-499` + `rtos_heap.c:299-338` | `rtos_task_create` 先调 `rtos_heap_get_max_alloc()` 校验栈尺寸，再**非原子地**分配 TCB（吃掉同一块里的 136+8 字节），最后才分配栈 | 碎片化时"恰好等于上界"的请求会返回 `ERR_NO_MEM`，破坏 `rtos_heap.h:73-74` 承诺的前置条件（历史"假上界"缺陷的残留形态） |
| A5 | `rtos_queue.c:142`（直传 memcpy）+ `268-269`/`301-302`（阻塞接收者登记 `transfer_buf`/`transfer_size`）+ `28-45`（`rtos_queue_init` 无条件 `memset`、不检查 `is_initialized`/等待者） | 零拷贝直传用 `memcpy(recv_buf, item, queue->item_size)`，而 `recv_buf` 是**调用方按旧 `item_size` 提供的缓冲**；`transfer_size` 全程只写不读，起不到校验作用 | `queue_deinit` 后重新 `queue_init` 成更大的 `item_size`，可让已阻塞接收者的 32B 栈缓冲被写入 256B ⇒ **接收任务栈溢出** |
| A4 | `rtos_task.c:343` | `rtos_internal_wake_highest_bm` 的同优先级"还有别人吗"扫描**缺 `p->tcb != NULL` 守卫**，而同文件的 `wait_remove_sync_bitmap:202` 有 | 链上出现 `tcb == NULL` 的陈旧节点时，这是唯一会解引用它的循环（其余路径都已加守卫） |
| A7 | `rtos_port.h:127-130` | 注释自相矛盾且与 `port.c` 相反：line 127 写"基本 FPU 帧"、line 128 写"bit4=…(有 FPU)"，而 `port.c:343` 的语义是 **bit4=1(0xFFFFFFFD)=无 FPU 扩展帧** | 纯文档缺陷，但这是移植层契约文件，下一位移植者会把 FPU 分支反过来 |
| A3 | `rtos_timer.c:183-197` | 周期定时器重插用 `expire_tick = now + period`（当前 tick），**不从原 `expire_tick` 递推** | 回调耗时/阻塞会转化为**永久周期漂移**（不是 catch-up，也不是过载计数）；模块内无 `missed_count` |

### 10.3 【已撤回】最坏关中断窗口（WCET）的测量方案作废

这是既有审计列为首要缺口的指标。本轮建了 D9 场景尝试量化，**结论是方案作废、数字不可引用**。

**已经验证成立的前提**（每一步都有实测）：
- 临界区内 `BASEPRI = 0x50`（读回确认）；
- `TIM6` 的 NVIC 优先级字节 `0x40`（= 优先级 4，**不被 0x50 屏蔽**）；
- 对照临界区实测时长 `ctrl_hold_ns = 5 006 071`（5.0 ms，用 DWT 周期计数做边界，不受优化影响）；
- 该 5 ms 窗口内 `TIM6` ISR 进入次数 **= 0**（确认中断确实被屏蔽、会挂起）。

**失败点**：按上述前提，BASEPRI 清零后那次被延迟的中断应使相邻两次 ISR 的时间差暴增，
但实测 `late` 始终只有 **190 ns**（p99.9 也是 190 ns，基线 0）。即"延迟"没有在
`(dlt − nominal)` 里体现出来。

**最可能的根因（未验证）**：`TIM7` 是 16 位计数器，84 MHz 下每 **780 µs 回绕**；
5 ms 的延迟在 16 位差值下回绕 26 次，取模相减后混叠成小数值。小延迟档同样未捕捉到，
说明还可能存在第二个问题（本次未能定位）。

**要拿到 WCET 上界，必须换方案**（任选其一）：
1. **32 位时基**测单次"应到 vs 实到"延迟；
2. **GPIO 翻转 + 逻辑分析仪**直接量 BASEPRI 高电平时长（最直接、无软件口径风险）；
3. 统计"被延迟中断"的**计数差**而非时间差（不依赖计数器位宽）。

> **因此"本内核的最坏关中断窗口"仍然是一个没有可信数字的缺口。**
> 既有性能报告中 BM0 的 23 周期只是**空临界区对**，不是上界，这一点仍然成立。

### 10.4 待办（按已验证程度排序）

| 优先级 | 事项 | 状态 |
|---|---|---|
| 1 | ~~`rtos_sched_suspend` 语义修复 + 补回归用例~~ | ✅ **已完成**（守卫 SD-1 + FIX19，431/0/442） |
| 2 | `rtos_queue_init` 加"存在等待者/已初始化"检查；零拷贝直传按 `transfer_size` 校验长度 | 代码级确认，未修 |
| 3 | `wake_highest_bm` 补 `tcb != NULL` 守卫（与同文件另一处对齐） | 代码级确认，未修 |
| 4 | 修正 `rtos_port.h` 的 EXC_RETURN 注释（bit4 语义写反了） | 代码级确认，未修 |
| 5 | 周期定时器改"从原 expire_tick 递推 + 过载计数" | 代码级确认，未修 |
| 6 | WCET 上界：换 32 位时基或 GPIO+逻辑分析仪重做 | 旧方案**已作废** |
| 7 | 优化项：`rtos_perf_get_all_tasks` 的栈水位扫描移出临界区（快照 TCB 后在锁外扫描） | 代码级分析，**未实测**，收益待 D9 修复后验证 |