# RTOS 测试覆盖度审计报告(缺口清单)

> 审计类型: **只读**审计, 未修改任何工程文件。
> 项目根: `E:\STM32_Project\CubeIDE\f446_rtos`
> 审计日期: 本次会话
> 证据来源(全部为已读文件, 引用格式 `文件:行号`):
>
> | 代号 | 文件 |
> |---|---|
> | [SR] | `docs\rtos_selftest_report.md` |
> | [PR] | `docs\rtos_vs_freertos_perf_report.md` |
> | [OR] | `docs\rtos_perf_optimization_report.md` |
> | [DD] | `docs\rtos_design.md` |
> | [HF] | `docs\hardfault_debug.md` |
> | [ST] | `Core\Src\selftest.c` |
> | [BM] | `Core\Src\benchmark.c` |
> | [MC] | `Core\Src\main.c` |
> | [TSK] | `RTOS\Core\Src\rtos_task.c` |
> | [SCH] | `RTOS\Core\Src\rtos_sched.c` |
> | [HEAP] | `RTOS\Core\Src\rtos_heap.c` |
> | [USB] | `RTOS\Core\Src\rtos_usb.c` |
> | [CFG] | `RTOS\Core\Inc\rtos_config.h` |
> | [H*] | `RTOS\Core\Inc\rtos_{task,sched,sem,mutex,queue,event,timer,mem,heap,perf,usb}.h` |
>
> 说明: 本次未读取 `docs\freertos_design.md`; 该文件虽存在于工程内, 但不属于本次指定证据集,
> 因此本报告**不引用**其内容(涉及"FreeRTOS 侧架构"的判断仅来自 [PR]/[OR] 的正文叙述)。
> 凡无法从已读文件确定的结论, 均显式标注"**not determinable from the files read**"。

---

## 0. 当前固件形态与测试可执行性前提(审计基线)

一个必须先说明的事实: 三份报告的数字都是在**改过 `main.c` 的临时固件形态**下取得的, 当前仓库
的默认形态**既不跑 selftest 也不跑 benchmark**。

| 事实 | 证据 |
|---|---|
| selftest 任务被注释掉 | `main.c:666` — `// rtos_task_create(&selftest_tcb, 1024, selftest_task, NULL, 9, "selftest");` |
| benchmark 任务被注释掉 | `main.c:667` — `// rtos_task_create(&bench_tcb, 1024, bench_task, NULL, 9, "bench");` |
| 当前实际创建的任务 | `main.c:643-657`: task0/6, task1/5, uart/1, algo/10, ft1~ft7/7, ft8_ovf/7(静态 128 字栈) — 共 11 个 |
| USB 应用任务同样被注释 | `main.c:661-663` (`usb_cdc`/`usb_msc`/`usb_hid` 全部 `//`) |
| USB 协议栈本身仍被初始化 | `main.c:640-641` — `rtos_usb_init()` + `rtos_usb_cdc_init(0, USB_OTG_FS)` |
| 静态任务池上限 16 | `rtos_config.h:64` `RTOS_CONFIG_MAX_TASKS (16U)`; `rtos_config.h:61` `USE_STATIC_ALLOCATION=1` |
| 复现指南要求手工切任务 | `rtos_perf_optimization_report.md:203` — "main.c:666 切换 `bench` 任务" / `:205` — "main.c:666 切换 `selftest` 任务" |

**推论(需人工确认, 非代码事实)**: 要重跑本报告建议的任何新测试, 都必须先手工打开
`main.c:666`/`:667` 之一或新增一行任务创建; 这意味着**当前固件无法一键回归**, 审计发现的所有
缺口在"默认构建"上都是不可执行的。

---

## SECTION A — 已覆盖内容(369 功能用例 + 8 性能基准)

### A.1 369 功能用例按主题分组

按模块计数直接取自 [SR] 的覆盖矩阵(`rtos_selftest_report.md:57-73`); "代码锚点"列给出该模块
测试函数在 [ST] 中的定义行, 用于核对"报告写的模块"与"代码里真实存在的模块"一致。

| 主题 | 用例数 [SR:57-73] | 测试函数([ST]) | 覆盖内容摘要(逐项均可在 [ST] 对应区间核对) |
|---|---|---|---|
| SCHED 调度器 | 17 | `test_sched` [ST:429] | state/tick 查询、delay(100/200) 精度 ±6、delay(0)=yield、delay_until 3×100、NULL 静默 |
| TASK 任务 | 41 | `test_task` [ST:477] | 创建/删除/自删/返回兜底、suspend+resume(阻塞 API 返回 ERR_DELETED)、set_priority 生效与抢占、prio31/128 字极限、NULL/prio≥32/栈<40 校验 |
| NOTIFY 通知 | 18 | `test_task` 内 [ST:497-525] | VALUE/BIT/INCREMENT 语义、NO_WAIT 空、超时 40ms 精度、非法 type |
| QUEUE 队列 | 32 | `test_queue` [ST:642] | FIFO、to_front 插队、满/空 NO_WAIT+超时、阻塞 recv 唤醒、阻塞 send 代办、deinit 唤醒、ISR/启动前路径 |
| SEM 信号量 | 37 | `test_sem` [ST:777] + `test_sem_priority_wake` [ST:869] | 计数/二值/上限溢出、直接移交(count 不变)、超时、deinit、双 taker 优先级唤醒序 |
| MUTEX 互斥锁 | 32 | `test_mutex` [ST:944] | 递归×3 层、移交、优先级继承(提升→恢复)、未持锁 give、他人持锁 NO_WAIT/超时、deinit |
| EVENT 事件 | 33 | `test_event` [ST:1064] | 粘性 set/clear、WAIT_ANY/ALL、CLEAR_ON_EXIT、24 位宽、一次 set 唤醒两个等待位、超时返回当前位、deinit 返回 0 |
| TIMER 定时器 | 32 | `test_timer` [ST:1237] | one-shot、restart 重计时、periodic 多触发+gap、change_period、回调上下文 queue send、回调自重启、delete 竞态(INFO) |
| MEM 内存池 | 27 | `test_mempool` [ST:1350] | 全分配/耗尽/释放/重分配、非对齐 bs=3 向上取整、越界 free、牺牲池验双重释放(INFO) |
| HEAP 动态堆 | 13 | `test_heap` [ST:1425] | 8B 对齐、耗尽 n≈26 后全回收、alloc(0)/巨型失败、双重释放忽略、野指针安全、计数器单调 |
| PERF 性能监视器 | 23 | `test_perf` [ST:1486] | 系统快照、窗口推进、任务统计、栈水位、内存统计、截断、ISR 快照 |
| ISR 错误码矩阵 | 25 | `test_isr_matrix` [ST:1569] + `selftest_uart_isr_hook` [ST:136] | 真 USART2 中断内 15 项 API 的允许/禁止矩阵 |
| OVF 栈溢出检测 | 3 | `test_stack_overflow` [ST:1659] | 破坏静态任务 `stack_magic` → hook 触发+归因 |
| SLICE 时间片 | 4 | `test_time_slice` [ST:1686] | 2 个同优先级任务各 100 次 yield 交替 |
| **修复验证(FIX1~FIX12)** | [OR:182] 称 30 项 | `test_fixes` [ST:2133] 调用 12 个子函数 [ST:2135-2146] | tick 回绕仿真、resume TOCTOU 3000 轮、event deinit UAF、FPU 96 字栈、heap 输入域、to_front 代办、heap 合并统计 500 轮、双重删除、多锁继承、等待链重排、mutex 注册表满、timer 回调自调用 |

**功能覆盖的上下文分层**([SR:44-49]): 启动前 main(66) / 任务(≈270) / 真 ISR(≈25) / SWD 读 RAM(3)。

### A.2 RTOS 机制的功能覆盖判定(逐机制, 无遗漏)

判定依据 = 该机制在 [H*] 中声明的公开 API 是否在 [ST] 中被调用。已用 grep 逐一核对
[ST] 全文的函数调用集合。

