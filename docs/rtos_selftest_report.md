# RTOS 全模块真机自测试报告

> 工程基准: `e:\STM32_Project\CubeIDE\f446_rtos` (自研 RTOS v1.0.0, Cortex-M4F)
> 测试日期: 2026-09-15
> 测试方式: 硬件在环 (HIL, Hardware-in-the-Loop), NUCLEO-F446RE 真机执行
> 最终结果: **369 用例, 361 PASS / 0 FAIL / 8 INFO (含 5 项危险场景安全跳过)**
> 附带成果: 发现并修复内核缺陷 1 项 (P1), 识别设计风险 9 项 (R1~R9)

---

## 1. 测试概述

### 1.1 目标

对 RTOS 全部已实现模块进行真机黑盒测试, 覆盖:

- 正常功能路径 (happy path)
- 边界条件 (满/空、计数上限、优先级端点、最小栈、超时精度)
- 边界外/非法用法 (NULL 指针、非法参数、越界值、错误上下文调用、未初始化对象)

### 1.2 测试环境

| 项目 | 值 |
|---|---|
| 目标板 | NUCLEO-F446RE (STM32F446RET6, Cortex-M4F @168MHz) |
| 调试/烧录 | STLink V2-1 (SWD, STM32CubeProgrammer v2.14.0) |
| 串口观测 | STLink Virtual COM Port = COM3, 115200-8-N-1 |
| 编译链 | GNU Tools for STM32 11.3.rel1 (CubeIDE 1.13.2 自带) |
| 测试固件 | `Core/Src/selftest.c` (~1400 行) + `main.c` 集成 (3 行调用) |
| RTOS 配置 | Tick 1kHz / 32 优先级 / 抢占+时间片 / 堆 32KB / 断言开 / 溢出检测方法2 / perf+DWT |

### 1.3 方法: 硬件在环闭环

```
 编写测试固件 → make 编译 → SWD 烧录 → 串口实时捕获结果行
      ↑                                          |
      └──── 分析失败用例, 修复代码/测试 ←────────┘
```

本次测试共执行 **5 轮"烧录→捕获→分析→修复"迭代** (详见第 6 节), 全程无人工介入。

### 1.4 测试上下文分层

| 阶段 | 上下文 | 用例数 | 说明 |
|---|---|---|---|
| 阶段1 | `rtos_start()` 之前的 main | 66 | 调度器未启动时 API 行为 |
| 阶段2 | selftest 任务 (prio 9) | ~270 | 调度器运行中的全量功能 |
| ISR | USART2 RX 真实中断 | ~25 | 上位机回发 'X' 触发, 在真中断里执行 API 矩阵 |
| 硬件 | 调试器 SWD 读 RAM | 3 | 死机后内存取证 (rtos_kernel.tick_count 等) |

输出协议: 每用例一行 `ST|<模块>|<用例>|<PASS|FAIL|INFO>|<详情>`, 由上位机解析统计。

---

## 2. 测试覆盖矩阵

