# RTOS 性能优化与硬件验证记录（2026-10-12）

> 本报告记录第一轮已完成的历史阶段；当前代码又完成了底层优化。最新实现、实板结果与日常镜像见 [底层优化报告](rtos_lowlevel_optimization_20261012.md)。

本轮最终保留 `nosched` 组合：临界区局部变量优化 + 互斥锁有序队列队首计算。调度器候选和对齐试验不保留。最终源码另完成全套板上回归，并已恢复日常固件。BM2 对夹具/构建布局的敏感性尚未解释，报告完整保留收益与退步，以及较早候选、有效控制对照和无效镜像各自的身份。

## 1. 当前可以确认的结果

本轮以此前完成缺陷修复的源码为性能基线，保留任务身份检查、等待队列不变式、中断屏蔽与 PendSV 竞态保护。先后测量临界区局部变量优化、唤醒调度路径优化、互斥锁有效优先级计算优化，使用同一块 NUCLEO-F446RE 板，每阶段运行三次独立复位测试。

最终保留的两项优化有明确的局部收益：临界区进出从 30 降至 24 cycle，4/8 个等待者的互斥锁优先级计算分别从 71/107 降至 46 cycle。但是带 PC 夹具的 perf 中 BM2 从 1395 增至 1607 cycle（+15.20%），单等待者互斥锁计算从 45 增至 46 cycle；普通 bench 对照中 BM2 从 1393 增至 1394 cycle（+0.07%），ISR 唤醒平均延迟从 546 增至 552 cycle（+1.10%）。不能概括为所有路径或所有吞吐都更快，完整两种夹具对照见第 5 节。

最终组合另完成新的审计回归 117 项 × 3 次、基础自测 432 项、边界测试 103 项，均无 FAIL，S1–S5 均返回 `status=0`。此前三项 combined 候选也完成一轮相同范围的回归，但其结果单独归档。这些结果证明所列测试运行通过，不构成任意负载下的正确性或最坏时延保证。

缺陷修复基线的范围见 [此前审计报告](rtos_audit_20261012.md)。本报告的性能与回归证据均来自新的 [perf_20261012 目录](../hil/perf_20261012)，不将旧审计结果计作本轮新验证。

## 2. 测量阶段与实现身份

四阶段是累积应用，`sched` 包含 `port`，`combined` 包含前两项。每份源码清单记录 RTOS 下 61 个 C/H 文件的 SHA-256。相对性能基线，四阶段只有下表所列文件发生变化；性能夹具保持相同。

| 阶段 | 相对性能基线的源码变化 | 源码清单 | 测量记录 |
| --- | --- | --- | --- |
| baseline | 无；此前完成缺陷修复的源码 | [sources_baseline.json](../hil/perf_20261012/sources_baseline.json) | [measurement_baseline.json](../hil/perf_20261012/measurement_baseline.json) |
| port | `rtos_port.h` | [sources_port.json](../hil/perf_20261012/sources_port.json) | [measurement_port.json](../hil/perf_20261012/measurement_port.json) |
| sched | `rtos_port.h`、`rtos_sched.c` | [sources_sched.json](../hil/perf_20261012/sources_sched.json) | [measurement_sched.json](../hil/perf_20261012/measurement_sched.json) |
| combined | `rtos_port.h`、`rtos_sched.c`、`rtos_mutex.c` | [sources_combined.json](../hil/perf_20261012/sources_combined.json) | [measurement_combined.json](../hil/perf_20261012/measurement_combined.json) |

同一阶段的三次运行使用相同 ELF，记录的 SHA-256 如下。源码清单用于区分实现，ELF 哈希用于区分实际烧录镜像；两者不能互相替代。

| 阶段 | 三次运行的 ELF SHA-256 |
| --- | --- |
| baseline | `BCD7EB4E69919E39D81BE50D0385241ABAF359143A403D988EA86131C0A85148` |
| port | `0630E40732D2401E9F20761A8FFF871962355080FFBA5329F3D07C7C68BAD008` |
| sched | `4772BF0F829B2729E57E24BECEF4F7B12B39270FC41CFC722540B2A933F29D34` |
| combined | `6E25A056915612B88A094903DD77C0CC0A6EA69C6097E8C5D38B39E7B66B66F3` |