| 机制 | 公开 API 来源 | 功能覆盖 | 证据 |
|---|---|---|---|
| 任务创建/删除(动态+静态) | [Htask:99,116,127] | ✅ 全覆盖 | [ST:224-238] 参数校验; [ST:532,555,573,591,1665] 各创建路径; [ST:1950] `create_static` |
| task yield/delay/delay_until | [Htask:133,140,147] | ✅ | [ST:441,447,455,465,1399] |
| get_current / get_priority / set_priority | [Htask:150,153,156] | ✅ | [ST:488-495,622-628] |
| suspend / resume | [Htask:162,169] | ✅ | [ST:597-606,166-167,1811] |
| notify / notify_wait | [Htask:176,182] | ✅ | [ST:499-525,163,2210] |
| idle_hook / stack_overflow_hook / assert_fail | [Htask:185,191,197] | ✅(重定义) | [ST:59-73] |
| task_delete_hook | [Htask:194] | ❌ 未调用 | `rtos_config.h:53` `RTOS_CONFIG_USE_DELETE_HOOK=0`, [ST] 无引用 |
| sched get_state / get_tick_count / suspend / resume | [Hsched:161,164,155,158] | ⚠️ 部分 | 前两者 [ST:212,214,434,436] 覆盖; `rtos_sched_suspend/resume` **[ST] 中零调用** |
| sched 内部 API(add_ready/remove_ready/block/unblock/schedule/tick…) | [Hsched:114-170] | ✅(间接) | 均为内部路径, 由公开 API 驱动; 无直接单元测试 |
| sem init/take/give/get_count/deinit | [Hsem:39-61] | ✅ 全覆盖 | [ST:246-258,784-853,1578] |
| mutex init/take/give/deinit | [Hmutex:37-55] | ✅ 全覆盖 | [ST:284-287,950-1058,1579] |
| queue init/send/recv/get_count/get_free/deinit | [Hqueue:48-76] | ✅ 全覆盖 | [ST:263-279,650-771,1576] |
| event init/set/clear/wait/get/deinit | [Hevent:47-77] | ✅ 全覆盖 | [ST:293-303,1071-1187,1580] |
| timer create/start/stop/change_period/delete | [Htimer:58-71] | ✅ 全覆盖 | [ST:307-311,1246-1345,1581] |
| timer_init / timer_service_task | [Htimer:77,74] | ✅(间接) | 由 `rtos_init()` 拉起; [ST] 无直接调用 |
| mempool init/alloc/free/get_free | [Hmem:43-60] | ✅ 全覆盖 | [ST:318-325,1357-1420] |
| heap init/alloc/free/get_free/get_alloc_count/get_free_count | [Hheap:41-68] | ✅ 全覆盖 | [ST:328-335,1431-1481,1881-1882,1928-1935] |
| perf 全部查询 API | [Hperf:116-178] | ✅ 全覆盖 | [ST:341,1494-1564] |
| perf 钩子/内部 API(init_cycles/cycles_to_us/register_pool/on_*) | [Hperf:195-221] | ✅(间接) | 由内核调用; `rtos_perf_register_pool` 无直接调用(仅 [ST:1555] 间接验证 registered_pools≥1) |
| **USB OSAL 全部 API(26 个)** | [Husb:161-226] | ❌ **零覆盖** | [ST] 中 grep `usb`/`USB` **0 命中** |
| **USB 子系统 API(init/register_isr/irq_dispatch/malloc/free/msleep)** | [Husb:236-284] | ❌ **零覆盖** | 同上 |
| **USB 类封装(cdc/msc/hid 全部)** | [Husb:326-433] | ❌ **零覆盖** | 同上; 用户任务被注释 [MC:661-663] |

### A.3 8 项性能基准的覆盖判定

基准定义见 [BM:74-368] 与 [OR:23-32]; "负载规模"列是审计重点 —— **全部基准的并发任务数 ≤ 4**。

| # | 名称 | 代码 | 测量对象 | 循环数 | 并发任务数 | 有对手方? |
|---|---|---|---|---|---|---|
| BM0 | critical_pair | [BM:74] | 临界区 enter+exit 一对 | n=10000 | 0(纯开销) | — |
| BM1 | yield_switch | [BM:113] | 同优先级 yield 互切 | 20000 | 2 | 无(同优先级) |
| BM2 | sem_roundtrip | [BM:163] | sem 乒乓一回合 | 10000 | 2 | 无 |
| BM3 | sem_wake_latency | [BM:214] | 低优 give → 高优醒来 | 1000 | **2** | 无(无中优先级干扰) |
| BM4 | queue_send_wake | [BM:251] | send → 阻塞 recv 醒来 | 5000 | 2 | 无 |
| BM5 | task_create_del | [BM:277] | create+delete | 500 | 1 | 无 |
| BM6 | delay1_period | [BM:294] | delay(1) 实际周期 | 500 | 1 | 无(单任务延时) |
| BM7 | isr_wake_latency | [BM:338] | TIM6 ISR give → 高优醒来 | 100 | 1+1 ISR | 无 |

**性能覆盖的机制**: 只有 4 条路径被计时 —— 临界区、上下文切换、sem give/唤醒、queue 直传。
**完全无性能覆盖的机制**(在 [BM] 中零调用): mutex(含优先级继承)、event、timer(含服务任务)、
mempool、heap alloc/free 单独计时、task notify、suspend/resume、delay_until。

**已有性能指标的两项重要缺失**: ① 所有指标都是"单次操作"的均值/最值, 没有**持续时间/稳态**
维度的测量(总时长量级: BM1 = 2 万次 yield ≈ 数十 ms; 全部 8 项合计在秒级以内);
② 所有指标的最坏情况样本数有限(BM3 N=1000, BM7 N=100; [OR:68] 只报告了 max, 未报告优化后的 min)。

---

## SECTION B — 未覆盖的测试类别(核心交付物)

优先级标定口径: **HIGH = 一天 HIL 内可完成、且结论会改变"该内核能否用于目标场景"的判断**;
MEDIUM = 一天内可完成、结论用于定位与调优; LOW = 需要额外设施或收益偏文档完备性。
"文档是否承认"列严格区分三种状态: **已承认**(文档明确写了该风险)、**部分承认**(文档提到
相关机制但未把它标为风险)、**未承认**(文档未提及)。

### B.1 负载 / 竞争 / 持续运行(负载维度整体缺失)

**总体判定**: 现有 8 项基准的并发任务数上限是 4([BM:119,171,221,259,354] 每次最多创建 2 个
被测任务), 且每项只测"一次操作的往返"。没有任何测试在**多任务争用同一对象**、**长时间稳态**
或**中断与任务同时争用**的条件下测量。这是最大的单块缺口。

| # | 名称 | 为什么重要(能抓到什么真实故障) | 触发的代码路径 | 文档是否承认 | 优先级 |
|---|---|---|---|---|---|
| B1.1 | **等待链表插入规模退化** (n 个任务同时阻塞在同一 sem/mutex/queue/event) | 等待链表是**按优先级排序插入**([TSK:93-113], `while (cur != NULL && cur->tcb->priority <= node->tcb->priority)`), 每次新等待者都要从链首走到正确位置 → O(n)。n 个任务争用一把锁时, 第 n 个任务的 take 阻塞成本 ∝ n。真实症状: 大量任务争用少数同步对象时, **阻塞调用本身的耗时不恒定**, 表现为实时抖动而非功能故障, 功能测试**永远抓不到** | `rtos_internal_wait_insert_by_prio` [TSK:93]; 调用点 `rtos_internal_block_current` [TSK:163]; 被 sem/mutex/queue/event 四条 take 路径共享 | **已承认**([PR:198] "极端并发下自研的插入成本会随等待者数增长"), 但**从未测量** | **HIGH** |
| B1.2 | **延时表有序插入规模退化** | `delay_list_insert` [SCH:153-178] 每次 `rtos_task_delay()`/带超时阻塞都从 `delay_head` 线性找插入点 ([SCH:156,159]) → O(n), n = 当前所有在延时链上的任务数。**注意与 P-5 的区别**: [OR:124-136] 的 P-5 只优化了**摘除**(`delay_list_remove` [SCH:185-202] 改双向 O(1)), **插入仍是 O(n) 全链线性扫描**。真实症状: 几十个任务各自 delay/等待超时时, 每个 delay 调用的成本 ∝ 阻塞任务总数 | `delay_list_insert` [SCH:153] ← `rtos_sched_block` [Hsched:130] ← [TSK:490] `sched_block` | **部分承认**: [DD:1732] 复杂度表写"延时链插入 O(n) n=阻塞任务数 / 低频路径"; [PR:191] 表格写"到期查 O(1), 摘除 O(n)"(该行已过期, 见 SECTION C); [OR:136] 只声称"消除了规模退化"——**指摘除, 未覆盖插入** | **HIGH** |
| B1.3 | **多任务争用同一 mutex(非 2 个)** | 现有继承测试([ST:1020-1044])只有 1 个 low + 1 个 high; 多锁测试([ST:1998-2033])是 1 low + 2 high 各自 1 锁。没有"m 个 high 同时等 1 把锁"的场景 → `mutex_waiter_left` / `compute_effective_priority` 的**多等待者迭代路径**未被负载验证 | `rtos_mutex_take` [Hmutex:46]、`rtos_internal_mutex_waiter_left`(由 [SCH:238] 超时路径调用) | 未承认 | MEDIUM |
| B1.4 | **队列在持续灌入下丢/重/乱序** | 现有队列测试每项只收发 1~4 条([ST:653-751])。真实故障: 长时间高吞吐下环形缓冲索引(若为 `head/tail/count` 分离存储)在多次回绕后出现 off-by-one、覆盖未读数据。功能测试的 4 条消息**不可能触发缓冲回绕** | `rtos_queue_send/recv` [Hqueue:58,67]; 环形缓冲索引维护 | 未承认 | MEDIUM |
| B1.5 | **持续运行/稳态压力** | 无任何测试运行超过秒级: 最长的单个用例是 `test_stack_overflow` 的 `rtos_task_delay(1400)`([ST:1668])。真实故障: 只在数分钟~数小时后才显现的竞态、计数漂移、低频泄漏 | 全内核 | 未承认 | MEDIUM |

### B.2 可扩展性: 大量阻塞任务