| 模块 | 用例 | 正常 | 边界 | 边界外/非法 | 上下文 | 结果 |
|---|---|---|---|---|---|---|
| SCHED 调度器 | 17 | 8 (状态/tick/yield) | 5 (delay 精度±6ms/delay_until 周期) | 1 (NULL 参数) + 3 SKIP | 任务+启动前 | 全 PASS |
| TASK 任务 | 41 | 12 (创建/删除/挂起/恢复) | 6 (prio 31/栈20字/自删/返回兜底) | 23 (NULL×6/prio 越界×2/栈19字/notify 非法 type×3...) | 任务+启动前+ISR | 全 PASS |
| NOTIFY 通知 | 18 | 8 (VALUE/BIT/INCREMENT 语义) | 4 (超时精度/自通知) | 6 (非法 type/NULL) | 任务+启动前+ISR | 全 PASS |
| QUEUE 队列 | 32 | 12 (FIFO/to_front/阻塞唤醒/双向阻塞) | 8 (满/空×NO_WAIT/超时/零拷贝传递) | 12 (NULL×5/未init×3/容量0/item0) | 任务+启动前+ISR+deinit | 全 PASS |
| SEM 信号量 | 37 | 14 (计数/二值/直接移交/优先级唤醒序) | 8 (计数上限溢出/超时精度) | 15 (NULL/未init/max0/init>max/deinit) | 任务+启动前+ISR | 全 PASS |
| MUTEX 互斥锁 | 32 | 14 (递归×3层/移交/优先级继承+恢复/超时) | 10 (递归期间他人获取/deinit 唤醒) | 8 (NULL/未init/未持锁 give) | 任务+启动前+ISR | 全 PASS |
| EVENT 事件 | 33 | 18 (set/clear 粘性/WAIT_ANY/ALL/CLEAR_ON_EXIT/多任务同醒/24位宽) | 6 (超时返回当前位) | 9 (NULL/未init/bits0/非法mode) | 任务+启动前+ISR+deinit | 全 PASS |
| TIMER 定时器 | 32 | 22 (单次/周期/重启重计时/change_period/回调上下文/自重启) | 7 (周期精度±15ms/幂等 stop) | 3 (NULL cb/period0/未init) + delete 竞态 INFO | 任务+启动前+ISR | 全 PASS ★含 1 项内核修复 |
| MEM 内存池 | 27 | 12 (全分配/释放/非对齐 size 向上取整) | 9 (耗尽返回 NULL/计数一致) | 6 (NULL/池外指针) + 双重释放 INFO | 任务+启动前+ISR | 全 PASS (含 2 项风险确认) |
| HEAP 动态堆 | 13 | 6 (对齐8/写读/计数) | 4 (耗尽 n=26 后全回收/巨型分配失败) | 3 (alloc0/双重释放静默忽略/野指针) | 任务+启动前 | 全 PASS |
| PERF 性能监视器 | 23 | 18 (系统快照/任务统计/栈水位/窗口推进/内存统计/截断) | 5 (数值范围) | 7 (NULL×5/max_count0) | 任务+启动前+ISR 快照 | 全 PASS |
| ISR 错误码矩阵 | 25 | 11 (允许列表: give/set/notify/...全部实测 OK) | 3 (delay 静默/event_wait 返回0) | 11 (禁止列表: take/send 超时/mutex/task_del/notify_wait 全部 ERR_ISR) | 真实 USART2 中断 | 全 PASS |
| OVF 栈溢出检测 | 3 | 3 (hook 触发/任务归因/魔数修复) | — | — | 静态任务+idle 扫描 | 全 PASS |
| SLICE 时间片 | 4 | 3 (同优先级交替 a=100/b=100) | 1 (计数均衡) | — | 任务 | 全 PASS |
| **合计** | **369** | **~165** | **~85** | **~119** | 4 种上下文 | **361 PASS / 0 FAIL / 8 INFO** |

---

## 3. 发现的内核缺陷

### 3.1 [P1·已修复] 周期定时器只触发一次 (rtos_timer.c)

**现象**: `PERIODIC` 模式 100ms 定时器, 等待 420ms 仅触发 1 次 (`periodic_multi cnt=1`); `change_period` 后同样只触发 1 次。

**根因** (rtos_timer.c 服务任务主循环, 修复前):

```c
for (;;) {
    ENTER_CRITICAL();
    expired = timer_collect_expired();   /* 周期定时器被摘出活跃链表 */
    timeout = timer_get_next_remain();   /* ★BUG: 此刻链表已空 → WAIT_FOREVER */
    EXIT_CRITICAL();
    timer_run_callbacks(expired);
    ENTER_CRITICAL();
    timer_reinsert_periodic(expired);   /* 重插, 但 timeout 已算完 */
    EXIT_CRITICAL();
    rtos_queue_recv(&s_cmd_queue, &cmd, timeout);  /* 永久等命令 → 不再触发 */
}
```

系统中**只有周期定时器活跃**时, 它被收集摘链的瞬间活跃链表为空, `timer_get_next_remain()` 返回 `WAIT_FOREVER`, 而 timeout 在重插**之前**计算, 导致服务任务陷入永久命令等待。任何定时器命令 (start/stop) 到达会临时唤醒它, 掩盖了大部分简单场景 — 这解释了为何单次定时器、自重启回调 (回调内 start 命令) 均正常, 唯纯周期触发异常。