### 2.1 临界区：减少重复读取嵌套计数

进入临界区时仍先读取进入前的 BASEPRI/PRIMASK 并建立屏蔽，然后将 `rtos_port_crit_nest` 读入局部变量，使用该值判断最外层进入并写回 `nesting + 1`。退出时将减一结果保存在局部变量，再判断是否恢复原屏蔽。原实现分别为条件和增减表达式重复读取 volatile 计数，候选减少这两次重复读取；没有增加全局状态或改变结构体布局。

必须保留的不变式：

- 共享嵌套计数的读取发生在建立中断屏蔽之后。
- BASEPRI 使用 `BASEPRI_MAX`，保留调用方更严格的屏蔽；最外层退出恢复进入前的值。
- PRIMASK 配置保存并恢复进入前的全局中断状态。
- 嵌套进入/退出、编译器 `memory` clobber、进入侧 ISB 与异常上下文约束保持原语义。

本项只优化 C 代码重复加载，不减少中断屏蔽操作或屏障。板上 BM0 的 6 cycle 差值是该构建和夹具的测量结果，不是对每条加载指令固定耗时的架构断言。源码路径为 [rtos_port.h](../RTOS/Core/Inc/rtos_port.h)，已测阶段应以清单和保存的反汇编辨认。

### 2.2 唤醒调度：利用已知就绪优先级

`rtos_sched_unblock` 已知道新就绪任务的优先级。候选减少再次进入完整调度选择路径的重复工作，并把“提交选定任务”的逻辑集中到共享 helper。选择某个优先级后仍取该优先级就绪链表的队首，不能直接用刚唤醒的 TCB 替换队首，否则会改变同优先级轮转次序。

保留调度状态检查、完整调度入口的空位图保护、抢占开关、`next == current` 时撤销过期切换并恢复任务状态、切换统计与 PendSV 请求。未移除此前审计添加的 guard，也未修改 TCB 字段。候选的机器码可查 [sched 阶段调度器反汇编](../hil/perf_20261012/sched_rtos_sched.disassembly.txt)。

这一候选虽然使部分唤醒路径测量下降，却同时对应 BM2 从 `port` 的 1494 上升至 1603 cycle。后续强制重编译的普通 bench 中，三项 combined 组合 BM2 为 1654，相对同夹具基线 1393 增加 18.74%，因此最终交付组合恢复原调度器实现。不能单凭源码指令减少认定整体更快。

### 2.3 互斥锁：使用有序等待队列的队首

`rtos_internal_mutex_compute_effective_priority` 仍扫描已注册互斥锁，对当前任务拥有的锁计算继承优先级。原实现继续扫描每个锁的全部等待者；候选使用按优先级排序的等待队列队首取得该锁的最高等待优先级，再与任务基础优先级及其余所持锁比较。

必须保留等待队列按优先级排序、同优先级 FIFO，以及插入、调优先级重排、删除和挂起时维护队列与继承回退的语义。仍遍历全部注册锁并处理多锁持有，不能提前返回第一个锁的结果。按注册锁数量 M、所持锁等待者总数 W 表示，计算成本由 O(M + W) 降到 O(M)；这不是对整个内核的 O(1) 优先级继承保证，也没有新增传递继承功能。

本项没有新增 TCB/互斥锁字段或修改公共 API，候选机器码见 [combined 互斥锁反汇编](../hil/perf_20261012/combined_rtos_mutex.disassembly.txt)。单等待者平均多 1 cycle、4/8 等待者明显减少，说明收益与工作负载相关。

### 2.4 PendSV 与中断保护保持不变

本轮没有修改 PendSV 汇编。此前修复的保护继续覆盖内核当前任务读取、C 调度决策与新任务上下文恢复，直到新 PSP 设置完成后恢复原中断屏蔽。BASEPRI 模式屏蔽数值优先级不小于系统调用阈值的可屏蔽中断；数值更小的高优先级中断仍可抢占，但不得调用 RTOS API。PRIMASK 模式屏蔽普通可屏蔽中断，不屏蔽 NMI/HardFault。