| # | 名称 | 为什么重要 | 代码路径 | 文档是否承认 | 优先级 |
|---|---|---|---|---|---|
| B2.1 | **优先级排序等待链在满负载下的测量(≥8/16 个阻塞者)** | 见 B1.1。测法: 8/16 个不同优先级任务全部阻塞在同一 sem 上, 测 (a) 每次 take 的阻塞成本, (b) 一次 give 唤醒首个的延迟, (c) 全部 16 个依次被 give 唤醒的**总时长与最坏单次**。这直接量化 [PR:198] 只做了定性描述的那句话 | [TSK:93-113] + `rtos_internal_wake_highest` [TSK:174] | 已承认([PR:198]), 未测量 | **HIGH** |
| B2.2 | **延时链长度敏感性(8/16/32 个任务各自 delay)** | 见 B1.2。测法: 逐级增加同时 delay 的任务数, 测 `rtos_task_delay()` 的**调用开销**曲线。若 O(n) 成立, 会看到近似线性上升; FreeRTOS 侧的插入是 O(1)([PR:149] 自己写了"FreeRTOS 是 O(1) 双向"——该处同样只描述摘除, 见 SECTION C) | `delay_list_insert` [SCH:153] | 部分承认([DD:1732]) | **HIGH** |
| B2.3 | **任务池容量边界**: 创建到 `RTOS_CONFIG_MAX_TASKS`(=16, [CFG:64]) 耗尽 | 现测试最多同时存在 4~5 个任务。真实故障: 池满时的返回值、`rtos_perf` 注册表(`RTOS_CONFIG_PERF_TASK_REGISTRY_SIZE = MAX_TASKS+2` [CFG:128])恰好写满时的行为、栈溢出巡检数组 `overflowed[RTOS_CONFIG_PERF_TASK_REGISTRY_SIZE]`([TSK:807])的边界 | `rtos_task_create` 对象池分配; `rtos_perf_task_registry_count/at` [Hperf:162,168] | 未承认 | MEDIUM |
| B2.4 | **栈溢出巡检在任务数满载时的关中断时长** | `idle_count & 0xFF` 每 256 圈跑一次全注册表扫描([TSK:806-850]), 其中**整段在临界区内**([TSK:811 ENTER, :850 EXIT]), 遍历 `rtos_perf_task_registry_count()` 个任务。任务数越多, 这一次性关中断窗口越长。**没有任何测量** | [TSK:806-855] | 未承认 | MEDIUM |

### B.3 优先级反转: 行为与延迟(机制已测, 效果未测)

| # | 名称 | 为什么重要 | 代码路径 | 文档是否承认 | 优先级 |
|---|---|---|---|---|---|
| B3.1 | **中优先级干扰者下的高优先级阻塞时长** | 现有继承测试只断言**优先级数值**被提升到 4、释放后恢复 20([ST:1032-1040])——这是**机制**测试。它无法回答真实问题: "当存在一个中优先级(如 10)任务持续就绪时, 等锁的高优先级任务的**最坏等待时间**是多少?" 若继承实现正确, 中优先级任务应被 low 抢占; 若 `compute_effective_priority` 在多锁/多等待者下漏算, 中优先级任务会持续插队, 高优先级任务等待时间**可能从微秒级劣化到中优先级任务的执行时长**。这是 FreeRTOS 与自研**唯一可能产生数量级差异**的实时性指标之一 | `rtos_mutex_take` 继承路径 [Hmutex:46]; `rtos_mutex_give` 恢复路径 [Hmutex:52]; `rtos_internal_requeue_wait_lists` [TSK:544]; `rtos_task_set_priority` 与继承交互 ([DD:1039]) | **部分承认**: [DD:1056] "缺点/已知限制"提及; 但继承的**延迟效果**在任何报告中都无数据 | **HIGH** |
| B3.2 | **链式(嵌套)继承不传递的实际后果** | [DD:1753] L10 明确写"链式继承不传递, A→B→C 三层锁, 深嵌套反转残留, 业界通病"。现有多锁测试([ST:1998])是 **1 个任务持 2 把锁**, 不是 **3 个任务形成 A→B→C 链**。因此 L10 的后果从未被观察过 | 同上 + [TSK:544-582] | 已承认([DD:1753]) | MEDIUM |
| B3.3 | **继承恢复的抢占时序**: give 瞬间是否立即抢占中优先级任务 | 继承恢复发生在 `mutex_give` 内部; 若恢复后的 low 仍持有另一把锁([ST:2018] 已测数值), 抢占决策依赖 `rtos_sched_schedule`。测法: 用 DWT 测 give→high 首条指令的延迟, 并让中优先级任务处于忙循环 | `rtos_mutex_give` [Hmutex:52] + `rtos_sched_schedule` [Hsched:114] | 未承认 | MEDIUM |

### B.4 最坏情况关中断窗口 / tick 处理 WCET

| # | 名称 | 为什么重要 | 代码路径 | 文档是否承认 | 优先级 |
|---|---|---|---|---|---|
| B4.1 | **最坏情况临界区时长(全局 max 采样)** | BM0 测的是 `RTOS_PORT_ENTER_CRITICAL(); RTOS_PORT_EXIT_CRITICAL();` 的空对(23 cyc, [OR:42])——即**下界**, 不是上界。真实的内核临界区里干的是重活: heap alloc 全程持锁做 first-fit 全块遍历([HEAP:221-249], ENTER 在 221、EXIT 在 249, 中间是整个 `while` 扫描)、heap free 持锁做合并([HEAP:268-291])、tick 处理持锁做到期扫描([SCH:512-514] 包裹 `rtos_sched_tick`, 其内 `process_delayed_tasks` [SCH:211-245] 循环所有到期任务)。**测法**: 用一个**高优先级(0-4, 不被 BASEPRI 0x50 屏蔽)的定时器 ISR** 记录 SysTick 到期的实际延迟, 或用一个 GPIO 翻转 + 逻辑分析仪测 BASEPRI 拉高时长; 跑满负载(多次 alloc/free + 多任务到期)取 max。这是判定"内核能否用于 µs 级控制环"的**唯一硬指标** | [HEAP:221-249]、[HEAP:268-291]、[SCH:512-514]、[TSK:811-850]、[SCH:130-142] 插入扫描 | **未承认**: [OR:88] 只说"enter 的 isb 保留是安全取舍", 从未量化最长临界区; [DD:1721-1736] 复杂度表给了"关中断时间"列但 heap 写"O(n)"而非周期数 | **HIGH** |
| B4.2 | **SysTick handler WCET(到期任务数敏感性)** | `rtos_port_sys_tick_handler` [port.c:504] → 临界区 [port.c:512] → `rtos_sched_tick` [SCH:396]。tick 内需处理**所有**到期任务([SCH:215-244]), 每个到期任务都可能触发 `rtos_internal_mutex_waiter_left`([SCH:238])。BM6 只测了 1 个任务的 `delay(1)` 周期([BM:294-304])→ **tick 处理的 WCET 从未被测**。测法: 让 N 个任务在同一 tick 到期(N=16/32), 测该 tick 的 handler 时长与该 tick 内其他任务的唤醒滞后 | [SCH:396-441] + [SCH:211-245] | 部分承认: [DD:1733] "tick 处理 O(到期数)" | **HIGH** |
| B4.3 | **栈巡检临界区的时长**([TSK:811-850]) | 见 B2.4。这是**周期性**(每 256 次 idle 循环)出现的关中断尖峰, 且随任务数线性增长 | [TSK:806-855] | 未承认 | MEDIUM |
| B4.4 | **`rtos_sched_suspend/resume` 的 mask 时长** | 两个 API 在 [HST] 中零调用 → 从未在任何上下文被试过。`rtos_sched_resume` 内还会调 `rtos_sched_schedule`([SCH:455]), 即"恢复调度可能立即触发一次切换"路径完全未测 | [SCH:443-457] | 未承认 | LOW |

### B.5 长时间运行稳定性: tick 漂移, 回绕, 内存增长

| # | 名称 | 为什么重要 | 代码路径 | 文档是否承认 | 优先级 |
|---|---|---|---|---|---|
| B5.1 | **真实 tick 漂移累积** | BM6([BM:294-304])测 500 次 `delay(1)` 的**平均**周期(优化后 168000 cyc, [OR:48]), 即 ~0.5 秒窗口。50 万 tick(≈8 分钟)/4.3M tick(≈72 分钟)量级的**累积**漂移从未测(1kHz 下 100ppm 误差 = 8.6 ms/天, 只有长时间才可观测) | SysTick 配置 + `rtos_sched_tick` [SCH:396] | 未承认 | MEDIUM |
| B5.2 | **tick 回绕在真实运行中的验证** | FIX1 是**把 `rtos_kernel.tick_count` 直接赋值为 `0xFFFFFF00` 再观察**([ST:1737,1757])——这是注入式仿真, 且注入后立刻改回 100000([ST:1757])。**从未让 tick 真的跨过 0xFFFFFFFF-0x00000000 边界运行**, 也未验证回绕后延时链/定时器链的**有序性不变式**(链是按 `sched_tick_after` 带符号差值排序的 [SCH:159])。B5.1 与 B5.2 可以合并: 用真实的 49.7 天不可行, 但可以**把 tick 频率临时改为 100 kHz**(或把 SysTick 装载值调小)加速回绕——一天 HIL 内可完成 | `delay_list_insert` [SCH:153-178] 的 `sched_tick_after`; `process_delayed_tasks` [SCH:217] | **已承认**([DD:1744] L1), 但被 FIX1 的"仿真"覆盖度**高估**了([SR:66] 把它记为 TIMER 模块全 PASS) | **HIGH** |
| B5.3 | **堆/内存池在长时间运行下的计数与空间增长** | FIX7 做了 500 轮 alloc/free 并断言 `rtos_heap_get_free() <= HEAP_SIZE`([ST:1937])——这是**下溢**断言, 不是**碎片**测量。没有测试观察 alloc 次数增长后**可分配的最大块**如何衰减 | [HEAP:201-292] | 部分承认: [DD:1750] L7 "堆无后向合并 → 长期碎片" | MEDIUM |
| B5.4 | **DWT CYCCNT 32 位回绕**(25 秒 @168MHz)对 perf 统计的影响 | [Hperf:28-29] 自己写"32 位 DWT_CYCCNT @168MHz 约 25s 回绕一次, 无符号减法天然处理单次回绕(系统不可能 25s 无切换)"。这是**假设**, 未被测试。注意 `RTOS_CONFIG_PERF_HOTPATH_STATS=0`([CFG:146])时该路径本就不执行 | [Hperf:28-29]; `rtos_perf_on_context_switch` 等 | 已承认(作为假设写在头文件) | LOW |
| B5.5 | **长期运行下的对象池/注册表泄漏** | `test_perf` 断言"此刻存活任务 = 3"[ST:1543] —— 说明测试序列已经假定注册表被正确回收。但没有一个"创建/删除同一任务 10000 次 + 周期性检查注册表计数恒定"的测试。FIXR3 只测 1 次双重删除 | `process_pending_free` [TSK:53] + idle 回收; `rtos_perf_on_task_deleted` | 未承认 | MEDIUM |

