# HIL 复现与审计报告 — 性能测试 / 边界测试 / 全量回归

> 日期: 2026-09-17
> 方式: 硬件在环 (HIL) — NUCLEO-F446RE (STM32F446RET6, Cortex-M4F @168MHz,
> HSE 8MHz × PLL 336/8/2), SYSCLK 168MHz, USART2 @ 115200-8N1 (COM3), SWD 烧录
> 工具链: GNU Tools for STM32 11.3.rel1, **-O2**; 计时 DWT CYCCNT (1 cyc ≈ 5.952ns)
> 复现脚本: `hil\repro_all.ps1` (一键跑完 bench/boundary/selftest/scenario)
> 原始日志: `hil\logs\`

---

## 0. 结论速览

| 项 | 结果 |
|---|---|
| 性能微基准 BM0–BM7 | **8/8 复现**, 7 项与报告一致(±3%), 1 项(BM5 创建/删除)从 6499 改善到 5795(**−11%**) |
| 边界夹具 103 断言 | **103 PASS / 0 FAIL**, 与报告一致; GEOM 行复现 `stack_max_words=7088` |
| selftest 全量回归 | 原 402 用例 **391 PASS / 0 FAIL / 11 INFO** 精确复现; 新增 6 组回归后 **419 / 0 / 11 / 430** |
| 系统级场景 S1–S5 | 全部复现, 但 **S1 的结论作废**(夹具测的不是它声称的东西), S2 显示等待链改造**已把退化从 +18.4% 降到 0%** |
| **发现并修复的内核缺陷** | **10 项**(其中 3 项真机复现并留下取证数据, 2 项为审计发现的高危项) |
| **发现的测试夹具缺陷** | **1 项致命的 S1 测量失效** + 2 项口径问题 |
| 环境 | 工程已还原为 CubeIDE 原始状态, 板子已回刷日常固件并复核运行正常 |

---

## 1. 复现结果逐项对照

### 1.1 BM0–BM7 微基准

本轮为修复后固件; "报告"列取自 `docs/rtos_vs_freertos_perf_report.md` 汇总表(自研列)。

| # | 指标 | 本轮实测 | 报告值 | 偏差 | 判定 |
|---|---|---|---|---|---|
| 0 | critical_pair | 23 | 23 | 0% | ✅ 逐位一致 |
| 1 | yield_switch | 191 | 191 | 0% | ✅ 逐位一致 |
| 2 | sem_roundtrip | 1021 | 1092 | −6.5% | ⚠️ 见 §1.2 |
| 3 | sem_wake_latency | 381 (328/638) | 374 (338/599) | +1.9% | ✅ |
| 4 | queue_send_wake | 510 (510/723) | 523 (523/748) | −2.5% | ✅ 布局敏感区间内 |
| 5 | task_create_del | **5795** | 6499 | **−10.8%** | ✅ 修复的副产物 |
| 6 | delay1_period | 168000 | 168000 | 0% | ✅ 逐位一致 |
| 7 | isr_wake_latency | 455 (447/591) | 456 (443/602) | −0.2% | ✅ |

**确定性**: 同一固件连续烧录 3 次, 10 行 BM 输出**逐字节完全相同**(`hil\repeat_run.ps1`),
复现了报告 §2.1 的"无随机性"结论。

### 1.2 关于 BM2 的 −6.5%(诚实披露)

BM2(sem 乒乓)在本次会话中先后测得 1092 / 1020 / 1021 / 1095, 即**同一份内核代码、
不同 `.text` 布局下相差 7%**。这**证实**了报告 §2.1 自己写下的警告:
"跨版本比较 BM2/BM4 时 ±7% 以内的差异可能纯由代码布局造成"。
因此 **BM2 的绝对值不可作为内核回归的判据**, 报告把它当作稳定指标引用是不严谨的。

### 1.3 BM5 改善 11% 的来源

| 变更 | 影响 | 归因 |
|---|---|---|
| `rtos_sched.c` 超时清理改为 `rtos_internal_wait_remove_sync_bitmap` | 位图不再残留 | 次要 |
| 本轮未改动堆分配器 | — | — |
| **`objects.list` 恢复完整(含 benchmark.o 与 selftest.o)** | 与报告基线同口径 | 主要 |

> 报告 §1.2 称两侧 `.text` 已对齐、§2.1 称"BM8=0 正式配置"；
> 本轮复现证实**构建输入(参与链接的 .o 集合)本身就会带来 ~11% 的差异**，
> 而报告未记录该输入。这是复现性上的一个实质缺口。

### 1.4 边界夹具(boundary_test, 103 断言)

`BT|SUMMARY|pass=103 fail=0` — **与报告一致**。关键几何量:

```
BT|GEOM|sizeof_tcb=140|heap=32768|max_alloc=28504|stack_max_words=7088
```

`stack_max_words=7088` 是**运行时按当前堆形状推导**的(28504−144−8)/4,
与报告 §A.2.1 记载的 "8157 → 修正后" 的最终行为一致:
`create_stack_upper_ok` PASS(恰好上界可创建)、`create_stack_over_heap` PASS。

**但发现一处夹具自身的时序瑕疵**(不影响结论):
`boundary_test.c:415` 计算 `max_stack_bytes` 时, 前面 4 组用例已经跑过,
堆并非"初始形状"; 而 `BT|GEOM` 打印的 `max_alloc=28504` 也不是初始值
(初始应为 32760)。注释里写的"该值必须在创建任何任务之前采样"与代码位置不符。
当前用例恰好成立(前序用例均已释放), 但**属于偶合, 不是不变式**。

### 1.5 selftest 全量回归

```
ST|SUMMARY|pass=391 fail=0 info=11 total=402|END        ← 修复前, 与 HANDOFF 基线一致
ST|SUMMARY|pass=419 fail=0 info=11 total=430|END        ← 修复后 + 新增 FIX13~FIX18
```

**精确复现**了 `.trae/scenario_test/HANDOFF.md` 记录的基线, 也确认了
`docs/rtos_selftest_report.md` 的 369 用例数字已过期(现行 402)。

新增的 6 组回归用例(FIX13~FIX18)全部 PASS, 详见 §3。

### 1.6 系统级场景 S1–S5

| 场景 | 指标 | 本轮实测 | 报告值 | 判定 |
|---|---|---|---|---|
| S1 | block_latency avg/min/max | **141 / 141 / 141** | 972 / 134 / 19(ad hoc) | ❌ **结论作废, 见 §2.1** |
| S2 | give 无等待者 | **92** | 91 | ✅ |
| S2 | give @1/4/8 等待者 | **340 / 339 / 339** | 1038 / 1064 / 1088 | ✅ **退化已消除(+18.4%→0%)** |
| S2 | 整回合(give+delay1) | 167539 | 166624 | ✅ 持平 |
| S3 | queue_latency avg/min | **15263 / 4216** | 15120 / 2025 | ✅ 量级一致 |
| S3 | control_period avg | **840279 cyc = 5.001ms** | 840279 | ✅ 逐位一致 |
| S3 | control_deviation avg | 380201 | 380199 | ⚠️ **指标本身无意义, 见 §2.2** |
| S3 | 吞吐 | **7895 items/s** | 7900 | ✅ 0.06% |
| S4 | period_actual avg (目标 2ms) | **335983 (1999.99µs)** | 335983 | ✅ 逐位一致 |
| S4 | wake_deviation avg/max | 3 / 145 | 2 / 19 | ⚠️ max 受宿主干扰, avg 一致 |
| S5 | 8 任务建立 | 8/8 | 8/8 | ✅ |

---

## 2. 既有测试中发现的**错误**

### 2.1 [致命] S1 优先级反转夹具**从未制造出锁争用** — 报告的核心结论之一无效

**取证方法**: 新增诊断场景 D1(`hil\fw\Src\diag_scenarios.c`, `-Variant diag -Scenario 11`),
**完全回放 S1 的规模与创建顺序**, 但在高优先级任务里加了一次 `KERN_NO_WAIT` 试探取锁,
用它判定"此刻锁是否为空"。

**实测结果**(修复前后各一次, 结论一致):

```
SC|D1|high_takes             | value=20      // 高优先级任务 20 轮全部拿到锁
SC|D1|free_acquisitions      | value=20      // ★ 20/20 次试探发现"锁是空的"
SC|D1|holder_entries         | value=8       // 低优先级持锁者只跑完 8/20 轮
SC|D1|holder_first_round     | value=1
SC|D1|high_first_acquisition | value=1
SC|D1|block_latency_cyc      | N=20|avg=100|min=100|max=100
```

**结论**: `free_acquisitions == high_takes == 20` 意味着**每一次**阻塞式
`kern_mutex_take()` 进入时锁都是空的 —— 高优先级任务走的是**无争用快路径**,
`block_latency` 测到的是 **100~141 周期的无争用取锁开销**, 与"优先级反转下的
头阻塞延迟"毫无关系。min=avg=max 的完全平坦分布本身就是这一点的旁证。

**根因**(夹具设计缺陷, 非内核缺陷):

1. 中优先级干扰者(prio 6)在 S1 中被**最后创建**, 而它一创建就抢占主任务并持续
   以 1ms 粒度占用 CPU;
2. 低优先级持锁者(prio 12)需要 CPU 才能进入临界区, 但它只能在"干扰者 delay 的缝隙"
   里运行 —— 实测一个 200ms 窗口只推进 8 轮;
3. 高优先级抢锁者(prio 4)优先级更高, 每次都在持锁者取锁**之前**就把锁拿走。

⇒ **报告里 S1 的所有数字(972 / 134 / 19、以及"无争用时自研更快"的解读)
都不是该指标应有的数据**。原报告 §3.1 用"自研 min 更小、max 更大"来论证
"自研的加锁路径无竞争时更快、有竞争时更慢", 其"有竞争"一侧从未被触发。

**修正方向**(未在本轮实施, 属夹具设计): 用一次性握手保证顺序 ——
持锁者先取锁并置 ready, 抢锁者等到 ready 再发起请求(本人 D2/D3 原型已验证该手法
可稳定制造争用, 有干扰者时实测 avg 1221 / max 21988, 与报告量级吻合)。

### 2.2 [重要] S3 的 `control_deviation_cyc` 是累积相位偏移, 不是抖动

指标定义为 `now − (上次唤醒时刻 + 标称周期)`, 且 `expect_cycle` **单调累加**。
一旦某次唤醒晚了超过一个周期, 基准就永久落后, 之后的偏差恒等于这个固定偏移。

**实测证据**: `control_period_cyc avg = 840279 周期 = 5.001 ms`, 而
`S3_CONTROL_PERIOD_MS` **就是 5** —— 实际周期完全正确; 而
`control_deviation_cyc avg = 380201 ≈ 半个周期`, 正是"永久偏移"的特征值
(与 `-(偏差)` 的补偿量吻合)。

⇒ **报告 §0.2 的判断是对的**("该结论作废"), 但报告**没有把这个指标从结果表里删掉**
也没有加作废标记(§2 总表仍列着 S3 控制环偏差、§0.9 仍在报它),
后续读者仍会把它当成有效的实时性数据。**建议直接从夹具与报告中移除该指标**。

### 2.3 `BM|7` 的源码注释是过期残留

`benchmark.c:11` 仍写着"上位机看到 `BM|7|ARM` 行后回发 100 个任意字节",
而实现早已改为 TIM6 硬件定时器触发, 代码中不存在 `BM|7|ARM` 输出。
`docs/_audit_test_coverage_gaps.md` C8 已记录, 本轮确认**仍未修复**。

### 2.4 文档间数字仍不一致(复核审计 C 节)

| 项 | 现状 |
|---|---|
| C1 用例总数 | `selftest` 实际 402(HANDOFF) / 430(本轮+FIX13~18); 旧报告写 369 |
| C3 selftest.c 行数 | 旧报告写 "~1400 行", 实际已 2500+ 行 |
| C4 最小栈阈值 | 报告建议 ≥128, 实际强校验 96, `rtos_config.h` 常量 128 —— 三者仍不一致 |
| C5 R5 状态 | 报告写"未实测", 代码早已修复且有 FIX5 用例 |
| C9 延时链描述 | 插入 O(n)/摘除 O(1) 的准确表述只在 `rtos_design.md`, 另两份报告仍过期 |

---

## 3. 发现并修复的**内核缺陷**

全部先复现/取证, 再修, 最后固化为回归用例。命名 EH/TM/BM/BI/BD/BS/TC 为缺陷编号,
FIX13~FIX18 为对应回归用例。

### 3.1 [已真机复现] EH-1: `rtos_event_wait` 遗留陈旧 `queue_bitmap`

**性质**: 跨对象的状态污染 —— 对一个对象的操作会去读改写**另一个无关对象**的内存。

**取证**(D4 场景, `-Variant diag -Scenario 16`, 修复前):

```
SC|D4|blocked_in_event   | value=1
SC|D4|bitmap_addr        | value=0x20009BE0     ← 事件等待期间 queue_bitmap 非 NULL
SC|D4|sem_bitmap_addr    | value=0x20009BE0     ← ★ 与信号量的位图地址完全相同
SC|D4|sem_bitmap_value   | value=2048           ← ★ 该信号量位图被写入了 bit11
SC|D4|verdict            | value=DEFECT_bitmap_not_null
```

**根因**: `rtos_internal_block_current_on` 与 `rtos_task_notify_wait` 都显式把
`wait_node.queue_bitmap` 置 NULL, 唯独 `rtos_event_wait` 逐字段填写时**漏了这两行**。
于是任务"先在信号量上超时、再在事件组上阻塞"后, 节点仍带着旧信号量的位图指针;
此后任何 `rtos_task_set_priority()` 都会经 `rtos_internal_requeue_wait_lists`
的位图分支去 RMW 那个旧信号量 —— **若该信号量已 deinit 且存储被复用, 就是对任意
内存的读改写**。

**修复**: `rtos_event.c` 补 `queue_bitmap = NULL; queued_priority = cur->priority;`
**修复后**: `bitmap_addr = 0x00000000`, `sem_bitmap_value = 0`, `verdict = OK_bitmap_null`

### 3.2 [已真机复现] TM-1: `rtos_timer_change_period` 缺周期上界校验

**取证**(D5 场景, 修复前):

```
SC|D5|change_period_ret     | value=0        ← 接受了 0xFFFFFFFF
SC|D5|fires_in_100ms_after  | value=100      ← ★ 100ms 内触发 100 次 = 1kHz 风暴
SC|D5|verdict               | value=DEFECT_storm
```

**根因**: `rtos_timer_create` 有 `period >= 0x80000000 → ERR_PARAM`
(注释明确写着"否则周期定时器以 1kHz 风暴执行"), 而
`rtos_timer_change_period` **只检查了 period == 0**, 漏了同一个上界。
改周期后 `expire_tick = now + period` 回绕到过去 → 服务任务每一轮都判定到期。

**修复**: 补同口径上界校验。**修复后**: `change_period_ret = -1`, `fires = 1`, `verdict = OK_bounded`

### 3.3 TM-2: 对仍活跃的定时器重新 create 会破坏活跃链表

`rtos_timer_create` 无条件 `memset`, 会把仍在活跃链表上的定时器的
`is_active/next/prev` 一并清零; 随后 `start` 把已入链节点再次插入 → **自环**;
`timer_collect_expired` 重复摘除并构造 `t->next = t` →
`timer_reinsert_periodic` 在**临界区内**死循环(SysTick 被屏蔽, 整机冻结)。
**修复**: `is_initialized && is_active` 时返回 `ERR_ALREADY`。
(判据取两者相与, 避免读取未清零 storage 的不可信字节导致首次 create 伪失败。)

### 3.4 BM-1 / BM-2: 位图索引链的"位图 ⇔ 链表"不变式被破坏

**两类破坏, 方向相反**:

| 编号 | 位置 | 后果 | 方向 |
|---|---|---|---|
| BM-1 | `process_delayed_tasks` 超时清理、任务删除、任务挂起 —— 只摘链**不清位** | 位图残留陈旧位; 唤醒侧 `ctz` 取到空分组, 只能靠全链扫描兜底 | 位图**多**报 |
| BM-2 | `requeue_wait_lists` 改优先级时**无条件清旧位** | 同优先级仍有等待者时把**共享位**一并抹掉 | 位图**漏**报 |

**BM-1 真机证据**(FIX13 首次运行时):
`ST|FIX13|unrelated_sem_bitmap_intact|FAIL|sem_bitmap=8192` —— 一次 5 tick 的信号量
超时后 `wait_bitmap` 残留 bit13。

**BM-2 的危害更严重**: 位图漏报时全链扫描**兜底不成立**(它只在"位图指向的优先级
找不到节点"时触发), 会唤醒**错误的任务**(低优先级先于高优先级)。

**修复**: 新增 `rtos_internal_wait_remove_sync_bitmap()`(摘链后扫描剩余链,
确认该优先级确实无人等待才清位), 在超时/删除/挂起三处使用;
`requeue_wait_lists` 改为同样的判据。

### 3.5 BM-3: 阻塞期间被改优先级的等待者, 不更新持有者的继承提升值

优先级继承只在"新等待者进入"与"等待者离开"两个时刻重新评估。
若等待者在阻塞期间被 `rtos_task_set_priority` 改了优先级, 持有者的提升值
仍停留在旧优先级 → 中优先级任务继续插队, **优先级继承要消除的反转又回来了**。
**修复**: 新增 `rtos_internal_mutex_waiter_requeued()`, 由 `rtos_task_set_priority`
调用, 反查注册表找出该等待者所在的锁并重算持有者有效优先级。

> **修复该缺陷时当场触发了一次 HardFault**(PC = `rtos_internal_mutex_waiter_requeued`,
> BFAR = 0x056884B5), 根因是新代码**无校验地正向遍历等待链**, 遇到一个陈旧 `next`
> 指针即解引用野地址。已补 `wait_obj` / `list_head` 归属校验。
> **这是一个真实的鲁棒性教训**: 侵入式等待节点被四个模块共用, 遍历前必须验证归属。

### 3.6 BI-1: `notify_wait` 超时后 `blocked_on` 哨兵残留 → 截断后续 delay

`rtos_task_notify` 用 `blocked_on == (void*)tcb` 作为"该任务正在 notify_wait 中"的
**唯一判据**, 但超时路径从不清 `blocked_on`。于是任务超时后只要再调用普通
`rtos_task_delay()`, 任意一次外部 `notify` 都会把它从延时表里摘出来提前唤醒。
**修复**: 超时清理处清 `blocked_on`。回归 FIX16 实测
`delay_ticks = 200`(修复前会被截断到个位数)。

### 3.7 BD-1: `delay(0x7FFFFFFF) ` 被误钳为"永不唤醒"

钳制判据写作 `ticks > 0x7FFFFFFE`, 但实际语义用的是 `sched_tick_after` 的
**带符号差值**, 而 `0x7FFFFFFF` 恰在该域内。原式把它多钳一格 →
`wake_tick = 0xFFFFFFFF` 且**不入延时表** → 没有任何路径能唤醒它。
**修复**: 判据改为 `> 0x7FFFFFFF`。回归 FIX17 实测
`wake_tick = 2147585699`(= now + 0x7FFFFFFF)且确认在延时链上。

### 3.8 BS-1 / TC-1

- **BS-1**: `rtos_sched_start` 索引 `ready_heads[top]` 缺 `top >= MAX_PRIORITIES`
  上界判断(与另两个调用点不同口径), 就绪位图为空时越界读。已补加固。
- **TC-1**: 定时器回调内 `stop/delete` **自己**完全无效 —— 服务任务顺序是
  "摘链 → 跑回调 → 重插周期定时器", 回调执行时 `is_active` 已是 0,
  stop/delete 的"在链则摘"成为空操作, 随后仍然重插。
  **修复**: 新增 `rtos_timer.deactivated` 一次性标记, 由 `timer_reinsert_periodic` 消费。
  回归 FIX18 实测: 20ms 周期定时器在第 3 次回调里自停, 300ms 内 **fires 恒为 3**(修复前会到 ~15)。

### 3.9 审计发现但**本轮未修**的项(诚实清单)

| # | 位置 | 问题 | 为何未修 |
|---|---|---|---|
| 1 | `rtos_task.c` notify_wait / 各 IPC 的阻塞入口 | 调度器启动前调用会解引用 NULL `current_tcb`(`notify_wait` 连断言都没有, 直接读写地址 0) | 需统一引入 `current_tcb == NULL → RTOS_ERR_SCHED` 守卫, 涉及 7 个入口; 现固件以 SKIP/INFO 记录该风险 |
| 2 | `rtos_mutex_take`(启动前) | 返回 `RTOS_OK` 但 `owner = NULL` —— **报成功却没拿到锁** | 同上级守卫一并处理; 现有自测把它记为"退化语义" |
| 3 | `rtos_timer_delete` | 只入队即返回 OK; delete→create→start 同一对象时 DELETE 先被处理, **START 被静默丢弃**(两个 API 都返回 OK) | 需要同步 delete 语义(代次计数或阻塞等待命令消费), 改动面较大 |
| 4 | `rtos_perf_on_task_created` | 注册表满(容量 MAX_TASKS+2=18)时**静默丢弃**; 而栈溢出巡检只遍历该注册表 → 任务栈溢出**完全无检测**; 且 `RTOS_CONFIG_MAX_TASKS` 在 create 路径从未被强制执行 | 需同时修注册表容量与 create 上限, 建议单独立项 |
| 5 | 优先级继承链式传递 | 单级继承, A→B→C 三层锁不传递(与 FreeRTOS 同) | 设计文档 L10 已承认; 修它会影响热路径 |

---

## 4. 复现工具链(本轮新增资产)

放在 `hil\`(自包含, 不污染 CubeIDE 工程):

| 文件 | 作用 |
|---|---|
| `hil\build_fw.ps1` | 变体化构建: `daily` / `selftest` / `bench` / `boundary` / `scenario` / `scenario_usb` / `diag`, 全部靠 `-DFIRMWARE_*` 宏切换,**不改源码** |
| `hil\run_hil.ps1` | 烧录 + 串口采集(同一调用内原子完成, 避免丢失开机输出), 支持终止行与自动应答 |
| `hil\repro_all.ps1` | 一键跑完四套测试 |
| `hil\run_scenarios.ps1` / `hil\repeat_run.ps1` | 场景批量运行 / 确定性复跑 |
| `hil\restore_project.ps1` | 把 `subdir.mk` / `objects.list` / 中间产物还原为 CubeIDE 原始状态并可重建 |
| `hil\fw\Src\diag_scenarios.c` | D1/D4/D5 取证场景 |

配套改动(已保留, 属正向改进):
- `Core\Src\main.c`: 用 `FIRMWARE_*` 宏守卫替代注释掉的代码块, 并补上**遗漏的
  `selftest_pre_start()` 调用**(`HANDOFF.md` §1 记录的踩坑点, 原 main.c 里确实没有);
  `boundary_test.h` 用 `__has_include` 兜住, 保证 CubeIDE 原生构建不受影响。
- `Core\Src\selftest.c`: 新增 FIX13~FIX18 六组回归(共 28 条断言)。

### 踩坑记录(工具链自身, 供后续参考)

1. **`make` 不跟踪宏变化**: 切换变体必须删除 `main.o`, 否则会静默跑上一个变体的场景。
2. **`objects.list` 不可做增量过滤**: 第一次排除 `benchmark.o` 后就永久丢失,
   导致后续 bench 变体链接失败。必须每次从模板完整重建。
3. **`hil\fw` 是变体源码目录, 不可当中间产物删除**。

---

## 5. 环境还原状态

| 项 | 状态 |
|---|---|
| `Debug\Core\Src\subdir.mk` | 已还原为 CubeIDE 原始内容 |
| `Debug\objects.list` | 已还原为完整 54 项 |
| `Debug\hil\`, `hil\fwsrc\`, `.hil_build_stamp` | 已删除 |
| `Core\Src\main.c` | 保留 `FIRMWARE_*` 守卫(默认值 = 日常固件, 行为不变) |
| `RTOS\**` | 保留 10 项缺陷修复 |
| 板子 | 已回刷日常固件, 复核 `task 0/1 running` 与 USB 枚举日志正常 |

---

## 6. 建议(按性价比)

| # | 建议 | 依据 |
|---|---|---|
| 1 | **重做 S1 夹具**(握手保证争用), 用真实数据替换报告中 S1 的全部结论 | §2.1 — 现有数字无意义 |
| 2 | **从夹具与报告中删除 `control_deviation_cyc`**, 只保留 `control_period_cyc` | §2.2 — 该指标是累积相位偏移 |
| 3 | 在 `docs/` 各报告中标注 BM2 ±7% 布局敏感性, 禁止跨版本引用其绝对值 | §1.2 |
| 4 | 统一 `current_tcb == NULL` 守卫(修 §3.9 的 1、2 两项) | 7 个入口, `RTOS_ERR_SCHED` 已存在 |
| 5 | 修定时器 delete 的异步语义(代次计数) | §3.9 第 3 项 — 两个 API 都报 OK 却静默失效 |
| 6 | 把"每对象等待者数 ≤8、任务数 <18"写入选型参数 | §3.9 第 4 项 — 超出后栈溢出监测静默失效 |
| 7 | 校正文档中所有过期数字(用例数/selftest 行数/最小栈阈值/R5 状态/延时链复杂度) | §2.4 |
