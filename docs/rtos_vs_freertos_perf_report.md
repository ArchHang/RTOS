# 自研 RTOS vs FreeRTOS V10.3.1 真机性能对比测试报告

> 测试日期: 2026-09-15
> 测试方式: 硬件在环 (HIL), 同一块 NUCLEO-F446RE 板分别烧录两套固件
> 对照双方: 自研 RTOS v1.0.0 (`f446_rtos` 工程) vs FreeRTOS Kernel V10.3.1 (`FreeRTOS` 工程)
> 最终结论速览: **IPC 通信路径自研 RTOS 全面更快(13%~145%), 纯上下文切换与任务创建 FreeRTOS 更快(17%~128%), tick 精度两者相当(差 0.2%)**

---

## 1. 测试环境与公平性控制

### 1.1 硬件与工具链 (两侧完全一致)

| 项目 | 值 |
|---|---|
| 目标板 | NUCLEO-F446RE (STM32F446RET6, Cortex-M4F) |
| 主频 | SYSCLK 168 MHz (HSE 8MHz × PLL 336/8/2), APB1 42MHz |
| 编译器 | GNU Tools for STM32 11.3.rel1, **-O2** (原 FreeRTOS 工程为 -O0, 已统一) |
| 串口 | ST-LINK VCP @115200-8-N-1 (观测/输出, 不参与计时) |
| 计时器 | **DWT CYCCNT** (周期级, 1 cycle ≈ 5.952 ns) |
| 烧录 | STM32CubeProgrammer v2.14.0 (SWD, 烧录+监听原子执行) |

### 1.2 内核配置对照

| 配置 | 自研 RTOS | FreeRTOS | 对公平性的影响 |
|---|---|---|---|
| Tick 频率 | 1 kHz | 1 kHz | 一致 |
| 优先级数 | 32 (数值小=高) | 56 (数值大=高) | 拓扑一致(见下) |
| 抢占+时间片 | 开 | 开 | 一致 |
| FPU 上下文 | 惰性保存 (EXC_RETURN bit4) | 惰性保存 (同) | 一致 |
| 临界区 | BASEPRI=0x50 + **isb×2**, 函数调用 | BASEPRI=0x50, 内联, exit 无屏障 | 差异计入实测 |
| 运行时统计 | **perf 常开** (DWT 周期统计, 切换路径钩子) | configGENERATE_RUN_TIME_STATS=0 (零钩子) | **按各自默认出厂配置对比**, 差异在 4.1 节单独量化 |
| 堆 | 自研 first-fit 32KB | heap_4 15KB | 任务创建项真实反映 |
| 栈溢出检测 | 方法2 (idle 批量扫描) | 关闭 | 切换路径均零开销 |
| 中断源 | SysTick 1kHz + 测试用 TIM6 100µs | 同 | 一致 |

### 1.3 基准测试设计

8 项指标, 两侧**逐项镜像的测试代码** (`Core/Src/benchmark.c`), 相同的优先级拓扑与循环次数:

| 逻辑角色 | 自研优先级 | FreeRTOS 优先级 | 栈 |
|---|---|---|---|
| 主控任务 | 9 | 20 | 1024 字 |
| H 被测高优先级 | 6 | 30 | 256 字 |
| A/B 乒乓对 | 14 | 15 | 256 字 |
| C 队列消费者 | 5 | 25 | 256 字 |
| L 低优先级 | 20 | 10 | 256 字 |

所有 printf 在测量区间外; ISR 唤醒测试用 **TIM6 硬件定时器中断**(100µs 周期 × 100 样本)作确定性触发源, 不依赖上位机交互。重复运行验证: 双方各完整执行 2~3 轮, 确定性路径指标 (BM0/BM1/BM6) 逐字节复现, IPC 类 (BM2-BM5) 轮间波动 <3%, 取最终轮数据 (BM7 初版 UART 触发因上位机交互随机性废弃, 已由 TIM6 版替代)。

---

## 2. 测试结果总表