### B.6 内存碎片(heap_4 vs first-fit + 前向合并)

| # | 名称 | 为什么重要 | 代码路径 | 文档是否承认 | 优先级 |
|---|---|---|---|---|---|
| B6.1 | **交错尺寸 alloc/free 后的最大可分配块** | 自研堆**只做前向合并**([HEAP:288-289] `block_merge_next`), 且 first-fit 扫描**包含已分配块**([HEAP:225-247], 注释在 [PR:161] 也确认)。真实故障: 交错 alloc/free 后**总空闲足够但无连续块**, 后续 `rtos_task_create`(需 2 次分配: TCB+栈)静默返回 `RTOS_ERR_NO_MEM`——这正是 [SR:114] 描述的故障链的**非栈溢出版本**, 且**无任何诊断**([SR:119] R2 已指出 heap 损坏零诊断)。测法: 与 FreeRTOS heap_4 同 seed 同序列(如 alloc 16/32/64/128 交错 + 随机 free)跑 10^4 轮, 每轮记录"能分配的最大块"和 alloc 失败率 | `rtos_heap_alloc` [HEAP:201-251]、`block_merge_next` [HEAP:288] | **已承认**([DD:1750] L7; [DD:1334] "只向高地址合并…L7"; [DD:1337] 提到任务删除释放顺序技巧在交错分配时失效), 但**从未测量** | **HIGH** |
| B6.2 | **分配失败的返回路径可用性(堆"看起来耗尽")** | 见 B6.1/[SR:114]: magic 不符时 `break` 返回 NULL([HEAP:226-229]), 无记录。测法: 在碎片化后统计 `rtos_heap_get_free()` 与"实际能 alloc 到的最大尺寸"的比值 —— 该比值就是**碎片率**, 一个可直接与 heap_4 对比的数字 | [HEAP:226-229] | 已承认([SR:119] R2) | MEDIUM |
| B6.3 | **mempool 双重释放的后果传播** | [SR:123] R3 实测"free 两次均返回 OK, free_count 达 5 > block_count=4", 测试代码在 [ST:1406-1420] 用**牺牲池**确认后**放弃该池**(注释 "pool abandoned")。即: **损坏已确认, 后果未量化**(后续 alloc 返回重复块的时机与后果未测) | `rtos_mempool_free` [Hmem:57] | 已承认([SR:123] R3) | LOW |
| B6.4 | **mempool 存储区尺寸不校验的越界写** | [SR:127] R4 实测"8×3B 块配 24B 区域 → 静默越界写 8B"。测试里用**足够大的 base2[8*4]** 绕开了这个场景([ST:1392])——即**越界写本身不在任何测试里被触发** | `rtos_mempool_init` [Hmem:43] | 已承认([SR:127] R4) | LOW |

### B.7 多服务集成 / 系统级场景

| # | 名称 | 为什么重要 | 代码路径 | 文档是否承认 | 优先级 |
|---|---|---|---|---|---|
| B7.1 | **系统级混合负载场景** | 现有最高级别的"集成"是 ISR 错误码矩阵([ST:1569-1636]), 它是**API 合法性检查**而非**负载场景**。没有任何测试同时运行: 周期性控制任务(如 `algo_task` [MC:646]) + 队列生产者/消费者 + 多定时器 + ISR 风暴 + 低优先级后台。真实故障: 只有多服务叠加时才出现的优先级配置错误、关中断尖峰的叠加、队列深度不足导致的偶发超时。**注意**: 当前默认固件形态([MC:643-657], 11 个任务)本身就是最接近的近似, 但**无任何断言/度量**, 只靠串口 printf 观察([MC:624-633]) | 全内核 + 应用任务 | 未承认 | **HIGH** |
| B7.2 | **selftest 与 benchmark 不可同时/连续运行** | 两者的任务创建位置互斥([MC:666] vs [MC:667] 只能开一个), 且 selftest 会**故意破坏** `rtos_kernel.tick_count`([ST:1737])、牺牲 mempool([ST:1408])、破坏 `stack_magic`([ST:1650])。没有任何"先跑 selftest 再跑 benchmark"的编排 → **功能回归与性能回归无法在同一次烧录中关联** | [MC:665-667] | 未承认(但 [OR:203-205] 的复现指南暴露了手工切换) | MEDIUM |
| B7.3 | **USB 与内核的交互场景** | 见 B8 | — | 未承认 | **HIGH** |

### B.8 USB 子系统

**总体判定**: **零测试覆盖, 且当前固件形态下 USB 处于"初始化但不使用"的半激活状态。**

| 事实 | 证据 |
|---|---|
| `selftest.c` 中 `usb`/`USB` 出现次数 | **0**(grep 全文无命中) |
| `benchmark.c` 中 USB 相关调用 | 无; `benchmark.c` 只 include `benchmark.h`/`rtos.h`/`main.h`([BM:13-16]) |
| USB 用户任务是否创建 | **否** — [MC:661-663] 三行全部注释 |
| USB 协议栈是否初始化 | **是** — [MC:640-641] |
| 三份报告中 USB 出现次数 | [SR]/[PR]/[OR] 全文无 USB 相关测试条目 |
| 设计文档对 USB 的篇幅 | [DD:1386-1716] 第十三章, 含 13.4.4 明确"当前链接真正消费的 OSAL 服务只有 6 个函数"([DD:1564]) |

| # | 名称 | 为什么重要 | 代码路径 | 文档是否承认 | 优先级 |
|---|---|---|---|---|---|
| B8.1 | **USB OSAL 层功能测试(26 个函数)** | 这 26 个函数是 CherryUSB 与内核之间的**唯一契约**([DD:1498] "OSAL 契约: 必须实现的 26 个函数")。零测试意味着: `usb_osal_thread_create` 的栈截断语义([Husb:196] "若超过…截断")、`usb_osal_mq_send/recv` 的 ISR 约束([Husb:188])、对象池耗尽的 NULL 返回——**全部未验证** | [Husb:161-226] 全部; 实现 [USB:266-536] | 未承认(文档只描述契约, 未声称已测) | **HIGH** |
| B8.2 | **USB 对象池耗尽** | 线程池仅 4([CFG:181]), 每个线程对象内嵌 512 字栈([CFG:187]) = **每个线程对象 ≥ 2KB RAM**; mq 池 4([CFG:179]), mq 内嵌 16 深度([CFG:194])。池满返回 NULL 的路径([Husb:161] 等)无测试 | `alloc_*_slot` [USB:132,146,160,174,188] | 未承认 | MEDIUM |
| B8.3 | **USB ISR 优先级与 BASEPRI 的交互** | [DD:1713] 明确写"USB IRQ 优先级 0~4 会导致随机 HardFault/调度器损坏, 规则是 IRQ 优先级 ≥5"; [SR:157] 也确认 USART2 用优先级 6。USB IRQ 实际配成 6([DD:1572] 提到 NVIC 优先级 6), 但**该配置在运行中未被任何测试验证**(无断言、无错误注入) | `rtos_usb_register_isr` [Husb:252]、`rtos_usb_irq_dispatch` [Husb:262]、[USB:225,254] | 已承认(作为"陷阱"写在 [DD:1704-1715]) | MEDIUM |
| B8.4 | **CDC 读写路径与 RTOS 原语的组合** | `rtos_usb_cdc_write` 内含 mutex + sem 的组合等待([USB:1130]), `hid_write` 同样([USB:1322-1377] 用 `hid_tx_mutex` + `hid_tx_sem`)。这些是**自研 mutex/sem 在真实驱动里的组合用法**, 却没有任何并发测试(多任务同时写 CDC/HID 的串行化行为)。[USB:1337-1339] 的注释明确说了无锁并发会"双写 DMA 缓冲/破坏在途传输/完成信号串号"——即**已知后果, 零测试** | [USB:1130-1314] (CDC), [USB:1322-1413] (HID), [USB:1246-1313] (flush/available) | 未承认 | **HIGH** |
| B8.5 | **MSC RAM 盘 32KB 的读/写一致性** | 32KB RAM 盘([CFG:211]) + 读写计数统计([USB:1511])。无"主机写入后读回比对"测试 | `usbd_msc_sector_read/write` [USB:1472,1487]、`rtos_usb_msc_get_stats` [USB:1511] | 未承认 | LOW |
| B8.6 | **USB 热插拔/断开重连** | [USB:1122] `rtos_usb_cdc_is_connected` 的 connected 状态机在断开时如何清理在途传输/等待者, 无测试 | [USB:764] event_handler, [USB:1122] | 未承认 | LOW |
| B8.7 | **`rtos_usb_malloc/free`(8KB 双档固定块池)的碎片与耗尽** | [CFG:219] `RTOS_CONFIG_USB_HEAP_SIZE = 8KB`, [CFG:216-218] 说明内部是"双档固定块池(≤64B/≤512B)"。**>512B 的请求行为**、池耗尽行为均未测 | `rtos_usb_malloc/free` [Husb:272,278]; [USB:509,523] | 未承认 | MEDIUM |