旧屏蔽值在 MSP 上保存，调用 C 时维持 8 字节对齐。EXC_RETURN、R4–R11、FPU S16–S31 保存和恢复路径保持不变。不能借性能优化删除此保护窗口或假定进入前屏蔽为零。参考 [port.c](../RTOS/Port/GCC/ARM_CM7/port.c) 和 [combined 移植层反汇编](../hil/perf_20261012/combined_port.disassembly.txt)。文件目录名为 ARM_CM7，本轮实际测试目标仍是 Cortex-M4 的 STM32F446RE。

## 3. 已考虑但未实施的方案

下表是源码审查层面的候选，未取得对应的板上性能收益证据。RAM 成本是 32 位平台示例估算，不能视为已经增加的实际用量。

| 方案 | 可能减少的工作 | ABI / RAM 成本及未采用原因 |
| --- | --- | --- |
| 嵌套临界区跳过重复 mask/ISB | 嵌套调用的系统寄存器操作 | 可不增加结构体字段，但依赖“非零计数必然已建立所需屏蔽”的更强约束；还会增加外层分支。收益与正确性证明不足，未采用。 |
| 删除进入临界区/PendSV 的 ISB，或删除 PendSV 请求侧屏障 | 屏障等待 | 无 RAM 收益，涉及异常、中断同步和不同 Cortex-M 目标；没有充分证据支持在保留全部运行语义时删除，未采用。 |
| C 调度决策返回当前 TCB，供 PendSV 直接使用 | C 返回后的全局指针重新加载 | 无新增结构体 RAM，但改变内部 C/汇编调用约定与声明，需覆盖所有返回分支、屏蔽窗口、FPU 路径并检查最终机器码；本轮未实施。 |
| 任务身份注册表增加地址索引/哈希 | 公共 API 的身份查找 | 不得删除生命周期 guard。B 个 32 位指针 bucket 至少增加 4 × B 字节；例如 64 bucket 至少 256 字节，若再给 TCB 增加链接字段还会改变 ABI。需维护删除、创建中间态和地址重用，未实施。 |
| 每个任务维护所持互斥锁链表 | 扫描所有注册互斥锁 | 示例双向侵入式链表需每 TCB 一个 4 字节头指针、每 mutex 两个共 8 字节链接指针，改变结构体布局；所有获取、释放、反初始化和任务退出路径都需维护。本轮保留注册表扫描，只消除每把锁的等待者扫描。 |

## 4. 硬件、构建与测量方法

### 4.1 真实执行环境

板上记录显示 NUCLEO-F446RE、STM32F446xx、Cortex-M4、设备 ID `0x421`、Flash 512 KB，SWD 4 MHz；ST-LINK 序列号 `066DFF555775514867092907`，固件 V2J43M28。使用 STM32CubeProgrammer 2.15.0 连接、下载、校验并硬件复位。串口为 COM3、115200 baud。工具链来自 STM32CubeIDE 1.13.2 的 GNU Arm 11.3.rel1，目标参数包含 Cortex-M4、`fpv4-sp-d16`、hard-float 和 `-O2`。

CPU 为 168 MHz，DWT `CYCCNT` 用于记录 cycle，1 cycle 约为 5.95 ns。没有把串口打印耗时计入目标操作的测量区间。`perf` 固件关闭应用任务、USB 应用和故障注入任务，保持断言启用；结果对应这一测试配置，不能直接等同于日常应用负载。

下载与串口捕获的一个完整实例见 [combined r1 烧录记录](../hil/perf_20261012/flash_perf_20261012_combined_r1.txt)、[运行元数据](../hil/perf_20261012/run_perf_20261012_combined_r1.json) 及 [原始串口日志](../hil/perf_20261012/log_perf_20261012_combined_r1.txt)。各阶段的 r2/r3 有各自的 flash/run/log 文件，不是把同一次输出重复计数。

### 4.2 每阶段的步骤与构建隔离

1. 确认候选源码，保存 61 个 RTOS C/H 文件的路径和哈希。
2. 构建相同 `perf` 夹具，保存构建输出、ELF、相关对象反汇编和 ELF 哈希。
3. 每阶段执行三次下载、校验、硬件复位、串口捕获，直到 `BM|DONE`。
4. 检查每次 mutex 夹具 `PC|SUMMARY|pass=22 fail=0`、11 个指标完整、运行期间 ELF 哈希不变，保存 measurement JSON。