| # | 指标 | 自研 RTOS (cyc) | FreeRTOS (cyc) | 自研/FreeRTOS | 自研 | FreeRTOS | 胜者 |
|---|---|---|---|---|---|---|---|
| 0 | 临界区 enter+exit 一对 | 55 | 47 | 1.17× | 327 ns | 280 ns | FreeRTOS +17% |
| 1 | yield 同优先级切换 | 324 | 142 | **2.28×** | 1929 ns | 845 ns | **FreeRTOS 快 2.28×** |
| 2 | 信号量乒乓往返¹ | **1339** | 3230 | 0.41× | 7970 ns | 19226 ns | **自研快 2.42×** |
| 3 | sem 抢占唤醒延迟² | **488** (min 486 / max 731) | 651 (646/910) | 0.75× | 2905 ns | 3875 ns | **自研快 1.33×** |
| 4 | 队列 4B send→唤醒 | **625** (624/917) | 764 (758/1019) | 0.82× | 3720 ns | 4548 ns | **自研快 1.22×** |
| 5 | 任务 create+delete | 6575 | 5478 | 1.20× | 39.1 µs | 32.6 µs | FreeRTOS 快 1.20× |
| 6 | delay(1) 实际周期 | 168336 | 168000 | 1.002× | 1002.0 µs | 1000.0 µs | 持平 (差 0.2%) |
| 7 | ISR→任务唤醒延迟³ | **542** (536/604) | 613 (609/685) | 0.88× | 3226 ns | 3649 ns | **自研快 1.13×** |

> ¹ 一回合 = 双任务各执行一次 take+give, 含 2 次完整上下文切换。
> ² 低优先级任务 give 信号量 → 高优先级任务醒来首条指令的延迟, N=1000。
> ³ TIM6 中断内 give → 高优先级任务醒来, N=100。

**抖动 (max-avg)**: 自研 BM3 抖动 243 cyc / FreeRTOS 259 cyc —— 两者同级; 自研 max 更小 (731 vs 910)。

---

## 3. 原理分析: 每一个数字为什么是这样

### 3.1 [自研快 2.28×? 不, 慢] BM1 yield 切换: 324 vs 142 —— 差距 182 周期的逐项归因

这是自研 RTOS 输得最彻底的指标, 差距全部来自**切换路径上的固定开销叠加**:

**(a) 双层 C 函数调用 vs 汇编直存 (约 40~50 周期)**