### B.9 每任务栈预算与等效任务集的 RAM 总成本

| # | 名称 | 为什么重要 | 代码路径 | 文档是否承认 | 优先级 |
|---|---|---|---|---|---|
| B9.1 | **每任务栈高水位实测表** | perf 已经能给出 `stack_used_bytes`([Hperf:91], 实测值见 [SR:199] "栈水位 1704/4096B"), 但**只测了 selftest 自己一个任务**。缺一张"每个任务在不同负载下的峰值栈用量"表 → 无法回答"这个任务实际需要 128 字还是 256 字"。真实故障: 栈分配过小 → 溢出(且按 [SR:133] R6, 检测可能延迟 256ms 甚至漏检) | `rtos_perf_get_task`/`get_all_tasks` [Hperf:149,170]; 水位填充 [CFG:136] | 部分承认([SR:135] R6 承认检测延迟) | **HIGH** |
| B9.2 | **等效任务集的 RAM 总成本对比(自研 vs FreeRTOS)** | 完全没有。可量化项: 每任务 TCB `sizeof(rtos_tcb_t)` + 栈; 静态池 `ready_heads[32]`([Hsched:46]); `RTOS_CONFIG_HEAP_SIZE = 32KB`([CFG:67])是**无条件静态数组**(与 FreeRTOS 侧 heap_4 的 15KB 对比见 [PR:33]); USB 子系统额外 RAM(线程池 4×512 字栈 [CFG:181,187] + mq 池 + 8KB USB 堆 [CFG:219] + 32KB RAM 盘 [CFG:211])——**这一项在 [DD:1769] 的脚注里被明确排除** | [Htask:33-77] (TCB), [CFG:67,181,187,211,219], [Hsched:46] | 部分承认: [DD:1769] 给出"Flash ~14KB / 静态 RAM ~70KB(不含 USB 子系统与 RAM 盘)", 但**未与 FreeRTOS 对比、未实测验证、未含 USB** | **HIGH** |
| B9.3 | **最小栈阈值的实测边界** | [CFG:44] `MINIMAL_STACK_SIZE=128`; [ST:1870-1874] FIX4 验证 FPU 任务 95 字被拒/96 字通过。但"**哪些调用组合在 96 字内安全**"没有系统性矩阵(如: 96 字 + 两级 mutex 嵌套 + ISR 打断 + FPU 惰性压栈的组合)。[SR:115] R1 建议"≥128 字", 当前实现是 96 字 —— **两者不一致**(见 SECTION C) | `rtos_task_create` 栈校验; [TSK:490] block 路径栈深 | 部分承认([SR:109-115] R1) | MEDIUM |
| B9.4 | **中断栈(MSP)与任务栈(PSP)的分离验证** | [HF:60-105] 详述硬件压栈帧, [HF:883] 的测试8 用 512B 静态栈验证真实溢出。但没有测试验证"深层嵌套 ISR 时 MSP 是否需要额外预算"(F446 的 MSP 目前由启动文件分配, 大小未在本次读取的文件中出现) | 启动文件(未读取) | 未承认 | LOW |

### B.10 定时器服务任务负载

| # | 名称 | 为什么重要 | 代码路径 | 文档是否承认 | 优先级 |
|---|---|---|---|---|---|
| B10.1 | **多个定时器同 tick 到期** | 测试中同时活跃的定时器最多 2 个([ST:1256-1329] 串行创建/停止)。服务任务是**单线程串行执行回调**([DD:1289] "回调串行排队, 长回调拖延后续所有定时器"), 且服务任务优先级 = `MAX_PRIORITIES-2` = **30**([CFG:90])——即**低于几乎所有应用任务**。测法: 8/16 个定时器同 tick 到期, 测**最后一个定时器回调的实际触发时刻与 Jitter** | `rtos_timer_service_task` [Htimer:74]; 主循环 [rtos_timer.c:332] | **已承认**([DD:1289] 明说缺点; [DD:1749] L6) | **HIGH** |
| B10.2 | **长回调放大 L6(到期检查延迟)** | [DD:1749] L6: "定时器 next_remain 在回调前计算 → 长回调导致到期检查延迟 = 回调时长"。测试中的回调都是"置一个 flag"([ST:1197-1235])或 20 次 start([ST:2102]), 时长微秒级 → **L6 从未被触发** | [rtos_timer.c] 服务循环 + `timer_get_next_remain` | 已承认([DD:1749] L6) | MEDIUM |
| B10.3 | **命令队列深度 16 的溢出行为** | [DD:1289] "命令队列 16 深度"; FIX-G6([ST:2098-2131])测的是**回调内自调用 20 次 start** 不死锁(就地执行)。但**任务侧**连续 20 次 `rtos_timer_start` 是否会阻塞/丢命令, 未测。真实故障: 短时间大量 timer start/stop 时, 调用方被阻塞 | `timer_send_cmd`; [rtos_timer.c:218,310,322] | 部分承认([DD:1289]) | MEDIUM |
| B10.4 | **tick 处理与定时器服务任务的交互延迟** | 定时器到期由服务任务轮询 `recv(timeout)` 实现([SR:95] 引用了原实现), 到期到回调执行的端到端延迟 = 服务任务被唤醒的调度延迟 + 队列延迟。测试只断言 ±15ms 容差([SR:66] "周期精度±15ms"), 未分解这 15ms 的来源 | [rtos_timer.c:332] | 未承认 | LOW |

### B.11 嵌套中断 / 相邻优先级抢占

| # | 名称 | 为什么重要 | 代码路径 | 文档是否承认 | 优先级 |
|---|---|---|---|---|---|
| B11.1 | **嵌套中断中调用 RTOS API** | ISR 矩阵只从 **1 个**中断(USART2, 优先级 6, [SR:157])触发; BM7 用 TIM6(优先级 6, [BM:357])。**没有"中断 A(优先级 5) 打断中断 B(优先级 6) 且两者都调 RTOS API"的测试**。临界区在 ISR 内是 `BASEPRI=0x50`([CFG:235]) → 优先级 5 也被屏蔽(5<<4 = 0x50, 屏蔽"数值 ≥ 5"的中断, 见 [CFG:231-235])——**即中断嵌套下 RTOS API 之间存在隐式串行化**, 其代价(优先级 5 中断被封的时长)从未测量 | `RTOS_PORT_ENTER_CRITICAL` 在 ISR 内路径 [rtos_port.h:56-94]; `RTOS_ASSERT_ISR_OK` [CFG:155-157] | 部分承认: [CFG:148-160] G-3 承认"优先级 <5 的 ISR 调用内核 API 会静默破坏"; 但**优先级 5 的封锁时长**未量化 | **HIGH** |
| B11.2 | **优先级 0-4 中断对内核的干扰(真实注入)** | [DD:251] L3 详细描述了"优先级 0~4 的 ISR 恰在 EXIT_CRITICAL→PendSV 之间唤醒刚阻塞任务 → 该任务延迟 ≤1 tick"。**该窗口从未被真实触发**(需要一个优先级 0-4 的 ISR 与一次 `rtos_task_delay` 精确对齐)。测法: 优先级 4 定时器 ISR 以随机相位 give 一个 sem, 统计唤醒延迟分布, 找是否有 ~1ms 的离群点 | [SCH:415-428] (tick 决策) + `rtos_sched_unblock` [Hsched:135] + [DD:251] | 已承认([DD:1746] L3; [DD:251]) | MEDIUM |
| B11.3 | **相邻优先级(preempt)精确验证** | 测试里的优先级差都很大(6 vs 20, 5 vs 10 等)。**相邻优先级(如 9 vs 10)的抢占与 `find_highest_priority` 位图边界**([Hsched:198] RBIT+CLZ)未单独验证。真实故障: 位图计算的 off-by-one 只在特定优先级组合下暴露 | `find_highest_priority`([DD:198] 描述, 实现在 [SCH]) | 未承认 | LOW |
| B11.4 | **同优先级 >2 个任务的时间片轮转** | `test_time_slice` 只有 2 个任务([ST:1692-1693])。`ready_list_rotate` 在多任务环上的公平性、以及 rotate 与 `next_ready` 字段复用(延时链也复用 `prev_ready`, [SCH:164-166])的交互, 未在多任务下验证 | `ready_list_rotate` [SCH:434] | 未承认 | MEDIUM |