自动化入口为 [run_measurement.ps1](../hil/perf_20261012/run_measurement.ps1)，构建配置见 [build_fw.ps1](../hil/build_fw.ps1)。脚本测量当前源码，阶段名称本身不负责切换实现；复测前必须先核对源码清单。已完成的阶段 JSON 禁止覆盖，应使用新阶段名保存新结果。

初始四阶段通过源码修改逐阶段刷新相应文件并编译。后续增加了复制快照的控制对照；复制保留旧时间戳，曾导致一个 `baseline_bench` 构建复用旧内核对象。该镜像不代表基线，已标记为 [invalid_stale_objects_baseline_bench.json](../hil/perf_20261012/invalid_stale_objects_baseline_bench.json) 并保存 [说明](../hil/perf_20261012/invalid_stale_objects_baseline_bench.md)，其性能值不进入本报告结论。当前测量脚本在每次构建前强制删除 `Debug/RTOS`、`Debug/Core/Src`、`Debug/hil` 中的对象和依赖文件，再重编译。下文 `clean_base_bench`、`clean_combined_bench`、`nosched`、`nosched_bench` 使用这一隔离方式。

### 4.3 BM 指标的区间和样本数

原 BM0–BM7 夹具见 [benchmark.c](../Core/Src/benchmark.c)。BM0 对 10000 次临界区对求批量均值；BM1 对两任务共 20000 次 yield 的切换区间求均值；BM2 对两任务各 5000 轮信号量 take/give、每轮含两次切换的往返求均值；BM5 对 500 次创建/删除求批量均值，BM6 对 1500 次 delay(1) 求周期均值。因此 JSON 中这些指标的 `N=1` 是一个已计算的批量均值，并非只执行一次操作。

BM3 记录 1000 个唤醒样本，BM4 记录 5000 个队列发送/唤醒样本。BM7 使用板上 TIM6 每 100 μs 的真实定时器中断，NVIC 数值优先级 6，记录 100 个 ISR 唤醒延迟样本；它不是由电脑串口字符到达触发。

### 4.4 PC22：真实阻塞等待者与优先级不变式

新增夹具 [perf_probes.c](../hil/fw/Src/perf_probes.c) 创建真实静态任务，等待者实际调用 `rtos_mutex_take(..., RTOS_WAIT_FOREVER)`。控制任务确认 TCB 状态、`blocked_on`、wait node 的链表身份与对象一致，再进行计算测量；没有手工伪造等待节点。使用 8 个 TCB、每任务 256 word 的栈和两个互斥锁。

单锁分别准备 1/4/8 个等待者，创建顺序故意不同于优先级排序。每组采样 1000 次，临界区进入在 `t0` 前、退出在 `t1` 后；测量区间包含 helper 调用和 DWT 读数，没有扣除读数固定开销。任务创建、等待状态确认、链表检查、统计和串口输出位于该区间之外。每次返回值都与预期有效优先级比较。

每次运行共 22 个逻辑检查，还覆盖多锁最低优先级选择、等待者升降优先级、同优先级 FIFO、挂起/删除等待者后的继承回退、释放一把锁仍保留另一把锁的提升，以及最终恢复基础优先级和清理。每个已测阶段的三次运行均为 `pass=22 fail=0`。这些检查针对当前非传递继承实现的不变式，不证明复杂传递继承或持续中优先级干扰下的最坏阻塞时间。

## 5. 四阶段全部性能结果

单位为 cycle。表中值是三个独立复位运行的 `avg` 中位数；本轮每个阶段三次运行所打印的 avg/min/max 都相同。百分比为 `(combined - baseline) / baseline`，负值表示减少。完整逐次 min/max 可在第 2 节四份 measurement JSON 查看。