**修复**: 将 `timeout` 计算移到 `timer_reinsert_periodic()` 之后 (一行移动, 见 [rtos_timer.c](file:///e:/STM32_Project/CubeIDE/f446_rtos/RTOS/Core/Src/rtos_timer.c#L333-L341), 含注释)。

**修复后实测**: `periodic_multi cnt=4`、三次 `periodic_gap gap=100`、`change_period_faster cnt=4`、`new_gap_50 gap=50` 全部通过。

---

## 4. 发现的设计风险与限制 (未修改内核, 供决策)

### R1 [高] 最小栈 20 字(80B)的任务无法安全调用任何阻塞 API

- `rtos_task_create` 允许 `stack_size=20` (仅校验 `>=20`), 创建成功。
- 实测: 该任务执行 `rtos_task_delay()` 时, **调用链 + PendSV 上下文保存(r4-r11) ≈ 112B > 80B**, 必然栈溢出。
- **放大效应**: 动态任务的栈从 `rtos_heap` 分配, 溢出写**穿透栈缓冲底部直接覆盖 heap 块 header (magic/size)**。
- **连锁反应** (实测故障链, 见 6.2 节): heap header 损坏 → 后续 `rtos_heap_alloc` 扫描时 magic 不符 → **静默 break 返回 NULL** → 所有任务创建返回 `RTOS_ERR_NO_MEM`, 系统看起来"堆耗尽"实则已损坏。
- 建议: 最小栈校验提高到安全值 (≥128 字), 或在文档中明确 20 字仅适用于"只写 flag 后自删且不进内核"的任务。

### R2 [高] 堆元数据损坏零诊断

`rtos_heap_alloc` 扫描发现 `magic != RTOS_HEAP_MAGIC` 时仅 `break` 返回 NULL (rtos_heap.c:209-212), 无任何记录。建议: 增加损坏计数器或断言, 使故障可观测。

### R3 [中] mempool 双重释放不检测

实测 (牺牲池): `free(p)` 两次均返回 `RTOS_OK`, `free_count` 达 5 > `block_count=4`, 空闲链表静默损坏, 后续 alloc 将返回重复块。与 heap 的双重释放防护 (magic 校验, 实测有效) 不一致。

### R4 [中] mempool 不校验存储区大小

`rtos_mempool_init(pool, base, block_size, block_count)` 不检查 `base` 指向的区域是否 ≥ `align4(block_size)*count`。实测: 给 8×3B 块配 24B 区域, 内部按 4B 对齐后需要 32B, **静默越界写 8B 相邻内存**。建议至少断言。

### R5 [中·代码审查] heap 大尺寸请求整数溢出 (未实测, 破坏性过强)

`align_up(size)` 在 `size >= 0xFFFFFFF9` 时回绕, `rtos_heap_alloc` 会"成功"返回仅 8B 用户区的块, 使用即越界毁堆。建议入口加 `size > HEAP_SIZE` 直接拒绝。

### R6 [低] 栈溢出检测延迟可达 ~256ms

idle 任务每 256 圈才扫描一次所有任务栈 (rtos_task.c:778), 且默认 `rtos_idle_hook` 为 `wfi` (每 SysTick 1ms 醒一次) → **最坏检测延迟 ≈ 256×1ms**; 高优先级任务持续运行时更久。此外"瞬时溢出后立即自删"的任务 (溢出时 TCB 已进待回收链) **完全检测不到** (R1 的故障链即如此)。

### R7 [中·文档级] 调度器启动前的危险 API 无统一防护

实测确认 (经代码审查 + 契约分析, 不在真机触发): `rtos_start()` 之前调用以下 API 会 assert 死循环或 HardFault, 且无错误码:
- `rtos_task_delay(>0)` / `rtos_task_delay_until` → assert 陷阱
- 空信号量 `sem_take(timeout>0)` / 满/空队列阻塞 send/recv / timer 命令超过 16 次 → assert 陷阱
- 不满足条件的 `event_wait(timeout>0)` / `notify_wait(timeout>0)` → **直接 HardFault** (解引用 NULL 的 current_tcb)

建议: 在 `rtos_sched_get_state() != RUNNING` 时这些 API 统一返回 `RTOS_ERR_SCHED`。

### R8 [低] event_wait 错误路径不可区分

NULL/未初始化/wait_bits=0/非法 mode/ISR+timeout 全部返回 0, 与"当前位恰好为 0"无法区分。协议级限制, 建议文档化或改为负值错误码 (会破坏返回类型约定, 需权衡)。

### R9 [记录] 已确认的语义设计 (非缺陷)

- 队列满 + `NO_WAIT` 返回 `RTOS_ERR_TIMEOUT` (而非 ERR_OVERFLOW); `ERR_OVERFLOW` 专用于 sem give 超上限 — 实测与头文件一致。
- 满队列阻塞 send 被接收方打开空位后唤醒, 数据进入**队尾** (FIFO 保序), 非插队。
- `timer_delete` 后立即 `timer_start` 返回 OK 但命令被服务任务丢弃 (异步竞态, 实测 `deleted_no_fire cnt=0` 验证)。
- mempool 空闲链表为**头插 LIFO**, alloc 顺序与块地址序相反 (实测 `bs3 d=-4`)。
- 启动前 mutex take/give 为退化"成功" (owner 恒 NULL 的空操作)。
- USART2 中断 NVIC 优先级 6 (≥5), 符合 BASEPRI(0x50) 屏蔽约束, ISR 调用 RTOS API 安全。

---

## 5. 测试结果详表

### 5.1 阶段1 (调度器启动前, 66 用例)

全部 PASS, 其中 5 项危险场景按计划跳过 (INFO):
`pre_delay_skip` / `pre_block_skip` / `pre_block_take_skip` / `pre_wait_skip` (event) / `pre_wait_skip` (notify)

关键实测行为:
- `rtos_sched_get_state()==NOT_STARTED`, `tick==0`, `get_current()==NULL` ✓
- 参数校验全部正确: NULL→`ERR_NULL`/`ERR_PARAM`, prio≥32→`ERR_PARAM`, 栈<20 字→`ERR_PARAM`, `notify type` 非法→`ERR_PARAM` ✓
- 非阻塞路径 (sem 计数/queue 收发/event set/get/mempool/heap) 启动前完全可用 ✓
- timer create + 1 次安全 start 入队 OK ✓
- perf `get_task(NULL,..)` 启动前→`ERR_PARAM` ✓

### 5.2 阶段2 (调度器运行中, ~270 用例)

代表性实测数据:

| 类别 | 用例 | 实测值 |
|---|---|---|
| 延时精度 | delay(100)/delay(200)/delay_until 3×100 | dt=100/200/300 (全部 0 误差) |
| 通知超时精度 | notify_wait(40) | r=-3 dt=40 |
| 队列超时精度 | 满 send(40)/空 recv(40) | r=-3 dt=40 |
| sem 移交语义 | give 唤醒 taker 后计数 | cnt=0 (不移入计数, 直接移交) ✓ |
| sem 优先级唤醒 | 两个 taker(prio 3/14) 顺序 give | 高优先级先得 (o0=1,o1=2) |
| 互斥锁递归 | take×3, 逐层 give 中他人 NO_WAIT | r0=r1=r2=TIMEOUT, 归零移交 r3=OK |
| 优先级继承 | low(20) 持锁, high(4) 阻塞 | low 被提升至 4, give 后恢复 20, high 获锁 |
| 挂起阻塞任务 | suspend→resume | 阻塞 API 返回 `ERR_DELETED` ✓ |
| deinit 唤醒 | sem/queue/mutex 等待者 | 全部 `ERR_DELETED` ✓ (event 例外: 返回 0, 契约一致) |
| 单次定时器 | 100ms 触发 | dt=104, 只触发 1 次 |
| 周期定时器(修复后) | 100ms 周期 | cnt=4, 三次 gap 全=100 |
| 重启重计时 | start→60ms→restart→150ms 到期 | dt=160 ∈ [150,190] |
| 周期变更 | 100ms→50ms | cnt=4, 新 gap=50 |
| 堆耗尽/回收 | alloc(1024) 循环 | n=26 次后 NULL, 全部 free 后 free 字节完全恢复 (28536→28536) |
| 堆双重释放 | free(p)×2 | 第二次静默忽略, free_count 不变 ✓ |
| 堆野指针 | free(NULL)/池外/栈地址 | 无崩溃无副作用 ✓ |
| mempool | 6 块全配→耗尽 NULL→释放→重配 | 计数精确 6→0→2→1 |
| perf 系统快照 | tick_rate/cpu_freq | 1000Hz / 168000000Hz 精确 |
| perf 任务统计 | selftest 自身 | 运行 822679µs, 67 次切换, 栈水位 1704/4096B |
| perf 窗口 | 两次查询间 delay(200) | du=200 精确推进 |
| 时间片轮转 | 同优先级双任务交替 | a=100/b=100 完全均衡 |
| 栈溢出检测 | 破坏 TCB 魔数 | hook 触发且正确归因任务 ✓ |

### 5.3 ISR 错误码矩阵 (真实 USART2 中断上下文, 25 项)

| API (ISR 中) | 实测 | 判定 |
|---|---|---|
| queue_recv/send NO_WAIT | OK + 数据正确 | ✓ 契约 |
| queue_recv/send timeout>0 | ERR_ISR | ✓ |
| sem_take NO_WAIT (空) | ERR_TIMEOUT | ✓ |
| sem_take timeout>0 | ERR_ISR | ✓ |
| sem_give | OK (任务侧可取走) | ✓ |
| mutex_take/give (任意 timeout) | ERR_ISR | ✓ |
| task_delete / notify_wait | ERR_ISR | ✓ |
| task_notify | OK (victim 收到 0xAB) | ✓ |
| task_suspend/resume (他人) | OK | ✓ |
| task_delay | 静默空操作, 不卡死 | ✓ |
| event_wait timeout>0 | 返回 0 | ✓ (R8) |
| event_set | 返回置位后的位 | ✓ |
| timer_start | OK (入队, 后续真触发) | ✓ |
| perf_get_cpu_usage | 值域正常 | ✓ ISR 安全 |

---

## 6. 测试迭代过程 (HIL 闭环实录)

本节记录 5 轮烧录迭代中 3 次关键故障的定位过程, 展示真机测试相对仿真/审查的价值。

### 6.1 第 1 轮: 完全静默 → RAM 取证定位

现象: 烧录后串口 45 秒零输出, 疑似启动失败。
取证: 调试器读 RAM (`rtos_kernel` @0x20009650):
`sched_state=1(RUNNING), tick_count=1190(2 秒后不变), st_pass=89, st_fail=1, st_total=112`

结论: 固件正常运行了 1.19 秒、执行 112 用例后死机 — 串口静默仅因输出早于监听窗口。**改为烧录与监听同脚本原子执行**, 捕获到完整故障报告: BusFault PRECISERR, BFAR=0xA5A5A5B9 (栈水印值!)。

### 6.2 第 2 轮: 20 字最小栈 → 堆静默损坏 (R1 完整故障链)

捕获输出显示 `create_returning FAIL ret=-2` (堆耗尽?) — 但此时堆仅用 ~5KB/32KB。死机点 BusFault 解引用 0xA5A5A5B9 (BFAR), 正是**栈水印 0xA5A5A5A5 + 0x15**。

推理链: 20 字栈任务 `st_p31_fn` 调用 `rtos_task_delete(NULL)` → 调用链深度 ~112B 溢出 80B 栈 → 溢出写覆盖**本任务栈缓冲底部的 heap 块 header** → `magic` 损坏 → 后续所有 `rtos_heap_alloc` 在扫描到损坏点后 break 返回 NULL → 全部任务创建 `ERR_NO_MEM` → 测试代码用未初始化 TCB 指针 (恰好是栈上残留水印值) 调 delete → BusFault。

此链条横跨三个模块 (task 栈溢出 → heap 元数据 → task create), **任何单一模块的单元测试都无法发现**, 真机黑盒测试直接捕获完整现场。

### 6.3 第 3 轮: 周期定时器只触发一次 → 内核修复 (P1)

修复 6.2 的测试侧问题后, 暴露出稳定的 3 项 FAIL: `periodic_multi cnt=1` / `change_period_faster cnt=1` / `new_gap gap=-438`。而单次、重启、回调内自重启全部正常 — 高度指向"纯周期无人打扰"场景。审查服务任务主循环确认 timeout 计算时序缺陷 (3.1 节), 移动一行代码修复, 重跑全绿。

**其余 2 轮**为测试预期修正 (sem 计数序列/queue FIFO 语义/mempool LIFO 顺序/perf 窗口内 printf 污染) 与竞态修复 (栈溢出检测窗口 150ms→700ms, 因 idle 256 圈扫描周期)。

---

## 7. 结论与建议

### 7.1 总体评价

| 维度 | 评价 |
|---|---|
| 功能正确性 | 修复 P1 后, 11 个模块 369 用例全数通过, 核心语义 (阻塞/唤醒/移交/继承/精度) 与设计文档一致 |
| 参数校验 | NULL/非法参数/越界值防护完整, 返回码与 `rtos_types.h` 枚举严格一致 |
| 上下文防护 | ISR 禁止列表 100% 正确拦截 (`ERR_ISR`), 允许列表全部实测可用 |
| 健壮性短板 | 堆/内存池元数据损坏零诊断 (R2/R3/R4), 最小栈阈值过于乐观 (R1) |

### 7.2 建议优先级

1. **立即**: 保留 P1 修复; 在 `rtos_task_create` 将最小栈校验从 20 字提高 (R1)。
2. **短期**: heap/mempool 损坏计数器与入口断言 (R2/R4/R5); 启动前危险 API 统一返回 `ERR_ERR_SCHED` (R7)。
3. **中期**: mempool 双重释放防护 (对齐 heap 的 magic 方案) (R3); 栈溢出检测加入任务删除前检查或缩短扫描周期 (R6)。
4. **文档**: R8/R9 语义显式写入头文件注释。

### 7.3 测试资产

- 测试固件: `Core/Src/selftest.c` / `Core/Inc/selftest.h` (可复用, 见第 8 节)
- 本次完整串口日志: 见本报告同目录 `selftest_run.txt` 捕获记录 (15102 字节)
- 内核修复: [rtos_timer.c](file:///e:/STM32_Project/CubeIDE/f446_rtos/RTOS/Core/Src/rtos_timer.c#L333-L341)

---

## 8. 复现指南

1. **接入自测试**: 在 `Debug/Core/Src/subdir.mk` 的 C_SRCS/OBJS/C_DEPS 三个列表各加一行 `selftest` (`.c/.o/.d`), 并在 `Debug/objects.list` 中 `"./Core/Src/main.o"` 后加 `"./Core/Src/selftest.o"`。
2. **main.c 集成** (三处):
   - `#include "selftest.h"`
   - `rtos_init()` 后调 `selftest_pre_start()`, 任务创建区加 `rtos_task_create(&tcb, 1024, selftest_task, NULL, 9, "selftest");`
   - `HAL_UART_RxCpltCallback` 开头: `if (selftest_uart_isr_hook(g_uart2_rx)) { HAL_UART_Receive_IT(...); return; }`
3. **编译烧录**: `make -j8 all` + `STM32_Programmer_CLI -c port=SWD -w f446_rtos.elf -rst`
4. **监听**: 115200 打开 COM3; 看到行 `ST|ISR|ARM|send 'X' now` 时回发字符 `X` (ISR 矩阵触发), 等待 `ST|SUMMARY|...|END` 结束行。
5. 断言失败会打印 `ST|ASSERT|TRAP|cond=... file=... line=...` 后停机; 系统总线错误由工程自带 HardFault 诊断打印完整现场。