### B.12 代码体积 / RAM 足迹对比

| # | 名称 | 为什么重要 | 证据/路径 | 文档是否承认 | 优先级 |
|---|---|---|---|---|---|
| B12.1 | **Flash 与静态 RAM 的实测对比(自研 vs FreeRTOS)** | 两个工程在同一台机器上(`f446_rtos` 与 `FreeRTOS`, 见 [PR:227]), 且都产出 ELF → `arm-none-eabi-size` 可直接对比。目前**唯一数字**是 [DD:1769] 的脚注("内核 ~3300 行 C + ~120 行汇编, Flash ~14KB / 静态 RAM ~70KB(不含 USB 子系统与 RAM 盘)"), 属于**估计值**, 未标注测量方法, 且**从未与 FreeRTOS 并排比较**。注意 [PR:33] 提到"自研 first-fit 32KB vs FreeRTOS heap_4 15KB"——这暗示自研的静态 RAM 至少多 17KB 的堆 | [DD:1769], [PR:33], [CFG:67] | 部分承认: 有估计数字, 无对比、无实测 | MEDIUM |
| B12.2 | **功能裁剪后的最小足迹** | [CFG] 提供 `USE_PERF_MONITOR`/`USE_USB`/`CHECK_FOR_STACK_OVERFLOW`/`ASSERT_ENABLED` 等开关, [CFG:118] 声称关闭 perf 后"零开销"、[CFG:166] 声称关闭 USB 后"零代码零 RAM"。**这些裁剪声明从未被编译验证过**(关闭开关后能否编译、能省多少, 无数据) | [CFG:98,101,119,146,167] | 部分承认(作为设计声明) | MEDIUM |

### B.13 文档已列为风险/限制但从未被实测的条目(逐条核对 — 这是"特殊关注"清单)

对 [DD] 第十五章 10 条设计债务([DD:1742-1753])与 [SR] 9 条风险(R1~R9, [SR:107-158])逐条判定
是否存在**真实机测量**。

| 条目 | 出处 | 描述 | 是否有实测 | 判定依据 |
|---|---|---|---|---|
| L1 | [DD:1744] | tick 回绕 49.7 天 | ⚠️ **仿真注入**(非真实回绕) | FIX1 直接赋值 `tick_count=0xFFFFFF00` [ST:1737] |
| L2 | [DD:1745] | 队列代办丢 to_front | ✅ 已修+已测 | FIX6 [ST:1895-1921] |
| L3 | [DD:1746] | 决策-执行窗口竞态(≤1 tick) | ❌ **无** | [ST] 无优先级 0-4 ISR 与阻塞对齐的测试 |
| L4 | [DD:1747] | 多锁 give 丢继承 | ✅ 已修+已测 | FIX-PI1 [ST:1998-2033] |
| L5 | [DD:1748]/[DD:1124] | 队列重试用完整原始超时 | ❌ **无** | [ST] 只测单次超时精度([ST:664,678]) |
| L6 | [DD:1749] | 定时器 next_remain 在回调前计算 | ❌ **无** | 回调全是微秒级 [ST:1197-1235] |
| L7 | [DD:1750]/[DD:1334] | 堆无后向合并 → 长期碎片 | ❌ **无测量** | FIX7 只断言计数不下溢 [ST:1937] |
| L8 | [DD:1751] | notify_wait 超时后 blocked_on 残留 | ❌ **无** | [ST] 无该断言 |
| L9 | [DD:1752] | 优先级≤32/名 16B/池 8 | ✅ 部分(池满已测) | FIX-G1 mutex 池满 [ST:2088]; sem/queue/event 池满未测 |
| L10 | [DD:1753] | 链式继承不传递 | ❌ **无** | 多锁测试是 1 任务持 2 锁, 非 A→B→C 三任务链 [ST:1998] |
| R1 | [SR:109] | 20 字最小栈不安全 | ✅ 实测(但**已过时**) | 见 SECTION C-4 |
| R2 | [SR:117] | heap 元数据损坏零诊断 | ✅ 实测(故障链) | [SR:237-243] |
| R3 | [SR:121] | mempool 双重释放不检测 | ✅ 实测 | [ST:1406-1420] |
| R4 | [SR:125] | mempool 不校验存储区大小 | ✅ 实测(越界 8B) | [SR:127]; 但测试侧已绕开 [ST:1392] |
| R5 | [SR:129] | heap 大尺寸整数溢出 | ✅ **已修+已测** | FIX5 [ST:1879-1883]; 修复在 [HEAP:207-212] |
| R6 | [SR:133] | 栈溢出检测延迟 ~256ms | ⚠️ 部分(测试窗口适配了它) | [ST:1650-1653] 注释"破坏窗口必须 > idle 扫描周期…窗口取 700ms" |
| R7 | [SR:137] | 启动前危险 API 无统一防护 | ❌ **仍未修, 且测试是 SKIP** | [ST:345-349] 五项 INFO 跳过; [SR:165-166] |
| R8 | [SR:146] | event_wait 错误路径不可区分 | ✅ 实测 | [ST:1103-1110] |
| R9 | [SR:150] | 已确认语义(6 项) | ✅ 逐项实测 | [SR:152-157] |

**汇总**: 19 条已承认风险中, **8 条(L3/L5/L6/L7/L8/L10/R7 + L1 的真实回绕)从未被真实机测量**;
其中 **L7(堆碎片)、L6(长回调)、L3(决策窗口)、L10(链式继承)** 属于会直接影响产品选型的实时性风险。

### B.14 其它零散缺口(不单列"为什么重要", 一行一条)

| # | 名称 | 证据 | 优先级 |
|---|---|---|---|
| B14.1 | `rtos_sched_suspend`/`resume` 零调用 | [Hsched:155,158] vs [ST] 全文无引用 | LOW |
| B14.2 | `rtos_task_delete_hook` 零调用(配置关闭) | [CFG:53] = 0 | LOW |
| B14.3 | `rtos_perf_get_switch_rate` / `rtos_perf_cycles_to_us` / `rtos_perf_task_at` 无直接测试 | [Hperf:185,201,168] | LOW |
| B14.4 | sem/queue/event 静态池耗尽(仅 mutex 测了) | [ST:2073] 仅 mutex; 池上限 [CFG:72,75,78,81] | MEDIUM |
| B14.5 | `rtos_heap_init` 无显式测试(仅隐式) | [Hheap:41] | LOW |
| B14.6 | `RTOS_CONFIG_ASSERT_ENABLED=0`(发布配置)下的行为从未测试 | [CFG:101]; 所有测试均在断言开启下运行([SR:30]) | MEDIUM |
| B14.7 | `RTOS_CONFIG_PERF_HOTPATH_STATS=1` 路径未回归 | [CFG:146] 默认 0; [OR:204] 要求手工改宏复测 | LOW |
| B14.8 | `RTOS_CONFIG_TICK_RATE_HZ` 非 1000 的配置未测 | [CFG:30] | LOW |
| B14.9 | `RTOS_CONFIG_GENERATE_RUN_TIME_STATS=0` 的裁剪组合未测 | [CFG:95] | LOW |

---

## SECTION C — 文档间不一致 / 过期数据

每条给出**双方引用**。"影响"列说明该不一致是否会导致误判。