| 指标 | 日志 N | baseline | port | sched | combined | combined 相对 baseline |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| BM0 `critical_pair` | 1（批量均值） | 30 | 24 | 24 | 24 | −6 / −20.00% |
| BM1 `yield_switch` | 1（批量均值） | 241 | 234 | 235 | 234 | −7 / −2.90% |
| BM2 `sem_roundtrip` | 1（批量均值） | 1395 | 1494 | 1603 | 1406 | +11 / +0.79% |
| BM3 `sem_wake_latency` | 1000 | 487 | 467 | 462 | 434 | −53 / −10.88% |
| BM4 `queue_send_wake` | 5000 | 574 | 554 | 545 | 548 | −26 / −4.53% |
| BM5 `task_create_del` | 1（批量均值） | 6188 | 6104 | 6110 | 6150 | −38 / −0.61% |
| BM6 `delay1_period` | 1（批量均值） | 168000 | 168000 | 167999 | 168000 | 0 / 0.00% |
| BM7 `isr_wake_latency` | 100 | 546 | 536 | 526 | 497 | −49 / −8.97% |
| PB `mutex_compute_w01` | 1000 | 45 | 44 | 44 | 46 | +1 / +2.22% |
| PB `mutex_compute_w04` | 1000 | 71 | 71 | 71 | 46 | −25 / −35.21% |
| PB `mutex_compute_w08` | 1000 | 107 | 107 | 107 | 46 | −61 / −57.01% |

平均值减少不代表最大观测值一定减少。例如 BM4 的 baseline min/max 为 574/797，combined 为 542/799，平均减少但本轮最大观测值增加 2 cycle。BM3 的最大观测值从 776 降至 706，BM7 从 606 降至 569；这些都是有限样本内的观测最大值，不是 WCET 上界。

BM2 的三个初始候选阶段相对基线均有增加，其中 `port` 为 +7.10%、`sched` 为 +14.91%、`combined` 为 +0.79%。源码简化、代码布局和夹具影响须结合独立对照判断，目前没有确证根因。不能把这些差异直接归因于某条指令，也不能将全部变化都归因于对应函数本身。后续对照与选择见 5.2–5.4。

### 5.1 构建体积与警告

以下来自四份 GNU size 构建输出，单位 byte，均包含相同的性能夹具。

| 阶段 | text | data | bss | data + bss |
| --- | ---: | ---: | ---: | ---: |
| baseline | 37040 | 120 | 51520 | 51640 |
| port | 36804 | 120 | 51520 | 51640 |
| sched | 36784 | 120 | 51520 | 51640 |
| combined | 36760 | 120 | 51520 | 51640 |

候选组合 text 减少 280 byte，data/bss 未增加。这里的 RAM 不是日常固件 RAM；PC 夹具的静态任务、栈等已经包含在测试镜像中。三项候选不增加生产结构体字段，未改变公共 API。

各阶段均有 25 条 `main.c` 中被该测试配置禁用的应用/故障注入代码 unused 警告；没有把已有警告写成零警告。构建证据为 [baseline](../hil/perf_20261012/build_baseline.txt)、[port](../hil/perf_20261012/build_port.txt)、[sched](../hil/perf_20261012/build_sched.txt)、[combined](../hil/perf_20261012/build_combined.txt)。

### 5.2 最终保留组合：perf 夹具完整对照

`nosched` 只保留临界区和互斥锁优化，调度器与信号量源码恢复性能基线。已测三次独立复位，11 个指标全部记录，每次 PC22 均通过。与原 perf baseline 使用相同夹具，不能与普通 bench 的基线交叉计算百分比。

| perf 指标 | baseline | nosched | 差值 / 百分比 |
| --- | ---: | ---: | ---: |
| `critical_pair` | 30 | 24 | −6 / −20.00% |
| `yield_switch` | 241 | 234 | −7 / −2.90% |
| `sem_roundtrip` | 1395 | 1607 | +212 / +15.20% |
| `sem_wake_latency` | 487 | 487 | 0 / 0.00% |
| `queue_send_wake` | 574 | 563 | −11 / −1.92% |
| `task_create_del` | 6188 | 6165 | −23 / −0.37% |
| `delay1_period` | 168000 | 167999 | −1 / 小于 0.01% |
| `isr_wake_latency` | 546 | 536 | −10 / −1.83% |
| `mutex_compute_w01` | 45 | 46 | +1 / +2.22% |
| `mutex_compute_w04` | 71 | 46 | −25 / −35.21% |
| `mutex_compute_w08` | 107 | 46 | −61 / −57.01% |