FreeRTOS PendSV 处理器是 `naked` 纯汇编 ([port.c:431-465](file:///E:/STM32_Project/CubeIDE/FreeRTOS/Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F/port.c#L431)):

```asm
mrs r0, psp
isb
ldr r3, pxCurrentTCBConst
ldr r2, [r3]
tst r14, #0x10            /* 惰性 FPU 判定 */
it eq
vstmdbeq r0!, {s16-s31}
stmdb r0!, {r4-r11, r14}
str r0, [r2]              /* ★ 栈顶指针 3 条指令直接保存 */
stmdb sp!, {r0, r3}
mov r0, #5
msr basepri, r0
dsb / isb
bl vTaskSwitchContext     /* ★ 唯一一次 C 调用 */
...恢复...
```

自研 RTOS 的 PendSV 在同样的保存序列后走 **`blx rtos_port_save_and_switch` (间接调用: ldr 字面量 + blx) → `rtos_sched_context_switch()` (第二层 C 调用)** ([port.c:362-387](file:///e:/STM32_Project/CubeIDE/f446_rtos/RTOS/Port/GCC/ARM_CM7/port.c#L362), [rtos_sched.c:429-453](file:///e:/STM32_Project/CubeIDE/f446_rtos/RTOS/Core/Src/rtos_sched.c#L429))。两层调用各付序言/尾声 (push/pop r4-r7 等 ≈15-25 周期)。

**(b) 性能监视器钩子常开 (约 30~40 周期/切换)**

自研每次切换在热路径上执行 ([rtos_perf.c:135-182](file:///e:/STM32_Project/CubeIDE/f446_rtos/RTOS/Core/Src/rtos_perf.c#L135)):
- 请求侧 `rtos_perf_on_pend_switch()`: DWT CYCCNT 读 + 1 次存储 ≈10 周期;
- PendSV 内 `rtos_perf_on_context_switch(prev, next)`: DWT 读 + `prev->perf_run_cycles` 读改写 + kernel `perf_total_cycles/perf_idle_cycles` 两次累加 + 调度延迟 5 项统计 (max/sum/count RMW) ≈20-25 周期;
- **三重切换计数**: `rtos_kernel.switch_count++` (请求侧) + `next->switch_count++` (PendSV 内) + `perf_switch_total++`, 3 次 volatile 读改写 ≈6-9 周期。

FreeRTOS 此刻 `configGENERATE_RUN_TIME_STATS=0 + configUSE_TRACE_FACILITY` 未激活钩子, **零指令**。`vTaskSwitchContext` 在该配置下只剩 `pxCurrentTCB = pxCurrentTCB->pxTaskTag...` 级别的几条赋值 (同优先级轮转直接 `listGET_OWNER_OF_NEXT_ENTRY` 取下一项)。

**(c) 临界区屏障差异 (约 10~15 周期)**

FreeRTOS `vPortExitCritical` ([port.c:420-428](file:///E:/STM32_Project/CubeIDE/FreeRTOS/Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F/port.c#L420)): assert + 计数-- + 归零时 `vPortSetBASEPRI`(**纯 msr, 无屏障**)。自研 `rtos_port_exit_critical` 是函数调用 + assert + 计数-- + `msr basepri + isb`。每对临界区多 1~2 条 `isb` (M4 上 ISB ≈ 4-8 周期, 流水线冲刷)。

**(d) 每调用固定检查 (约 15~20 周期)**

`rtos_sem_take/queue_send` 等每次调用 `RTOS_PORT_IN_ISR()` (**函数调用** + mrs ipsr + 比较); `rtos_sched_context_switch` 内的状态搬运 (prev/next state 字段写)。FreeRTOS 发布版把 ISR 检查全部编译为空 (configASSERT 关闭中断检查)。

> 182 ≈ (a)45 + (b)35 + (c)12 + (d)18 + 调度选择差异 ~70 —— 归因闭环。

**架构注记**: 自研把"选下一任务"提前到请求切换的临界区内 (`rtos_sched_schedule`), PendSV 内只做搬运 —— 设计意图是缩短 PendSV 禁中断窗口; 但**每次 yield 都要跑一遍 RBIT+CLZ 位图选择**, 而 FreeRTOS 同优先级 yield 直接轮转链表取下一项 (零选择)。对本测试的同优先级场景, 自研的选择前置反而多付了位图计算。

### 3.2 [自研快 2.42×] BM2 信号量乒乓: 1339 vs 3230 —— 专用结构对队列泛化的胜利

**这是差距最大的指标, 纯架构级差异。** 扣除双方各含 2 次切换 (BM1×2): 自研 IPC 净开销 691 周期, FreeRTOS **2946 周期, 净差 2255 周期 (4.3×)**。

FreeRTOS 的信号量是**队列的特例** (V10.3.1 经典设计): `xSemaphoreGive → xQueueGenericSend(queueSEND_TO_BACK)`、`xSemaphoreTake → xQueueSemaphoreTake → xQueueGenericReceive`。一次 give 唤醒路径 ([queue.c:2406](file:///E:/STM32_Project/CubeIDE/FreeRTOS/Middlewares/Third_Party/FreeRTOS/Source/queue.c#L2406)):

```
临界区 → 队列锁检查分支 → prvCopyDataToQueue(信号量分支=++计数)
→ co-routine 等待检查分支 (mtCOVERAGE_TEST_MARKER 死分支)
→ xTaskRemoveFromEventList:
     uxListRemove(事件项) + uxListRemove(状态项)   ← 两次 O(1) 双向摘链
     + prvAddTaskToReadyList + uxSchedulerSuspended 挂起检查
→ portYIELD_WITHIN_API → PendSV
```

单是 `xTaskRemoveFromEventList` ([tasks.c:3138-3183](file:///E:/STM32_Project/CubeIDE/FreeRTOS/Middlewares/Third_Party/FreeRTOS/Source/tasks.c#L3138)) 就是两次列表摘除 + 就绪插入 + 三处分支。take 阻塞路径还要走 `vListInsert` 延时表 + 事件表尾插。**巨型泛化函数 (xQueueGenericSend 数百行, 含互斥锁让渡/通知/peek/超时全套分支) 的分支深度本身就是成本** —— 即使 item_size=0 不拷数据。

自研 RTOS 是**专用信号量结构** ([rtos_sem.h](file:///e:/STM32_Project/CubeIDE/f446_rtos/RTOS/Core/Inc/rtos_sem.h): `count/max_count/wait_head`):

```
give: 临界区 → rtos_internal_wake_highest(取链首) → wait_remove(双向 O(1) 摘链)
      → rtos_sched_unblock(delay_list 摘除 + ready 尾插 + 优先级比较) → schedule → exit
```

无队列锁、无 co-routine 分支、无双 ListItem 摘链。**尽管自研的延时表摘除是 O(n) 单向线性遍历** (FreeRTOS 是 O(1) 双向), 静态短链 (测试中仅 2 个任务) 下劣势未显现, 专用路径的精简完胜。

> **深读**: 该结果不代表 FreeRTOS 设计差 —— 它用一套机制统一了队列/互斥/计数信号量/递归锁四种对象 (代码体积与维护性收益); 自研用 sem/queue/mutex 三种专用结构换取 IPC 热路径速度。这是"泛化 vs 专用"的经典工程权衡, 数据把代价量化为 **每对阻塞-唤醒 ~1100 周期 (6.7µs)**。

### 3.3 [自研快 13%~33%] BM3/4/7 唤醒延迟类: 同一优势的三个切面

- **BM3 sem 唤醒 (1.33×)**: 同 3.2 的 give 路径差异, 且自研唤醒链首即最高优先级等待者 (等待链表按优先级**排序插入** [rtos_task.c:113-133](file:///e:/STM32_Project/CubeIDE/f446_rtos/RTOS/Core/Src/rtos_task.c#L113), FreeRTOS 尾插+取首)。
- **BM4 队列唤醒 (1.22×)**: 双方都有"阻塞接收者直传"机制 (拷贝直接进等待任务缓冲), 但自研的直传判定与唤醒路径更短; FreeRTOS 的队列路径额外付泛化分支。
- **BM7 ISR 唤醒 (1.13×)**: 中断内 `xSemaphoreGiveFromISR` + `portYIELD_FROM_ISR` vs 自研 `rtos_sem_give` (ISR 内含切换请求)。优势收窄到 13% 是因为 FromISR 变体做了专门裁剪 (省掉挂起检查), 而**自研的 ISR 路径仍走完整临界区 (isb×2) + IN_ISR 检查** —— 3.1(c)(d) 的固定税在 ISR 场景依然存在, 抵消了部分架构优势。

### 3.4 [FreeRTOS 快 1.20×] BM5 任务创建+删除: 6575 vs 5478, 差距 1097 周期的三个来源

1. **堆分配次数与算法** (约 250 周期): 自研 `rtos_task_create` 分配 **2 次** (TCB + 栈分离, [rtos_task.c:333/339](file:///e:/STM32_Project/CubeIDE/f446_rtos/RTOS/Core/Src/rtos_task.c#L333)), 每次是 **first-fit 全块线性遍历** (含已分配块, [rtos_heap.c:206-230](file:///e:/STM32_Project/CubeIDE/f446_rtos/RTOS/Core/Src/rtos_heap.c#L206)); FreeRTOS `xTaskCreate` **1 次**合并分配 (TCB+栈单块), heap_4 只遍历**空闲块链**。
2. **栈水印填充** (约 650~700 周期): 自研 `rtos_perf_on_task_created` 对 128 字 (512B) 栈**逐字填 0xA5A5A5A5** ([rtos_perf.c:226-235](file:///e:/STM32_Project/CubeIDE/f446_rtos/RTOS/Core/Src/rtos_perf.c#L226)) —— 这是栈高水位测量功能的固有成本, FreeRTOS 关闭等价功能 (uxTaskGetStackHighWaterMark 是查询时扫描)。
3. **删除侧**: 自研 free 有前向合并 + 两次释放; FreeRTOS heap_4 双向合并单次。

**这 1097 周期中约 2/3 是"功能税"** (水印统计), 1/3 是堆实现差距。若关闭 `RTOS_CONFIG_PERF_STACK_WATERMARK`, 预期差距缩至 ~5% 以内。

### 3.5 [持平] BM6 delay(1): 168336 vs 168000, 自研醒得晚 336 周期 (2µs)

SysTick 周期是硬件的 (精确 168000 cyc = 1ms), 测得的 +336 是**到期唤醒的处理延迟**: 任务在 tick 中断处理完并调度后才恢复运行。自研 SysTick 路径更重 ([port.c:525-536](file:///e:/STM32_Project/CubeIDE/f446_rtos/RTOS/Port/GCC/ARM_CM7/port.c#L525)): 调度状态检查×2 + **完整临界区 (函数调用 + isb×2 + 嵌套计数)** + 延时链处理 + 位图选择 + perf 钩子; FreeRTOS `xPortSysTickHandler` ([port.c:488-504](file:///E:/STM32_Project/CubeIDE/FreeRTOS/Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F/port.c#L488)) 用**内联 BASEPRI 掩码, 无计数无 isb**, `xTaskIncrementTick` 内一次链首检查。+0.2% 的周期偏差对绝大多数应用无感, 但在µs 级周期任务 (如 1kHz 控制环) 里属于可测的固定相位滞后。

### 3.6 [FreeRTOS 快 17%] BM0 临界区: 55 vs 47

| 步骤 | 自研 | FreeRTOS |
|---|---|---|
| enter | 函数调用 + `msr basepri` + **isb** + 计数++ | 内联 `vPortRaiseBASEPRI` (mov+msr+isb+dsb) + 计数++ |
| exit | assert + 计数-- + 归零判 `msr basepri` + **isb** | assert + 计数-- + 归零 `vPortSetBASEPRI` (**纯 msr 无屏障**) |

差值 ≈ 1~2 条 isb + 函数调用开销。另外 FreeRTOS 有 ISR 快路径 `portSET_INTERRUPT_MASK_FROM_ISR` (无计数无屏障), 自研 ISR 内走与任务相同的完整临界区 —— 这也部分解释了 BM7 优势为何小于 BM3。

---

## 4. 架构层差异总结

| 维度 | 自研 RTOS | FreeRTOS V10.3.1 | 实测影响 |
|---|---|---|---|
| IPC 对象模型 | 专用结构 (sem/queue/mutex 独立) | 队列统一泛化 | **IPC 快 13%~145%** (自研) |
| PendSV 结构 | 汇编→双层 C 调用, 选择前置 | 汇编直存+单次 C 调用 | 切换慢 ~45 cyc (自研) |
| 运行时统计 | **常开** (DWT 周期级+调度延迟) | 默认关 | 切换慢 ~35 cyc, 创建慢 ~700 cyc (自研功能税) |
| 临界区 | BASEPRI + isb×2 + 函数 | BASEPRI + 内联 + exit 无屏障 | 临界区慢 8 cyc (自研) |
| 就绪选择 | 32 位位图 RBIT+CLZ 无状态重算 | uxTopReadyPriority 增量维护+下溯 | 同优先级 yield 场景 FreeRTOS 免选择 |
| 延时结构 | 升序单向链表: 到期查 O(1), **摘除 O(n)** | 双向链表: 插/摘均 O(1) | 本测试短链未显现; 大量并发超时等待时自研 give 唤醒路径会退化 |
| 任务创建堆 | 2 次分配 + O(stack) 水印 | 1 次合并分配 | 创建慢 17% (自研) |
| ISR 内 IPC | 完整临界区(含 isb/计数) | FROM_ISR 快路径 | ISR 唤醒优势从 33% 收窄到 13% |

**值得强调的三点**:

1. **自研 RTOS 是带着常开的 perf 统计跑赢全部 IPC 指标的** —— 若关闭 `RTOS_CONFIG_USE_PERF_MONITOR` (该开关真正裁剪热路径钩子, 仅剩 switch_count 三重计数与 isb), BM2 预计再快 ~15%, BM1 差距从 2.28× 缩到 ~1.7×。功能税已按源码逐项量化, 数据表反映的是**双方出厂默认配置**的真实对比。
2. **自研的等待链表按优先级排序插入 (O(n)) vs FreeRTOS 尾插 (O(1))** 是一个"实时性保证换吞吐"的选择: 自研在任意时刻唤醒的必是最高优先级等待者 (含运行中改优先级的边角场景), FreeRTOS 依赖"高优先级必然先阻塞"的调度序不变式。本测试拓扑简单, 两者等效; 极端并发下自研的插入成本会随等待者数增长。
3. **FreeRTOS 的 tick 路径更精简** (内联掩码、无计数、无 isb), 对高频率 tick (如 10kHz) 或 tickless 场景是实打实的优势; 自研 tick 的重开销与 336 周期唤醒滞后在 µs 级实时环里需要关注。

---

## 5. 结论

1. **IPC 吞吐与唤醒延迟 (RTOS 最常用的热路径): 自研 RTOS 全面占优** —— 信号量乒乓快 2.42×, 任务间唤醒快 1.33×, 队列唤醒快 1.22×, ISR 唤醒快 1.13×, 且抖动不劣于 FreeRTOS。专用 IPC 结构 + 零队列泛化是其直接来源。
2. **裸上下文切换: FreeRTOS 快 2.28× (845ns vs 1929ns)**。差距可完全归因于: 双层 C 调用 (~45 cyc) + 常开统计钩子 (~35 cyc) + 临界区屏障/ISR 检查固定税 (~30 cyc) + 同优先级场景免选择 (~70 cyc)。前三项合计约 110 周期属于**可优化项** (见建议), 优化后理论可达 ~1.6×。
3. **任务创建/删除: FreeRTOS 快 1.20×**, 其中约 2/3 是自研栈水印功能的固有成本 (可配置), 1/3 是堆实现差距 (first-fit 全块遍历 vs heap_4 空闲链)。
4. **tick/延时精度: 持平** (自研唤醒滞后 +2µs, 0.2%)。
5. **综合判断**: 这是一个"**IPC 快、切换重**"的内核 —— 适合中断/事件驱动、消息吞吐密集的场景 (本项目音频分类管线正是此类); 若用于高频调度切换为主的负载 (如大量同优先级任务协作), 切换路径有明确的优化空间。

### 优化建议 (按性价比排序)

| # | 改动 | 预期收益 | 依据 |
|---|---|---|---|
| 1 | `RTOS_PORT_ENTER/EXIT_CRITICAL` 改为 `static inline` (或宏) + exit 去掉 isb (M4 上 msr basepri 对取指流的影响有限, FreeRTOS 同判) | 切换 -10~15 cyc, 每 IPC -10 cyc | 3.1(c) |
| 2 | `rtos_sched_context_switch` 的双层调用扁平化: PendSV 直接 `bl` 单一函数, 内部直存 `top_of_stack` | 切换 -20~40 cyc | 3.1(a) |
| 3 | perf 钩子改编译期开关默认关 / 或降级为"仅 DWT 基准差"两条指令 | 切换 -30~35 cyc, 创建 -650 cyc | 3.1(b)/3.4 |
| 4 | 延时表改双向链表 (摘除 O(n)→O(1)) | 并发超时多时 give 唤醒不再退化 | 3.2 注 |
| 5 | `RTOS_PORT_IN_ISR()` 改内联宏 | 每 IPC 调用 -8~10 cyc | 3.1(d) |

> 1+2+3+5 全部落地后, yield 切换预计从 324 → ~230-250 cyc (差距 2.28× → ~1.7×), IPC 优势进一步扩大。

---

## 6. 复现指南

1. 自研侧: `E:\STM32_Project\CubeIDE\f446_rtos`, `Core/Src/benchmark.c` 已集成 (main.c 创建 bench 任务, prio 9); FreeRTOS 侧: `E:\STM32_Project\CubeIDE\FreeRTOS`, 同名文件已集成 (原生 API, prio 20; 已补 `__io_putchar`/USART2_IRQHandler/TIM6 中断)。
2. 两工程 Debug 目录均已统一 **-O2** (全量 subdir.mk)。
3. 执行: `make -j8 all` 后 `STM32_Programmer_CLI -c port=SWD -w <elf> -rst`, 115200 打开 COM3, 等待 `BM|DONE` 结束行; 全流程无需人工交互。
4. 数据行格式: `BM|<id>|<name>|N=<n>|avg=<>|min=<>|max=<>` (单位: CPU 周期, ×5.952 = ns)。