| # | 主题 | 文档 A 的说法 | 文档 B / 代码的说法 | 影响 |
|---|---|---|---|---|
| C1 | **用例总数** | [SR:6] "369 用例, 361 PASS / 0 FAIL / 8 INFO"; [SR:73] 合计 369 | [OR:7] "399 用例全量回归 0 FAIL"; [OR:18] "399 用例"; [OR:182] "369 项原有… + 30 项修复验证用例" | 面值一致(369+30=399), 但 **FIX 用例实际有 42 项**: `test_fixes` [ST:2133-2147] 调 12 个子函数, 逐函数计数 ST_CHECK/st_expect 得 FIX1=6, FIX2=1, FIX3=1, FIX4=5, FIX5=2, FIX6=2, FIX7=1, FIXR3=3, FIXP1=5, FIXP2=1, FIXG1=2, FIXG6=1, 合计 **42** ≠ 30。**精确划分方式 not determinable from the files read**; 但可确定 [OR] 的"30"与代码不符 |
| C2 | **P1 缺陷状态** | [SR:79] "**[P1·已修复]** 周期定时器只触发一次"; [SR:103] "修复后实测…全部通过" | [OR] 全文未提及 P1; [OR:182] 的回归项列举为 "tick 回绕、resume 竞态、event UAF、多锁优先级继承等", 不含 P1 | 非直接冲突, 但**无法从 [OR] 确认 P1 修复在优化后内核中仍有效** —— 若 P1 修复被后续改动回退, 两份报告都不会显示矛盾 |
| C3 | **【自相矛盾】报告内两张表的用例数** | [SR:29] "测试固件 `Core/Src/selftest.c` (**~1400 行**)" | [ST] 实际 **2212 行**; [SR:51] 的 "~270" 是约数 | 文档内部自我矛盾(同一份报告内): [SR:29] 说 ~1400 行, 与文件实际 2212 行差距 58%。说明 [SR] 的行数与用例数描述**未随代码增长更新** |
| C4 | **最小栈阈值** | [SR:109-115] R1 "最小栈 20 字(80B) 的任务无法安全调用任何阻塞 API…**建议**: 最小栈校验提高到安全值 (**≥128 字**)" | 代码已改为 **96 字**: [ST:235-236] 注释 "[fix BUG-4] 最小栈 20→96 字"; FIX4 断言 95 被拒/96 通过 ([ST:1870-1871]); 但 [CFG:44] `RTOS_CONFIG_MINIMAL_STACK_SIZE (128U)`, 且 [SR:30] 配置表仍写 "栈 20 字" 的测试前提 | **双重不一致**: ① [SR] 报告的 R1 仍以 20 字为前提(已过期); ② [SR] 建议的 128 字与实际强制的 96 字([ST:1870-1871])以及配置常量 128([CFG:44])三者互不相同。**注**: [SR:115] 建议"≥128 字"对应 [CFG:44] 的 `MINIMAL_STACK_SIZE`, 但 FIX4 证明实际拒绝边界是 96 字 —— 说明 `rtos_task_create` 的校验常量**不是** `RTOS_CONFIG_MINIMAL_STACK_SIZE`(该函数的校验段位于 [TSK] 的 0~215 行区间, 本次未逐行读取) |
| C5 | **R5 状态: "未实测" vs 已修复且已测** | [SR:129] "R5 [中·代码审查] heap 大尺寸请求整数溢出 (**未实测**, 破坏性过强)" | 代码已加输入域校验 [HEAP:207-212]; FIX5 已实测两个边界值并断言 NULL [ST:1881-1882] | [SR] 第 4 节的风险清单**未随修复更新**, 第 7.2 节建议仍写 "heap…入口断言 (R2/R4/**R5**)"([SR:267]) —— 建议做已完成的事 |
| C6 | **R7 状态: 建议 vs 未实施** | [SR:144] 建议 "在 `rtos_sched_get_state() != RUNNING` 时这些 API 统一返回 `RTOS_ERR_SCHED`"; [SR:267] 列为"短期"项 | 测试侧仍是 5 条 SKIP INFO([ST:345-349]), 无任何返回值断言 | 不是文档冲突, 而是**文档建议与代码现状的差距从未被跟踪**。审计上应记为"开放风险"而非"已解决" |
| C7 | **性能报告的行号引用全部指向优化前代码** | [PR:102] 引 `port.c:362-387` 的 `rtos_port_save_and_switch` 调用; [PR:106] 引 `rtos_perf.c:135-182` | [OR:94] 明确该函数**已被删除**(P-2 改动); 优化后 [SCH:153] 是 `delay_list_insert`, 与 [PR:126] 引用的同一行号已不同; [SCH:469] 才是 `rtos_sched_context_switch`, 而 [PR:102] 称其在 429-453 | [PR] 的链接在优化后**全部失效或指错函数**。作为"优化前基线"文档可以接受, 但**没有任何交叉引用说明**, 读者按链接跳到优化后代码会读到无关内容 |
| C8 | **ISR 触发机制: 源码注释 vs 报告** | [BM:11] 注释: "ISR 测试: 上位机看到 `BM\|7\|ARM` 行后回发 **100 个任意字节**" | [PR:49] "**BM7 初版 UART 触发因上位机交互随机性废弃, 已由 TIM6 版替代**"; 实际代码用 TIM6 硬件定时器([BM:313-324,346-359]), **且代码中不存在 `BM\|7\|ARM` 输出行** | **源码注释是过期残留**。功能上无害, 但会误导复现者(按注释去等一个永远不会出现的行) |
| C9 | **延时链数据结构描述** | [OR:124-136] P-5: "延时表改**双向链表** (唤醒路径 O(n)→O(1))… 延时链表插入维护 `prev_ready`… `delay_list_remove` 从线性搜索前驱改为 O(1) 摘链" | 代码确认摘除已双向 O(1)([SCH:185-202]), 但**插入仍是有序线性扫描 O(n)**([SCH:153-178], 尤其 [SCH:156,159])。[PR:191] 的表格仍写 "延时结构: 升序**单向链表**: 到期查 O(1), 摘除 O(n)" | 三处表述不同: [PR:191] 说单向+摘除 O(n)(已过期); [OR] 说双向+O(1)(只对摘除成立); [DD:1732] 说"延时链插入 O(n) n=阻塞任务数"(唯一准确的一条)。**风险**: 读者据 [OR:136] "消除了规模退化" 会误以为延时链已完全 O(1), 实际插入仍是 O(n) |
| C10 | **BM3/BM7 抖动数据的完整性** | [PR:70] 抖动: "自研 BM3 抖动 243 cyc; 自研 max 更小 (731 vs 910)"; [PR:60,64] 给出优化前的 min/avg/max 三元组(BM3: 488 / 486 / 731) | [OR:68] 优化后只给 "BM3 max 540 (优化前 731), BM7 max 545 (前 604)" —— **未给优化后的 min**, 也未重算抖动 | 优化后的**抖动(max-avg = 540-307 = 233)** 与优化前(243)相比几乎不变, 而 [OR:66] 的结论是"最坏情况同步改善"。**抖动本身没有改善**, 只有 max 下降。这个结论在当前数据下**不支持**"最坏情况改善"的强度 |
| C11 | **FreeRTOS 侧堆大小对比被引用但无来源** | [PR:33] 配置表 "堆: 自研 first-fit **32KB** vs heap_4 **15KB**" | [CFG:67] `RTOS_CONFIG_HEAP_SIZE = 32U*1024U` ✅ 自研侧可核实; FreeRTOS 侧 15KB **来自另一个工程(`E:\STM32_Project\CubeIDE\FreeRTOS`), 本次未读取** | 自研侧数字正确; FreeRTOS 侧 15KB **not determinable from the files read**。且该 17KB 差异**未被计入任何 RAM 足迹对比**(见 B12.1) |
| C12 | **"399 用例 0 FAIL" 与 "8 INFO" 的口径** | [SR:6] 明确 "**361 PASS / 0 FAIL / 8 INFO**" (369 = 361+8) | [OR:7,182] 只写 "399 用例 0 FAIL", 未提 INFO 数量; [OR:183] 另提 "2 项 PERF 断言按 HOTPATH_STATS=0 语义变为 **INFO**" | 优化报告把 INFO 口径隐去了。由于 2 项 PERF 从断言降级为 INFO([OR:183]), "0 FAIL" 的**信息量下降**了 —— 这两项正是被 [OR:110-118] 关掉的统计功能。审计建议: 回归摘要必须同时报 PASS/FAIL/INFO |

---

## SECTION D — 建议新增的测试(针对 NUCLEO-F446RE 一天 HIL 的 FreeRTOS 对比, 按价值排序)

排序依据: ① 能否**证伪一个当前无数据的架构性风险**; ② 一天的 HIL 时间是否够;
③ 判别力(两个内核是否会给出**不同**的结论, 而非"都很好")。
"预期谁赢"仅依据已读的架构描述([PR:149,191,198]、[OR:124-136]、[SCH:153-202]、[TSK:93-113]),
不涉及未读的 FreeRTOS 源码。