证据为 [measurement_nosched.json](../hil/perf_20261012/measurement_nosched.json)，ELF SHA-256 为 `B3EE45FD57CE5EE3AB7B6B393CD2C90A926B58D132F7388D1B2EEA9B17D998A4`。三个运行的打印 avg/min/max 一致。相对性能基线，[sources_nosched.json](../hil/perf_20261012/sources_nosched.json) 与 [sources_final_selected.json](../hil/perf_20261012/sources_final_selected.json) 都只有 `rtos_port.h`、`rtos_mutex.c` 两个文件变化。

nosched 中 BM4 最大观测值为 803，高于 baseline 的 797；ISR 最大观测值为 596，baseline 为 606。三个 mutex 组最大观测值均为 61 cycle。保留组合仍存在测量退步，不将较早 combined 中较低的 BM2 或唤醒数据替代本表。

### 5.3 最终保留组合：普通 bench 完整对照

普通 bench 不包含额外 PC 静态任务和互斥锁探针，对照使用强制重编译的 `clean_base_bench` 与 `nosched_bench`，各三次独立复位。单位和统计方式同前，均以三个 avg 的中位数表示；各阶段三个运行打印值一致。

| 普通 bench 指标 | clean_base_bench | nosched_bench | 差值 / 百分比 |
| --- | ---: | ---: | ---: |
| `critical_pair` | 30 | 24 | −6 / −20.00% |
| `yield_switch` | 241 | 235 | −6 / −2.49% |
| `sem_roundtrip` | 1393 | 1394 | +1 / +0.07% |
| `sem_wake_latency` | 488 | 482 | −6 / −1.23% |
| `queue_send_wake` | 571 | 556 | −15 / −2.63% |
| `task_create_del` | 6175 | 6121 | −54 / −0.87% |
| `delay1_period` | 167999 | 167999 | 0 / 0.00% |
| `isr_wake_latency` | 546 | 552 | +6 / +1.10% |

普通 bench 的 ISR 最大观测值为 baseline 606、nosched 619，平均和本轮最大观测值都增加。BM4 最大值分别为 810、774。两种夹具结果必须同时保留，不能只选择有利的一张表。

原始记录为 [measurement_clean_base_bench.json](../hil/perf_20261012/measurement_clean_base_bench.json) 和 [measurement_nosched_bench.json](../hil/perf_20261012/measurement_nosched_bench.json)。ELF SHA-256 分别为 `19A4A997A303BA820C1F0AAEE6B1C217135CDB2870801572DA3D0341AB43BD28`、`92A29E85E957C37F73BCB61633656C885C626CF1D652DB22956E26E653A4E4A9`。

普通 bench 的 text 从 32552 降至 32316 byte，data 120、bss 41992 byte 保持相同；perf 的 baseline/nosched text 为 37040/36780 byte、data 120、bss 51520 byte。夹具差异已经影响镜像与布局，不能将两种变体的体积或测量值混成同一个固件。

### 5.4 其他控制对照与选择依据

| 控制阶段 | 夹具 | BM2 平均 cycle | 采用判断与证据 |
| --- | --- | ---: | --- |
| clean_base_bench | bench | 1393 | 有效的普通 bench 基线 |
| clean_combined_bench | bench | 1654 | 相对同夹具基线 +18.74%，恢复原调度器；[记录](../hil/perf_20261012/measurement_clean_combined_bench.json) |
| mutex_only | perf | 1622 | 仅 mutex 的组合不保留；[记录](../hil/perf_20261012/measurement_mutex_only.json) |
| mutex_only_bench | bench | 1622 | 普通夹具也未消除 BM2 退步；[记录](../hil/perf_20261012/measurement_mutex_only_bench.json) |
| mutex_align | perf | 1631 | 对齐试验不保留；信号量唤醒最大观测值 1287；[记录](../hil/perf_20261012/measurement_mutex_align.json) |

当前选择着重于已确认的局部临界区成本减少和多等待者 PI 计算减少，同时恢复原调度器、舍弃对齐试验。不同候选与夹具表现说明结果与最终镜像布局相关，但未定位具体原因；不能据此将 BM2 的变化全部归因于 Flash 对齐或某个函数。保留组合的 perf BM2 +15.20% 与普通 bench ISR +1.10% 是实际代价，是否适合某个应用仍需按该应用工作负载评估。

## 6. 最终组合完整回归与恢复日常固件