| 排名 | 测试名 | 测量什么(一行) | 预期判别力(谁赢 + 为什么) |
|---|---|---|---|
| **1** | **SCALE-WAIT: N 个阻塞者的 take/give 成本曲线** | 对 N ∈ {2, 4, 8, 16, 32} 各跑一遍"N 个不同优先级任务阻塞在同一 sem 上, 依次 give 直到全部唤醒", 测每次 `sem_take` 阻塞成本、每次 give→唤醒延迟、以及**全部唤醒总时长**; 同时在 FreeRTOS 侧跑镜像代码 | **FreeRTOS 胜, 且差距随 N 超线性扩大**。自研的等待链表是优先级排序插入 O(n)([TSK:93-113]), 第 N 个等待者的 take 要扫过 N-1 个节点; FreeRTOS 尾插 O(1)([PR:198] 自述)。这是**唯一一条 [PR:198] 已定性承认但零数据的规模风险**, 也是本内核在"大量任务争用少数同步对象"场景下的选型生死线。附带产出: give 唤醒是否恒为 O(1)(自研取链首 [TSK:174], 应为 O(1)) |
| **2** | **SCALE-DELAY: 延时链长度对 `delay()` 开销的影响** | 让 N ∈ {1, 4, 8, 16} 个任务各自处于不同的 `wake_tick`(错开到不同 tick 到期), 在负载稳态下用 DWT 测第 N+1 个任务调用 `rtos_task_delay(t)` 的**纯开销**, 画开销-N 曲线 | **FreeRTOS 明显胜**。自研 `delay_list_insert` 每次从链首线性找插入点([SCH:153-178]), 且插入者 wake_tick 越晚扫得越远(最坏 O(n)); P-5 只优化了摘除([OR:124-136])。**关键**: [OR:136] 声称 P-5"消除了规模退化", 本测试正是验证该声称的**适用范围**——预期结论是"摘除 O(1), 插入仍 O(n)", 这将直接修正 [OR] 的结论 |
| **3** | **PI-LAT: 中优先级干扰下的优先级反转延迟** | 三任务: L(prio 20, 持 mutex, 持锁后主动 delay 让出)、**M(prio 10, 纯 CPU 忙循环无阻塞)**、H(prio 4, 等锁)。用 DWT 测 H 从 `mutex_take` 调用到**获得锁**的完整时长, 并与"无 M 干扰"的基线对比; 同时记录 L 在持锁期间的优先级轨迹 | **自研在正确实现下应胜或持平, 但若实现有漏算则可能大幅败北**——这是**判定性测试**(非"谁快"而是"哪边正确")。自研已实现继承([ST:1032-1040] 断言提升到 4 并恢复)且修过"多锁 keep-boost"缺陷([ST:2018]), 理论上继承生效后 M **不能**插队; FreeRTOS 同样有继承。差别在**链式/多锁场景**: [DD:1753] L10 明确写"链式继承不传递", 若把 M 换成"持二级锁的中间任务", 自研可能出现数量级劣化。**注意**: 现有测试只测了优先级数值([ST:1032]), 从未测延迟 |
| **4** | **WCET-ISR: 最坏关中断窗口与 SysTick handler WCET** | (a) 用一个**优先级 4**(不被 BASEPRI 0x50 屏蔽)的 TIM 中断, 在中断入口读 DWT、与该中断的定时到期时刻求差, 取 max 作为**关中断时长上界**; (b) 构造 N ∈ {1, 4, 8, 16} 个任务**在同一 tick 到期**, 测 SysTick handler 的入口-出口周期数 | **预期 FreeRTOS 显著胜**, 依据: [PR:169] 已实测"自研 tick 路径更重(状态检查×2 + 完整临界区含 isb + 延时链 + 位图选择 + perf 钩子)", [OR:74-88] 的 P-1 才刚把临界区从 55→23 cyc。更关键的是自研的**临界区里干重活**: heap alloc 全程持锁遍历([HEAP:221-249])、超时扫描([SCH:211-245])、栈巡检持锁遍历全任务([TSK:811-850])。BM0 的 23 cyc 是**空对的成本, 不是上界**——本测试把"上界"这个缺失的数字补上。这是 [OR] 与 [PR] **都没有触及**的维度 |
| **5** | **FRAG: heap_4 vs first-fit+前向合并 的碎片率** | 同 seed / 同序列(交错 size: 16/32/64/128/512B 的 alloc + 伪随机 free)跑 10^4 轮, 每轮记录 ① `rtos_heap_get_free()` 与 ② 实际能 alloc 到的**最大尺寸**, 定义碎片率 = 1 - max_alloc/free_bytes; 与 FreeRTOS heap_4(地址序双向合并)镜像对比 | **FreeRTOS heap_4 胜**。[DD:1750] L7 与 [DD:1334] 已承认自研"只向高地址合并"、[DD:1337] 承认任务删除的"先 free 栈后 free TCB 以触发前向合并"技巧**在交错分配时失效**。FreeRTOS heap_4 的地址序双向合并是 O(1) 判邻接([PR:163] 也提到自研 free 有前向合并+两次释放)。**真实危害**: 碎片导致 `rtos_task_create` 返回 `RTOS_ERR_NO_MEM` 而堆仍有大量空闲, 且按 [SR:119] R2 **零诊断** |
| **6** | **DRIFT/WRAP: 长时间 tick 漂移 + 真实回绕跨边界** | (a) 连续运行 ≥30 分钟, 每 10 秒记录 `rtos_sched_get_tick_count()` 与 DWT 累计周期, 拟合**累积漂移**(ppm); (b) 把 tick 频率临时提高到使 32 位 tick 在数分钟内回绕(或直接把 SysTick 装载值改小), 在**真实跨越 0xFFFFFFFF→0** 的前后各跑一轮延时/定时器精度断言 | **预期两者漂移持平**([PR:63] 实测 168336 vs 168000, 优化后 [OR:48] 完全持平), **但回绕一项可能暴露真问题**: 自研的 FIX1 是**注入式仿真**([ST:1737] 直接改 `tick_count`, 且立刻改回 100000 [ST:1757]), 从未真实跨边界运行; [DD:1744] L1 仍以"无符号比较"描述该债务, 而 [SCH:159] 已改用 `sched_tick_after` 带符号差值 → **代码与 L1 描述可能已不同步**。若真实回绕下延时链有序性被破坏, 症状是"唤醒错乱/定时器失联"([DD:1744] 影响列) |
| **7** | **TIMERLOAD: 多定时器同 tick 到期的回调延迟与 jitter** | 创建 N ∈ {2, 8, 16} 个周期相同的定时器(同 tick 到期), 每个回调记录自己的触发 tick 与 DWT 时刻; 测**第一个与最后一个回调之间的展开时长**以及跨周期的 jitter | **预期自研败**。服务任务是单线程串行执行回调且优先级仅 30([CFG:90], 低于几乎所有应用任务), [DD:1289] 自述"回调串行排队(长回调拖延后续所有定时器, L6 的超时计算时机又放大此效应)"。FreeRTOS 的 timer service task 同为单线程, 差异在**自研多一层 next_remain 计算时机的缺陷(L6, [DD:1749])** 和 tick 路径更重([PR:169])。现有测试同时活跃定时器 ≤2 个([ST:1256-1329]), 完全没有覆盖 |
| **8** | **FOOTPRINT/SYSTEM: 等效 11 任务系统的 Flash+RAM 实测与稳态混合负载** | ① `arm-none-eabi-size` 对两工程同功能固件对比 text/data/bss; ② 在同一 11 任务集(控制周期任务 + 队列生产者/消费者 + 多定时器 + 优先级 6 的 ISR 风暴 + 低优后台)下连续跑 10 分钟, 记录 CPU 占用、最大调度延迟、栈高水位与内存统计 | **可预期自研在 Flash 上可能更小(专用结构无队列泛化, [PR:151]), 但在静态 RAM 上更大**: `RTOS_CONFIG_HEAP_SIZE = 32KB` **无条件静态数组**([CFG:67]), 对比 [PR:33] 的 FreeRTOS heap_4 15KB; 另有 32 个 `ready_heads` 指针([Hsched:46])与 USB 相关的 ~2KB/线程([CFG:181,187])。② 的稳态负载是**唯一能同时触发多条缺口的测试**: 它会自然地把 B1/B2/B4/B7/B10 的风险叠加暴露。当前没有任何测试在**系统级**断言([MC:643-657] 的默认固件只 printf, 无度量) |

### D 补充: 不建议在一天窗口内做的项(避免误投入)

| 项 | 理由 |
|---|---|
| 链式继承三层锁延迟(L10) | 需要构造 A→B→C 三任务三锁链 + 中优先级干扰, 调参易失败; 建议排在 PI-LAT(第 3 名)之后作为延伸 |
| 真实 49.7 天回绕 | 不可行; 用提高 tick 频率的等效方案(D-6b)替代 |
| USB 全链路一致性(CDC/MSC/HID 同时) | 需要主机侧脚本 + 会话级编排, 超出一天; 建议先做 B8.1 的 OSAL 单元测试(纯固件内, 无主机依赖) |
| `RTOS_CONFIG_ASSERT_ENABLED=0` 发布配置回归 | 需要第二份构建产物与第二遍烧录, 收益偏完备性 |

---

## 附录: 审计方法学与可复现性

1. **"未覆盖"的判定方式**: 对每个待判机制, 先在 [H*] 中列出公开 API(附录 A), 再用 grep 在
   [ST]/[BM] 全文搜索该 API 标识符; 零命中即判"零覆盖"。USB 的判定即用此法
   (grep `usb|USB|Usb` 于 `selftest.c` → **0 命中**)。
2. **"文档是否承认"的判定**: grep [DD]/[SR]/[PR]/[OR] 中的 `未测试|未验证|未实测|待测|TODO|局限|限制|风险|债务`
   关键词, 再对命中的段落逐条核对是否伴随实测数据。
3. **行号复核(本次审计的最后一道工序)**: 报告中每一处 `file:line` 引证均已用
   grep `-n` / 读取工具逐条复核原文命中。**工具可靠性提示**: 本会话中通过 `pwsh Get-Content`
   做行号索引会因编码/行尾处理产生偏移(实测 `main.c` 偏 3 行、`selftest.c` 偏 9 行),
   且会低估总行数(把 375 行的本文件报成 294 行)。**因此本报告的行号一律以 `read`/`grep` 工具
   的输出为准**, 复现时请勿使用 shell 行号索引核对。
4. **未读文件清单(因此相关结论为"not determinable")**:
   - `docs\freertos_design.md`(存在但不属本次证据集)
   - `docs\compilation_pipeline.md`、`README.md`
   - `RTOS\Core\Src\rtos_mutex.c` / `rtos_sem.c` / `rtos_queue.c` / `rtos_event.c` / `rtos_timer.c` / `rtos_mem.c` / `rtos_perf.c`: **未逐行读实现**, 相关引用仅来自头部契约([H*])与报告描述; 例外: [rtos_sched.c] 读了 150-269 / 390-479 行, [rtos_task.c] 读了 85-144 / 215-284 / 790-857 行, [rtos_heap.c] 读了 200-299 行, [rtos_usb.c] 只读了 1310-1439 行并做了全文件函数名 grep
   - `Core\Src\stm32f4xx_it.c`(HardFault 诊断实现)、启动文件/链接脚本(因此 MSP 大小、实际 Flash/RAM 布局 not determinable)
   - `docs\selftest_run.txt`(被 [SR:274] 引用, 未在本次读取范围内)
   - FreeRTOS 侧工程(`E:\STM32_Project\CubeIDE\FreeRTOS`)全文
5. **本报告未修改任何工程文件**; 仅新建本审计产物 `docs\_audit_test_coverage_gaps.md`。