自动执行入口为 [run_regression.ps1](../hil/perf_20261012/run_regression.ps1)。最终组合使用 `final_selected` 独立标签，汇总为 [regression_final_selected_results.json](../hil/perf_20261012/regression_final_selected_results.json)。每个变体另保存构建、下载、运行元数据、原始串口日志和对应 ELF。不同变体不能拿同一个 ELF 哈希互相代替。

| 测试 | 最终组合结果 | 证据 |
| --- | --- | --- |
| 审计专用回归，独立复位 1 | 117 PASS，0 FAIL | [audit1](../hil/perf_20261012/log_perf_20261012_final_selected_audit1.txt) |
| 审计专用回归，独立复位 2 | 117 PASS，0 FAIL | [audit2](../hil/perf_20261012/log_perf_20261012_final_selected_audit2.txt) |
| 审计专用回归，独立复位 3 | 117 PASS，0 FAIL | [audit3](../hil/perf_20261012/log_perf_20261012_final_selected_audit3.txt) |
| 基础自测 | 432 PASS，0 FAIL，10 INFO，总 442 | [selftest](../hil/perf_20261012/log_perf_20261012_final_selected_selftest.txt) |
| 边界测试 | 103 PASS，0 FAIL | [boundary](../hil/perf_20261012/log_perf_20261012_final_selected_boundary.txt) |
| 常规 BM0–BM7 | 到达 `BM\|DONE`；avg 与 nosched_bench 表相同 | [bench](../hil/perf_20261012/log_perf_20261012_final_selected_bench.txt) |
| S1 | `status=0`，20 个样本，avg/min/max 152 | [S1](../hil/perf_20261012/log_perf_20261012_final_selected_S1.txt) |
| S2 | `status=0`，give 无/1/4/8 等待者 avg 104/372/381/381 | [S2](../hil/perf_20261012/log_perf_20261012_final_selected_S2.txt) |
| S3 | `status=0`，生产/消费各 9000 项，吞吐 7895 item/s，队列延迟 avg 16267 | [S3](../hil/perf_20261012/log_perf_20261012_final_selected_S3.txt) |
| S4 | `status=0`，10000 个 2 ms 周期样本；唤醒偏差 avg 1/max 149，实际周期 avg 335983 | [S4](../hil/perf_20261012/log_perf_20261012_final_selected_S4.txt) |
| S5 | `status=0`，创建 8 个任务，每任务栈 256 word | [S5](../hil/perf_20261012/log_perf_20261012_final_selected_S5.txt) |
| PRIMASK 配置 | 13 个翻译单元编译通过，0 失败、0 警告；仅编译 | [compile.log](../hil/perf_20261012/primask_final_selected/compile.log) |

性能与场景时间数值未特别标明时均为 cycle。117 项包括调度/任务 25、IPC 15、内存/定时器 23、USB/移植层 54。三个最终 audit 运行的镜像哈希均为 `1E3DEC422D858D30A38782EA18BDC912FB399A0BF2A6A3446504FF929002764A`。审计继续覆盖真实合法优先级 IRQ 对 PendSV 决策窗口的竞争和强/弱屏蔽恢复；基础自测也包含 FPU 小栈拒绝、允许及运行回归。测试宏与模拟输入属于专用固件，不能作为生产功能启用。

最终 bench ELF 哈希为 `92A29E85E957C37F73BCB61633656C885C626CF1D652DB22956E26E653A4E4A9`，与三次 nosched_bench 测量一致，[运行记录](../hil/perf_20261012/run_perf_20261012_final_selected_bench.json) 对应实际下载和 `BM|DONE`。

### 6.1 较早 combined 候选回归保留记录

| 较早候选 | 记录的结果 | 独立证据 |
| --- | --- | --- |
| 三项 combined | audit 117 × 3、selftest 432/0/10 INFO、boundary 103/0、bench 完成、S1–S5 status=0 | [regression_combined_results.json](../hil/perf_20261012/regression_combined_results.json) |
| combined 的 PRIMASK | 13 个单元仅编译通过 | [旧 compile.log](../hil/perf_20261012/primask_compile/compile.log) |

较早 audit ELF SHA-256 为 `78FC51012EF4A70CCD775F61604D82EBDD3CE635090300C8DB033B2F28427D58`，它的通过结果保留原身份，不代替最终组合。原始四阶段性能值也继续保留在第 5 节，不把已放弃的调度器候选收益写入最终方案。

### 6.2 日常固件已恢复

测试完成后重新构建并下载 `daily` 固件，ELF SHA-256 为 `08B1A29DB171C738664D50548FCF8BA50AFDD2C86C03752CE226EA76A4EE2C16`。下载、校验与 15 秒串口捕获（整个调用约 21 秒）见 [运行元数据](../hil/perf_20261012/run_perf_20261012_daily.json)、[下载记录](../hil/perf_20261012/flash_perf_20261012_daily.txt) 和 [串口日志](../hil/perf_20261012/log_perf_20261012_daily.txt)。日志包含系统初始化、真实 DWC2 寄存器信息及周期任务持续输出，没有记录到 failure line。

该配置恢复日常应用和故障演示入口，本轮未主动触发故障演示。日常 ELF 符号记录为 [daily_symbols.txt](../hil/perf_20261012/daily_symbols.txt)，其中不包含本轮 perf 探针或 audit 专用入口。日常运行观察不是替代专用回归的额外完整测试。

## 7. 证据的适用范围与后续条件

- USB 相关结果是板上执行的 OSAL、协议输入模拟、驱动寄存器影子等逻辑测试。电脑通过 ST-LINK/SWD 和串口与板连接，本轮没有 USB 设备物理数据线到主机的枚举或实际 CDC/HID/MSC 传输验证。启用 USB 应用的 S1–S5 不能补足这一物理链路证据。
- 最终 S1 的 `block_latency` 为 20 个样本、avg/min/max 都为 152 cycle，没有证明高优先级任务真实阻塞在低优先级所有者持有的锁上并同时受到中优先级任务干扰。不能据此宣称完成实际 PI 争用性能验证。PC22 的真实阻塞等待者提供排序、继承计算及回退证据，但也不能替代该负载场景。
- S2 的统计调用把 `bad_threshold` 设为 0，统计框架仅在阈值非零且样本超过阈值时增加 bad，因此 `bad=0` 不表示达到某个延迟上限。S2 的 `status=0`、样本数和 avg/min/max 应分开解释。实现见 [test_scenarios.c](../hil/fw/Src/test_scenarios.c) 和 [test_framework.c](../hil/fw/Src/test_framework.c)。
- 最终 S3 吞吐 7895 item/s，S4 唤醒偏差 avg 1/max 149 cycle。这是单次场景观测，没有匹配基线对照，不宣称这些场景获得同等幅度的性能改善。
- PRIMASK 对最终源码使用隔离配置头文件编译 13 个内核/移植相关翻译单元，原配置头路径未混入；没有将 `RTOS_CONFIG_USE_BASEPRI=0` 固件上板运行。该结果仅确认此快照的编译兼容性。
- 三次复位和所列压力场景没有覆盖长时间 soak、全部 IRQ 相位、不同编译器、不同主频/Flash 等待配置、Cortex-M7 缓存行为或任意任务数量。观测最大值不构成严格最坏情况时延证明。
- BM2 变化根因未定位。扩展 perf 中 +15.20%、普通 bench ISR 中 +1.10%、单等待者 mutex 中 +2.22% 都是最终组合已记录的退步；实际应用应依据其工作负载衡量局部收益和这些代价，不能套用“全路径加速”的结论。

如果继续优化，先固定编译器、链接布局、夹具与源码身份，采用有效重构建镜像比较两种夹具；对 BM2 分段采样或固定相关函数布局的实验应单独归档，避免仅按最有利数字选择方案。第 3 节涉及 ABI/RAM 的索引和持锁链表方案，需要先明确最大任务/对象数量与生命周期维护成本，再决定是否值得实施。

若需形成应用级实时性结论，应另构造真实高/中/低优先级 PI 争用及多锁回退负载，为 S2 设置非零的应用延迟阈值，并补充长时间运行、PRIMASK 上板与 USB 物理链路验证。这些是后续验证条件，本轮没有把它们计作已完成测试。

本轮证据按“源码清单 → 重构建 ELF 哈希 → 实际下载记录 → 串口输出”追溯。无效控制镜像和较早阶段日志保留原身份，不覆盖或改写旧证据。
