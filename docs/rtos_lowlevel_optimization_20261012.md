# RTOS 低层与汇编优化验证记录（2026-10-12）

本轮最终保留 `isr_cached` 累积组合：PendSV 直接接收 C 返回的 TCB，调度提交正常分支直接返回所选任务，信号量恢复时复用本任务指针，等待节点摘链复用链接快照，以及信号量入口复用 ISR 状态。此前修复的生命周期、等待队列和中断保护继续保留。

相对本轮基线，最终 perf 的 BM2 从 1607 降至 1296 cycle（−19.35%），普通 bench 从 1394 降至 1307 cycle（−6.24%）。两种夹具的 yield 分别从 234/235 降至 229 cycle。结果存在负载和构建布局差异：普通 bench 的 BM5 创建删除从 6121 增至 6137 cycle（+0.26%），BM4 最大值从 774 增至 795 cycle。本报告列出全部阶段与两种夹具，不能由这些结果推导所有路径都更快。

最终组合完成新的板上审计 117 项 × 3 次、基础自测 432 项、边界测试 103 项、普通 benchmark，以及 S1–S5 场景。额外上下文检查分别在默认和统计开启配置运行三次，每次 LC 24 PASS / 0 FAIL，随后 PC 22 PASS / 0 FAIL。配置矩阵为 7 × 13 个编译单元全部通过；它是编译检查，不能算作七种配置全部上板。日常固件已经恢复、下载校验，并完成 15 秒启动观察。

## 1. 阶段范围与实现身份

本阶段从第一轮性能优化最终保留的 `nosched` 源码继续，称为“phase2 基线”。第一轮的调度选择 helper 和对齐候选均已撤回，本轮没有重新启用。第一轮报告中的 perf BM2 退步以及较早候选结果仍作为历史记录保留，见 [第一轮性能报告](rtos_optimization_20261012.md)；缺陷修复过程见 [审计报告](rtos_audit_20261012.md)。

开始时的日常 ELF SHA-256 为 `08B1A29DB171C738664D50548FCF8BA50AFDD2C86C03752CE226EA76A4EE2C16`，记录在 [daily_before.json](../hil/lowlevel_20261012/daily_before.json)。日常 ELF 与隔离性能 ELF 的任务、链接布局不同，不能用它替代本轮性能基线。

| 累积阶段 | 相对上一阶段的变化 | RTOS 源码清单 |
| --- | --- | --- |
| baseline | 第一轮最终 nosched：临界区局部计数、互斥锁有序队首，保留此前缺陷修复 | [perf](../hil/lowlevel_20261012/sources_baseline.json) / [bench](../hil/lowlevel_20261012/sources_baseline_bench.json) |
| ptr_return | internal.h、sched.c、port.c：内部返回 TCB，保留公共 void 包装 | [perf](../hil/lowlevel_20261012/sources_ptr_return.json) / [bench](../hil/lowlevel_20261012/sources_ptr_return_bench.json) |
| ptr_direct | sched.c：正常提交分支直接 return next | [perf](../hil/lowlevel_20261012/sources_ptr_direct.json) / [bench](../hil/lowlevel_20261012/sources_ptr_direct_bench.json) |
| sem_cur | sem.c：阻塞返回复用本任务 cur 指针 | [perf](../hil/lowlevel_20261012/sources_sem_cur.json) / [bench](../hil/lowlevel_20261012/sources_sem_cur_bench.json) |
| wait_cache | task.c：摘链前快照 next/prev | [perf](../hil/lowlevel_20261012/sources_wait_cache.json) / [bench](../hil/lowlevel_20261012/sources_wait_cache_bench.json) |
| isr_cached | sem.c、config.h：复用入口 ISR 状态，保持优先级断言 | [perf](../hil/lowlevel_20261012/sources_isr_cached.json) / [bench](../hil/lowlevel_20261012/sources_isr_cached_bench.json) |

源码清单覆盖 RTOS 下 61 个 C/H 文件；阶段副本位于 `hil/lowlevel_20261012/baseline_RTOS/`（仅本地归档，不入库），最初清单为 [sources_before.json](../hil/lowlevel_20261012/sources_before.json)。最终相对本轮基线实际变化的是六个生产文件：`rtos_config.h`、`rtos_internal.h`、`rtos_sched.c`、`rtos_sem.c`、`rtos_task.c`、`port.c`。没有给 TCB、等待节点或 IPC 对象增加字段，没有新增生产动态分配，也没有修改公共 void 调度接口 ABI。

每个性能阶段的三次独立复位使用同一 ELF。源码 SHA 标识实现，ELF SHA 标识实际镜像；两者不能互相替代。

| 阶段 | perf ELF SHA-256 | bench ELF SHA-256 |
| --- | --- | --- |
| baseline | `B3EE45FD57CE5EE3AB7B6B393CD2C90A926B58D132F7388D1B2EEA9B17D998A4` | `92A29E85E957C37F73BCB61633656C885C626CF1D652DB22956E26E653A4E4A9` |
| ptr_return | `057A120C4322869AFF1E02E9FC0383EF9475D40F7E8D26052641B7E2AA98F2D9` | `DF76BF6A39B1A3416F20BADA77BBE768C602B2D6194DF90B9F0F48FE4A699829` |
| ptr_direct | `210B0ED46504BE0872453743BA191317D53C8DFA84F51D40EECB3351E0CF8B13` | `AA1D207086E5BFBBCA76CA1C531D6A0297223B492CB6EBFCBE91A124CC72944F` |
| sem_cur | `8690136A5FF044F1808DC18179717DCE8EC7F9F676CEBF70DCFEFB7D008157A2` | `D12C3EE5AAE2AF631291C414E4874864DAF2C81339B19FBBF303FC762C2DE76F` |
| wait_cache | `DE054E6B8EF9489CB1EBAA411B6A49196EBA86C007D2B64A9BFB8E49BA615AB6` | `D3BA014BEABE65D010A2216037C1E4786553BFEB7329487440B5DC2870BFDA63` |
| isr_cached | `40AECCDD864DA39CD8C574D2BF0D185E504B8CAE387C8927804326A729968D6D` | `69C64789185A9404E525E2F5059B397F371E0EB10B47893D67EAF17A4D1AC5C0` |

修改和专用夹具按用户本地提供的 `c-code-style/SKILL.md` 规范评审；该规范属于外部用户配置文件，不随仓库发布。评审关注命名、括号、4 空格、100 列、定宽类型、头文件自包含、接口注释和内嵌汇编约束。此处只描述本轮改动的约束，不宣称整个历史工程已完成全量风格改写。

## 2. 保留的实现与真实机器码

### 2.1 PendSV 使用 C 返回值

新增内部 `rtos_sched_commit_context_switch()`，指针按 AAPCS 从 R0 返回；原 `void rtos_sched_context_switch()` 保留为调用内部函数并丢弃返回值的包装。内部函数在调度器非 RUNNING 时清空 `next_tcb` 并返回原 current；没有候选时返回原 current；正常提交后返回快照中的 next。正常提交仍保留原断言、任务状态更新、切换计数、全局 current 更新以及可选统计钩子。自切路径保留原有计数语义。

PendSV 保存原任务栈后调用新内部入口，直接从 R0 指向的 TCB 读取 `top_of_stack`。机器码变化为：

```asm
; 基线：C 调用后重新加载全局 current，再取栈
bl    rtos_sched_context_switch
ldr   r1, =rtos_kernel
ldr.w r1, [r1, #144]
ldr   r0, [r1, #0]

; 最终：C 返回所选 TCB
bl    rtos_sched_commit_context_switch
ldr   r0, [r0, #0]
```

实际减少调用后的两条 LDR，不减少保存原任务时读取 current 的两条加载。偏移 `144` 和 `0` 是这一 Cortex-M4 构建的布局，源码继续使用编译期结构偏移。参见 [基线移植层反汇编](../hil/lowlevel_20261012/baseline_port.disassembly.txt) 与 [最终移植层反汇编](../hil/lowlevel_20261012/isr_cached_port.disassembly.txt)。

正常分支直接 `return next` 的 `ptr_direct` 阶段进一步改善 C 返回路径；有无收益以最终板上结果判断，不能把删掉一条指令简单换算成固定 cycle。

### 2.2 保护窗口、ABI 与浮点上下文

改动保留进入前 BASEPRI/PRIMASK 的保存与恢复、BASEPRI_MAX、屏障，以及从读取 current、C 提交到安装新 PSP 的保护窗口。MSP 上仍保存两个字的 `{r2,r3}`，保持 C 调用时 8 字节对齐。任务栈上的 R4–R11、EXC_RETURN，以及按 EXC_RETURN 位 4 判断的 S16–S31 保存恢复路径保持原顺序。不能为了少一条指令删除此保护或把原屏蔽值假定为零。

真实 PendSV 入口原本就要求安装了有效 current；NULL 返回用于内部未启动分支，并不意味着汇编增加了 NULL 安全恢复路径。专用检查在临界区内直接调用内部函数，不安装 PSP，也不启动到测试借用的 TCB。

统计开启版本会在提交后调用 `rtos_perf_on_context_switch`，因此不能假定 R0 跨该调用保持。已人工核对最终统计 ELF：`0x080047f0` 将 next 保存到 R5，`0x080047f6` 调用统计钩子，`0x080047fa` 恢复 R0=R5，再返回。证据为 [统计开启调度器反汇编](../hil/lowlevel_20261012/checks_final_stats_rtos_sched.disassembly.txt)、[人工返回路径核验](../hil/lowlevel_20261012/stats_return_manual_review.json) 和第 5 节三次真实运行。矩阵工具的静态识别状态仍写为 `manual_review_required`；这里给出了人工检查的具体依据。

依据 [Arm AAPCS32](https://github.com/ARM-software/abi-aa/blob/main/aapcs32/aapcs32.rst) 第 6.4 节，单字返回值使用 R0；第 6.2.1.2 节规定公共调用处栈按 8 字节对齐，第 6.1.2.1 节规定 S16–S31 跨子程序调用保持。此处用实际机器码和混合任务检查这些约定的落地情况。

### 2.3 信号量阻塞恢复复用 cur

`rtos_sem_take` 在进入阻塞前已取得本任务的 cur。任务被切换出去后，该指针随 callee-saved 寄存器恢复；返回此调用时读取 `cur->wait_result` 即可。已经删除的任务不会再恢复并继续这次调用，因此不能用该优化为提前释放仍活跃 TCB 辩护。任务身份和生命周期 guard 均继续保留。

基线恢复路径为 `ldr.w r3,[r9,#144]` 后读 wait_result；最终直接使用 R4 读取 wait_result，省去重新读取全局 current。此构建的函数保存集合从八个寄存器 32 B 降到六个寄存器 24 B。它是 `rtos_sem_take` 的编译器序言保存量，不代表整个任务栈或嵌套调用最大栈深度减少 8 B。参见 [基线信号量反汇编](../hil/lowlevel_20261012/baseline_rtos_sem.disassembly.txt) / [最终信号量反汇编](../hil/lowlevel_20261012/isr_cached_rtos_sem.disassembly.txt)。

### 2.4 摘链前读取 next 和 prev

等待节点摘链前把 next/prev 读取到局部变量，再更新前驱、后继或队首，并清空被移除节点的链接。合法等待链由调用方在临界区内维护；不能以该优化允许损坏链、自环节点或未注册节点参与摘链。头部、中间、尾部以及同步优先级位图语义继续保留。

基线中间节点路径四次读取 next/prev；最终编译为 `ldrd r3,r2,[r1,#20]` 取得两个 32 位链接，并用于后续更新。相同 helper 的内联路径也有相应变化。LDRD 是双字加载指令，不能据此声称只读一个字、原子更新整条链或固定只花一个 cycle。参见 [基线任务反汇编](../hil/lowlevel_20261012/baseline_rtos_task.disassembly.txt) / [最终任务反汇编](../hil/lowlevel_20261012/isr_cached_rtos_task.disassembly.txt)。

### 2.5 ISR 状态读取一次，保留完整 guard

信号量入口先读取 ISR 状态，供优先级断言和“ISR 不允许阻塞”的判断共用。新增 `RTOS_ASSERT_ISR_STATE_OK(in_isr)` 宏；原 `RTOS_ASSERT_ISR_OK()` 仍通过它执行同一优先级检查。断言开启时 ISR 路径仍调用 `rtos_port_isr_priority()` 并比较系统调用阈值；断言关闭时 ISR 阻塞错误检查仍存在。

这里减少的是该函数线程路径上的一次重复 `MRS IPSR`：基线有两条，最终只有入口处一条。ISR 优先级查询自己的硬件读取没有被删除，不能描述为整个 ISR API 只读取一次异常状态。线程执行被中断后恢复，调用本身仍处于原线程上下文，因此入口状态可在这次调用中复用。证据见信号量反汇编和 [断言宏源码](../RTOS/Core/Inc/rtos_config.h)。

## 3. 测量环境与口径

真实环境为同一块 NUCLEO-F446RE / STM32F446、168 MHz Cortex-M4，GNU Arm 11.3.rel1，`-O2`、`fpv4-sp-d16`、hard-float。通过 ST-LINK/SWD 下载并校验，COM3 / 115200 捕获串口。DWT CYCCNT 记录 cycle；目标测量区间不包含串口打印。

`perf` 是增加 PC 互斥锁检查和 PB 微基准的隔离固件；名称不表示开启 `RTOS_CONFIG_PERF_HOTPATH_STATS`。普通 `bench` 没有 PC/PB 扩展。两者链接布局不同，所有百分比都使用本变体本轮 baseline 作分母，不混用另一夹具或上一轮历史镜像。

一次最终 perf 完整执行的证据为 [下载校验记录](../hil/lowlevel_20261012/flash_lowlevel_20261012_isr_cached_r1.txt)、[run 身份](../hil/lowlevel_20261012/run_lowlevel_20261012_isr_cached_r1.json)、[原始串口](../hil/lowlevel_20261012/log_lowlevel_20261012_isr_cached_r1.txt)。各阶段的 r2/r3 均有各自 flash/run/log，不是将同一次输出重复计数。汇总由 [run_measurement.ps1](../hil/lowlevel_20261012/run_measurement.ps1) 从日志生成，构建时强制刷新相关对象，防止旧宏或旧实现残留。

| 指标 | 日志 N | 真正统计单位 |
| --- | --- | --- |
| BM0 critical_pair | 1 | 10000 对临界区进出后的批量平均 |
| BM1 yield_switch | 1 | 20000 次 yield 的批量平均 |
| BM2 sem_roundtrip | 1 | 5000 round 的批量平均；round 包含 A/B 信号量 take/give 和切换 |
| BM3 sem_wake_latency | 1000 | 1000 次信号量唤醒测量 |
| BM4 queue_send_wake | 5000 | 5000 次队列发送唤醒测量 |
| BM5 task_create_del | 1 | 500 次创建删除的批量平均 |
| BM6 delay1_period | 1 | 500 次 delay(1) 的批量平均 |
| BM7 isr_wake_latency | 100 | 100 次 ISR 唤醒测量 |
| PB mutex_compute_w01/w04/w08 | 各 1000 | 有 1/4/8 等待者时，1000 次有效优先级计算 |

BM 的 N=1 不表示仅执行一次底层操作。此类 min/max 与 avg 相同，因为一次日志只提交一个已平均的批次；不能作为单次操作极值或 WCET。BM3/4/7 和 PB 的 max 也只是已观测样本最大值，不是最坏时间上界。各阶段三次日志的整数汇总值完全一致，只能说明这个夹具下输出重复，不能推出任意输入的零抖动。

## 4. 全部阶段结果

以下单位都是 cycle，每个数格为 **avg/min/max**。十二份 measurement JSON 全部列入，分别包含三次运行的原始解析值、N、镜像 SHA 和 PC 检查数量。表格不把相同三次汇总再做一个虚假的独立大样本池。

### 4.1 perf，六个累积阶段

| 阶段 / 测量记录 | BM0 临界区 | BM1 yield | BM2 信号量往返 | BM3 信号量唤醒 | BM4 队列唤醒 | BM5 创建删除 | BM6 delay(1) | BM7 ISR 唤醒 | PB 锁 w01 | PB 锁 w04 | PB 锁 w08 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| [baseline](../hil/lowlevel_20261012/measurement_baseline.json) | 24/24/24 | 234/234/234 | 1607/1607/1607 | 487/486/752 | 563/563/803 | 6165/6165/6165 | 167999/167999/167999 | 536/532/596 | 46/46/61 | 46/46/61 | 46/46/61 |
| [ptr_return](../hil/lowlevel_20261012/measurement_ptr_return.json) | 24/24/24 | 232/232/232 | 1562/1562/1562 | 466/462/685 | 557/557/762 | 6126/6126/6126 | 168000/168000/168000 | 532/527/601 | 46/46/61 | 46/46/61 | 46/46/61 |
| [ptr_direct](../hil/lowlevel_20261012/measurement_ptr_direct.json) | 24/24/24 | 229/229/229 | 1563/1563/1563 | 459/454/732 | 554/549/794 | 6127/6127/6127 | 168000/168000/168000 | 527/518/603 | 46/46/61 | 46/46/61 | 46/46/61 |
| [sem_cur](../hil/lowlevel_20261012/measurement_sem_cur.json) | 24/24/24 | 229/229/229 | 1347/1347/1347 | 444/444/697 | 556/556/794 | 6151/6151/6151 | 167999/167999/167999 | 508/505/575 | 46/46/59 | 46/46/59 | 46/46/59 |
| [wait_cache](../hil/lowlevel_20261012/measurement_wait_cache.json) | 24/24/24 | 229/229/229 | 1525/1525/1525 | 449/443/1261 | 559/553/790 | 6149/6149/6149 | 168000/168000/168000 | 507/502/577 | 46/46/61 | 46/46/61 | 46/46/61 |
| [isr_cached](../hil/lowlevel_20261012/measurement_isr_cached.json) | 24/24/24 | 229/229/229 | 1296/1296/1296 | 443/443/709 | 553/553/758 | 6135/6135/6135 | 168000/168000/168000 | 509/504/586 | 46/46/59 | 46/46/59 | 46/46/59 |

每个 perf 阶段的三次运行均有 PC 22 PASS / 0 FAIL。PB w01/w04/w08 的平均值均为 46 cycle，第一轮保留的互斥锁算法没有再次改动。wait_cache 的 BM3 样本最大值 1261 cycle 高于基线 752，不能只展示它平均值下降；最终阶段为 709。

### 4.2 普通 bench，六个累积阶段

| 阶段 / 测量记录 | BM0 临界区 | BM1 yield | BM2 信号量往返 | BM3 信号量唤醒 | BM4 队列唤醒 | BM5 创建删除 | BM6 delay(1) | BM7 ISR 唤醒 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| [baseline_bench](../hil/lowlevel_20261012/measurement_baseline_bench.json) | 24/24/24 | 235/235/235 | 1394/1394/1394 | 482/482/759 | 556/556/774 | 6121/6121/6121 | 167999/167999/167999 | 552/547/619 |
| [ptr_return_bench](../hil/lowlevel_20261012/measurement_ptr_return_bench.json) | 24/24/24 | 232/232/232 | 1392/1392/1392 | 474/473/699 | 551/551/795 | 6132/6132/6132 | 167999/167999/167999 | 540/529/606 |
| [ptr_direct_bench](../hil/lowlevel_20261012/measurement_ptr_direct_bench.json) | 24/24/24 | 229/229/229 | 1397/1397/1397 | 470/464/743 | 548/548/794 | 6132/6132/6132 | 167999/167999/167999 | 529/525/608 |
| [sem_cur_bench](../hil/lowlevel_20261012/measurement_sem_cur_bench.json) | 24/24/24 | 229/229/229 | 1417/1417/1417 | 488/487/767 | 554/553/777 | 6156/6156/6156 | 168000/168000/168000 | 542/538/609 |
| [wait_cache_bench](../hil/lowlevel_20261012/measurement_wait_cache_bench.json) | 24/24/24 | 229/229/229 | 1409/1409/1409 | 488/488/716 | 539/539/787 | 6158/6158/6158 | 168000/168000/168000 | 535/531/602 |
| [isr_cached_bench](../hil/lowlevel_20261012/measurement_isr_cached_bench.json) | 24/24/24 | 229/229/229 | 1307/1307/1307 | 456/455/731 | 556/549/795 | 6137/6137/6137 | 168000/168000/168000 | 529/525/597 |

这组没有 PC/PB 夹具。sem_cur 和 wait_cache 中间阶段的 BM2 分别为 1417/1409，高于其 baseline 1394；最终 isr_cached 为 1307。指令减少和阶段平均收益并非单调关系。

### 4.3 最终 perf 对本阶段 perf 基线

基线 [measurement_baseline.json](../hil/lowlevel_20261012/measurement_baseline.json)，最终 [measurement_isr_cached.json](../hil/lowlevel_20261012/measurement_isr_cached.json)。负数表示测量减少。

| 指标 | N | 阶段基线 avg/min/max | 最终 avg/min/max | 平均变化 cycle | 平均变化 % |
| --- | --- | --- | --- | --- | --- |
| BM0 临界区 | 1 | 24/24/24 | 24/24/24 | 0 | 0.00% |
| BM1 yield | 1 | 234/234/234 | 229/229/229 | -5 | -2.14% |
| BM2 信号量往返 | 1 | 1607/1607/1607 | 1296/1296/1296 | -311 | -19.35% |
| BM3 信号量唤醒 | 1000 | 487/486/752 | 443/443/709 | -44 | -9.03% |
| BM4 队列唤醒 | 5000 | 563/563/803 | 553/553/758 | -10 | -1.78% |
| BM5 创建删除 | 1 | 6165/6165/6165 | 6135/6135/6135 | -30 | -0.49% |
| BM6 delay(1) | 1 | 167999/167999/167999 | 168000/168000/168000 | +1 | +0.00% |
| BM7 ISR 唤醒 | 100 | 536/532/596 | 509/504/586 | -27 | -5.04% |
| PB 锁 w01 | 1000 | 46/46/61 | 46/46/59 | 0 | 0.00% |
| PB 锁 w04 | 1000 | 46/46/61 | 46/46/59 | 0 | 0.00% |
| PB 锁 w08 | 1000 | 46/46/61 | 46/46/59 | 0 | 0.00% |

### 4.4 最终 bench 对本阶段 bench 基线

基线 [measurement_baseline_bench.json](../hil/lowlevel_20261012/measurement_baseline_bench.json)，最终 [measurement_isr_cached_bench.json](../hil/lowlevel_20261012/measurement_isr_cached_bench.json)。

| 指标 | N | 阶段基线 avg/min/max | 最终 avg/min/max | 平均变化 cycle | 平均变化 % |
| --- | --- | --- | --- | --- | --- |
| BM0 临界区 | 1 | 24/24/24 | 24/24/24 | 0 | 0.00% |
| BM1 yield | 1 | 235/235/235 | 229/229/229 | -6 | -2.55% |
| BM2 信号量往返 | 1 | 1394/1394/1394 | 1307/1307/1307 | -87 | -6.24% |
| BM3 信号量唤醒 | 1000 | 482/482/759 | 456/455/731 | -26 | -5.39% |
| BM4 队列唤醒 | 5000 | 556/556/774 | 556/549/795 | 0 | 0.00% |
| BM5 创建删除 | 1 | 6121/6121/6121 | 6137/6137/6137 | +16 | +0.26% |
| BM6 delay(1) | 1 | 167999/167999/167999 | 168000/168000/168000 | +1 | +0.00% |
| BM7 ISR 唤醒 | 100 | 552/547/619 | 529/525/597 | -23 | -4.17% |

BM6 的 +1 cycle 是 500 次 tick 延时批平均的整数结果，不可解读为 tick 配置改变。普通 bench 的 BM4 平均不变、max 增加，BM5 平均也有小幅增加；这些观测保留，不以“总体更快”掩盖。不同夹具 BM2 差距及其阶段变化仍说明构建布局和 Flash 取指效果值得关注，第 7 节只给出同一诊断镜像内的缓存对照证据。

## 5. 新增上下文健壮性检查

专用文件为 [lowlevel_checks.h](../hil/fw/Inc/lowlevel_checks.h)、[lowlevel_checks.c](../hil/fw/Src/lowlevel_checks.c)、[lowlevel_fpu_probe.c](../hil/fw/Port/ARM/lowlevel_fpu_probe.c)。在普通 BM7 完成后运行 LC，再运行原 PC 22。汇总 [checks_context_verified_results.json](../hil/lowlevel_20261012/checks_context_verified_results.json) 记录默认/统计开启配置各三次。

| 配置 | 运行数 / 结果 | 同一组三次 ELF SHA-256 | RTOS 源码清单 |
| --- | --- | --- | --- |
| 默认，HOTPATH_STATS=0 | LC 24/0 × 3，PC 22/0 × 3 | `7C474D473324DD8725EAF6FDC6E09F240EC3430B2408328CC163B2F08EEADC7A` | [sources_checks_final_lowlevel.json](../hil/lowlevel_20261012/sources_checks_final_lowlevel.json) |
| 统计开启，HOTPATH_STATS=1 | LC 24/0 × 3，PC 22/0 × 3 | `138BD6DA82012C2D081E24AF2B93EB7ED79AB1E2260F3135526C14C5328D8E93` | [sources_checks_final_stats.json](../hil/lowlevel_20261012/sources_checks_final_stats.json) |

两份源码清单各含 61 个 RTOS C/H，差别只在统计开关对应的 config.h。夹具位于 HIL，不包含在上述 RTOS 清单内；本次读取的三个夹具 SHA-256 如下，以免把内核清单误当夹具清单：

| 夹具 | SHA-256 |
| --- | --- |
| lowlevel_checks.h | `4F77939D09A2D63DDAB37D75CA12DE76F95A6B30623C032A10EDDD0230D1ACC7` |
| lowlevel_checks.c | `9B18DB61773EA8A04CB76D3562F484E78C3BB7FD2638C85ED2FDE308D9D60586` |
| lowlevel_fpu_probe.c | `71286BF7C05FE712020E3FDC6FEAFD12D7ABAFA6723EC8020CA0826CF566F7CC` |

### 5.1 内部返回分支与公共 void ABI

十个直接检查覆盖未启动且 NULL current、未启动但有 current、SUSPENDED、STOPPED、RUNNING 无 next、自切、正常切入 READY peer、非 RUNNING prev 保持原状态、NULL prev 防御分支，以及原 public void 函数指针的调用兼容和提交效果。

控制任务处于真实运行上下文，三个静态 TCB 先由公共接口合法注册。检查在临界区内保存整个 kernel 和所涉及的 current/peer TCB，再暂借这些状态直接调用 helper；结束前恢复全部快照，包括统计字段。不会执行到借用 peer 的真实上下文，不安装伪 PSP，也不依赖未初始化 TCB。此变体没有启用审计 PendSV 注入 hook，避免直接分支检查触发外部注入。

### 5.2 真实 S16–S31 混合切换

两个 FPU 任务使用不同的 16 个 uint32_t 位模式，只在进入任务时一次 VLDM 到 S16–S31。每轮先调用 C helper 执行 yield，每 64 轮执行 delay(1)，然后 VSTM 真实寄存器到 observed[16]。整数 C 代码逐位比较 expected/observed，不依赖 `volatile float` 被编译器偶然留在寄存器里。

最终默认 ELF 的 VLDM 在 `0x08007714`，循环从 `0x08007718` 开始，VSTM 在 `0x08007722`，回跳目标跳过 VLDM；统计开启 ELF 对应位置为 VLDM `0x08007750`、循环 `0x08007754`、VSTM `0x0800775e`。因此测试不会在每轮比较前重新加载期望值来掩盖破坏。专用 naked 汇编任务在 Port/ARM，带 Cortex-M4/FPU 宏 guard，8 字节任务栈和 `push {r4,lr}` 维持 AAPCS；任务入口 LR 保存为普通任务返回地址，PendSV 的 EXC_RETURN 继续由移植层单独保存。

另有不使用浮点的整数 peer，执行 10000 轮并检查最终值。控制任务优先级 9，三个测试任务优先级 10，先完成创建再开始工作；控制任务使用有限超时等待并检查完成标志和删除结果；子任务完成后主动调用挂起，夹具没有单独断言其挂起状态。共享进度在临界区读写。

六次日志均显示 `rounds_a=10000 rounds_b=10000 integer=10000 errors_a=0 errors_b=0`。这是每个任务 10000 轮调度/寄存器比较，不能声称每个 yield 都必然发生上下文切换，或已穷尽 FPU 所有异常、NaN、故障与嵌套情况。

| 配置 | 三份独立串口日志 |
| --- | --- |
| 默认 | [r1](../hil/lowlevel_20261012/log_lowlevel_20261012_checks_final_lowlevel_r1.txt) / [r2](../hil/lowlevel_20261012/log_lowlevel_20261012_checks_final_lowlevel_r2.txt) / [r3](../hil/lowlevel_20261012/log_lowlevel_20261012_checks_final_lowlevel_r3.txt) |
| 统计开启 | [r1](../hil/lowlevel_20261012/log_lowlevel_20261012_checks_final_stats_r1.txt) / [r2](../hil/lowlevel_20261012/log_lowlevel_20261012_checks_final_stats_r2.txt) / [r3](../hil/lowlevel_20261012/log_lowlevel_20261012_checks_final_stats_r3.txt) |

编译和保存的 probe 机器码见 [默认 probe](../hil/lowlevel_20261012/checks_final_lowlevel_lowlevel_fpu_probe.disassembly.txt) / [统计开启 probe](../hil/lowlevel_20261012/checks_final_stats_lowlevel_fpu_probe.disassembly.txt)；实际链接 ELF 为 `hil/lowlevel_20261012/firmware_lowlevel_20261012_checks_final_lowlevel_r1.elf`（仅本地归档，不入库） / `hil/lowlevel_20261012/firmware_lowlevel_20261012_checks_final_stats_r1.elf`（仅本地归档，不入库）。新夹具构建规则采用 `-Wall -Wextra -Werror`；默认和统计测试最初的六份镜像使用原 `-Wall` 规则，随后对三个新夹具及两种基准集成分别补做了五次独立严格编译，均无警告，见 [fixture_compile/results.json](../hil/lowlevel_20261012/fixture_compile/results.json)。历史 main.c 的未使用变体函数警告不能与下一节内核编译矩阵的零警告混为一谈。

## 6. 最终组合的完整回归与编译矩阵

[证据总校验](../hil/lowlevel_20261012/evidence_validation.json) 包含本阶段 60 次下载捕获：57 次正常完成，包括十二阶段性能 × 3=36 次、完整回归 11 次、LC 默认/统计开启 6 次、最终 Flash 诊断 3 次和 daily 1 次。另外 3 次是首次动态诊断失败及其两次故障现场复核，明确排除通过计数，不能写成“60 次全部 PASS”。保存 ELF 哈希均匹配，最终源码与 daily 清单一致。

### 6.1 真实硬件回归

本次新的 [regression_final_selected_results.json](../hil/lowlevel_20261012/regression_final_selected_results.json) 记录以下结果；没有将上一轮的已通过次数计入本轮。

| 套件 | 结果 |
| --- | --- |
| 专项审计 audit | 同一 ELF 三次，每次 117 PASS / 0 FAIL；SCHEDTASK 25、IPC 15、MEMTIMER 23、USBPORT 54 |
| 原始 selftest | 432 PASS / 0 FAIL / 10 INFO，total=442 |
| 边界 boundary | 103 PASS / 0 FAIL |
| 普通 benchmark | BM0–BM7 完成并输出 BM DONE |
| scenario_usb S1–S5 | 五个场景均 SC DONE / status=0 |
| 新 lowlevel 默认与 stats | 每配置三次，LC 24/0 × 6，总共 144 个通过；随后 PC 22/0 × 6 |
| Flash 同镜像缓存诊断 | 三次 FD 7/0，详见第 7 节；不混计 LC/PC |

所有下面的 run 元数据均记录真实下载成功、结束行出现、failure_lines 为空。专项审计 r2/r3 与 audit1 的 SHA 相同，其独立日志为 [audit2](../hil/lowlevel_20261012/log_lowlevel_20261012_final_selected_audit2.txt) / [audit3](../hil/lowlevel_20261012/log_lowlevel_20261012_final_selected_audit3.txt)。

| 回归镜像 | ELF SHA-256 | 运行身份 / 原始日志 |
| --- | --- | --- |
| audit1 | `B3FEFBE65DA21911FCDAD67739B4582321F14C4E89C8080863133221BC99DD4F` | [run](../hil/lowlevel_20261012/run_lowlevel_20261012_final_selected_audit1.json) / [log](../hil/lowlevel_20261012/log_lowlevel_20261012_final_selected_audit1.txt) |
| selftest | `A4FEDB8C592B07BFEE4DB2DA648A8C042804A39BFEAF8E9B63F6BE9E87F96822` | [run](../hil/lowlevel_20261012/run_lowlevel_20261012_final_selected_selftest.json) / [log](../hil/lowlevel_20261012/log_lowlevel_20261012_final_selected_selftest.txt) |
| boundary | `39BD25256429C2DA0C57ACB0DEE6C02B08AEA90B1D3B7407ED584A37B36F3AD0` | [run](../hil/lowlevel_20261012/run_lowlevel_20261012_final_selected_boundary.json) / [log](../hil/lowlevel_20261012/log_lowlevel_20261012_final_selected_boundary.txt) |
| bench | `69C64789185A9404E525E2F5059B397F371E0EB10B47893D67EAF17A4D1AC5C0` | [run](../hil/lowlevel_20261012/run_lowlevel_20261012_final_selected_bench.json) / [log](../hil/lowlevel_20261012/log_lowlevel_20261012_final_selected_bench.txt) |
| S1 | `9C3C07067F779AE5AF6D5E1FD3839DBF27ADD5F0470438CC925EE59CC2A6D782` | [run](../hil/lowlevel_20261012/run_lowlevel_20261012_final_selected_S1.json) / [log](../hil/lowlevel_20261012/log_lowlevel_20261012_final_selected_S1.txt) |
| S2 | `711134908728C1C3476E6D5E6CC642DB0F94E32CE8FE9AD0CB8BA03C6C6894E4` | [run](../hil/lowlevel_20261012/run_lowlevel_20261012_final_selected_S2.json) / [log](../hil/lowlevel_20261012/log_lowlevel_20261012_final_selected_S2.txt) |
| S3 | `3533D7E50738BAA318D5E9FE6109CFC0F08C8361E8B70768698637E50C8BDE97` | [run](../hil/lowlevel_20261012/run_lowlevel_20261012_final_selected_S3.json) / [log](../hil/lowlevel_20261012/log_lowlevel_20261012_final_selected_S3.txt) |
| S4 | `47A29A345384B73F323D069BC07BEBAFC897D5FE4B6F2172832381D6594E0C64` | [run](../hil/lowlevel_20261012/run_lowlevel_20261012_final_selected_S4.json) / [log](../hil/lowlevel_20261012/log_lowlevel_20261012_final_selected_S4.txt) |
| S5 | `3508F01B019FB90F294726E9411E1CD22B9DDBC41F74648C020E281F2158F7E8` | [run](../hil/lowlevel_20261012/run_lowlevel_20261012_final_selected_S5.json) / [log](../hil/lowlevel_20261012/log_lowlevel_20261012_final_selected_S5.txt) |

S1 日志仅有 20 个 158 cycle 的取锁样本，结合当前任务启动次序，未人为制造“高优先级等待者已阻塞在低优先级持锁者上”的稳定 PI 争用。这些样本不能用于宣称消除了优先级反转；互斥锁优先级恢复和多等待者行为由专项审计与 PC 检查另行覆盖。

S2 的 `bad=0` 不表示满足某个时延阈值：当前 test_stats 使用 threshold=0 关闭超阈值计数。S3 生产/消费计数均为 9000，吞吐为 7895 items/s，但控制周期观察到 min/max 335940/1512530 cycle；S4 记录 10000 个样本，周期 min/max 168054/336164；场景 status=0 只表示夹具完成，不能替代实时 deadline 证明。S5 成功创建八个任务，每栈 256 words。

`scenario_usb` 保留 USB 应用及初始化输出，但本轮没有连接并测试物理 USB 主机/设备数据传输、枚举协议完整性、吞吐或拔插稳定性。USBPORT 是软件接口和合法 IRQ 的专项测试，不是物理 USB 总线认证或端到端外设测试。

### 6.2 独立 ARM 编译矩阵

[最终矩阵 JSON](../hil/lowlevel_20261012/config_matrix_final/matrix.json) 记录七种配置，每种 13 个内核/移植/USB 适配单元，合计 91 个真实 ARM 编译任务。全部 exitCode=0、warning=0、error=0，使用 `-Wall -Wextra -Werror`。

| 配置 | 优化级别 | BASEPRI | 抢占 | 断言 | HOTPATH_STATS | 单元结果 |
| --- | --- | --- | --- | --- | --- | --- |
| default_o2 | -O2 | 1 | 1 | 1 | 0 | 13/13 |
| perf_stats_o2 | -O2 | 1 | 1 | 1 | 1 | 13/13 |
| primask_o2 | -O2 | 0 | 1 | 1 | 0 | 13/13 |
| cooperative_o2 | -O2 | 1 | 0 | 1 | 0 | 13/13 |
| assert_off_o2 | -O2 | 1 | 1 | 0 | 0 | 13/13 |
| default_o0 | -O0 | 1 | 1 | 1 | 0 | 13/13 |
| default_o3 | -O3 | 1 | 1 | 1 | 0 | 13/13 |

矩阵从独立 Input/Inc/Src 快照编译，不覆盖生产配置或共享 Debug 对象。执行记录为 2026-10-12 12:47:50–12:48:05 UTC；脚本 [compile_configs.ps1](../hil/lowlevel_20261012/compile_configs.ps1) 保存命令、宏、输入 SHA、日志和关键反汇编。13 单元为 port、sched、task、sem、mutex、queue、event、heap、mem、timer、perf、usb、usb_osal。

默认和统计开启配置已经做本节/第 5 节所述上板；PRIMASK、协作调度、断言关闭、-O0、-O3 在本阶段只有矩阵编译证据，未声称板上功能或时延验证。七个单点配置也不是所有宏组合的笛卡尔积测试。矩阵顶层 `compiled_manual_asm_review_required` 表示编译完成且仍保留人工机器码检查要求，不能改述为形式化验证完成。

## 7. 同一 ELF 的 Flash 缓存诊断与失败保留

### 7.1 最终静态回调诊断

首次动态创建删除的诊断因严格堆快照变化而失败后，新增隔离静态任务回调，避免该性能对照自身改变堆链布局。成功记录 [checks_static_flash_results.json](../hil/lowlevel_20261012/checks_static_flash_results.json) 为同一 ELF 的三次独立复位，SHA-256 为 `59C7EB70BB3488E29E9B24AC4E6256186F290FA788784B174E5637C86D3AF74A`；源码清单为 [sources_checks_static_flash_flash.json](../hil/lowlevel_20261012/sources_checks_static_flash_flash.json)。

| 同镜像模式顺序 | 实读 FLASH ACR | BM2 批平均 cycle | 每次快照 heap_before/after | 结果 |
| --- | --- | --- | --- | --- |
| ICACHE_ON_FIRST | 0x00000705 | 1229 | 28496 / 28496 | 三次均完成 |
| ICACHE_OFF | 0x00000505 | 1750 | 28496 / 28496 | 三次均完成 |
| ICACHE_ON_AGAIN | 0x00000705 | 1229 | 28496 / 28496 | 三次均完成 |

三次均 FD SUMMARY pass=7 fail=0 / status=0，最后 ACR 恢复为保存值 0x705。记录显示每模式任务数和 ready bitmap 不变、注册表有效，tick 正常推进。[缓存对照解析](../hil/lowlevel_20261012/flash_cache_comparison.json) 将每个 FD MODE 区间内恰好一条 BM2 与该模式、ACR、堆快照和镜像 SHA 对应。三份日志为 [r1](../hil/lowlevel_20261012/log_lowlevel_20261012_checks_static_flash_flash_r1.txt) / [r2](../hil/lowlevel_20261012/log_lowlevel_20261012_checks_static_flash_flash_r2.txt) / [r3](../hil/lowlevel_20261012/log_lowlevel_20261012_checks_static_flash_flash_r3.txt)。

这证明在该固定 ELF、静态回调和当前板上，改变 Flash 指令缓存开关会改变 BM2 测量，恢复开关后测量恢复；关闭后相对 1229 cycle 增加 521 cycle（+42.39%）。切换遵循 [ST RM0390](https://www.st.com/resource/zh/reference_manual/DM00135183.pdf) 的 FLASH_ACR 操作顺序，只在 ICEN 关闭时复位指令缓存，保留 LATENCY、DCEN 和 PRFTEN。保留预取还避免主动引入 [STM32F446 ES0298 第 2.2.3 节](https://www.st.com/resource/en/errata_sheet/es0298-stm32f446xcxe-device-errata-stmicroelectronics.pdf) 所列关闭预取后调试休眠的勘误条件。它不是与其他动态 BM2 镜像的有效直接百分比对照，也不能证明所有阶段变化都由缓存单一因素造成。本轮没有凭该诊断新增生产取指缓存配置或大范围对齐改造。

### 7.2 首次失败与真实 RAM 头部核验

首次动态回调 [原始失败日志](../hil/lowlevel_20261012/log_lowlevel_20261012_checks_final_flash_r1.txt) 完整保留：第一模式 BM2 为 1592，任务数和注册表正常，但 heap_free 从 28496 到 28488，严格不变断言使 FD pass=2 fail=1 / status=-1，并已恢复 ACR。不能删除该记录或将其计为通过。

随后同一动态诊断模型复跑到故意停留的失败现场，使用不会复位目标的 HOTPLUG 读取两个真实 RAM 空闲块头以及分配/释放计数。[heap_fragment_hotplug_verified.json](../hil/lowlevel_20261012/heap_fragment_hotplug_verified.json) 记录镜像 SHA `07B09BCEAED3A7423F027E3E7EE159FE3050841BAE644D7654045AFB750E7100`，`hardware_headers_match=true`：

| 真实 RAM 读取 | 值 | 含义 |
| --- | --- | --- |
| [A @ 0x200023C8](../hil/lowlevel_20261012/heap_fragment_hotplug_free_header_a.txt) | A5A5A5A5 / 800004A8 | 空闲块 A，物理块长 0x4A8 |
| [B @ 0x20002870](../hil/lowlevel_20261012/heap_fragment_hotplug_free_header_b.txt) | A5A5A5A5 / 80006AB0 | 紧邻空闲块 B，物理块长 0x6AB0 |
| [alloc_count](../hil/lowlevel_20261012/heap_fragment_hotplug_alloc_count.txt) | 0x3FE = 1022 | 成功分配次数 |
| [free_count](../hil/lowlevel_20261012/heap_fragment_hotplug_free_count.txt) | 0x3FC = 1020 | 成功释放次数 |

A 末端地址恰好为 B 起始地址。两块物理长度合计 28504 B，扣除两份 8 B header 后可用 payload 合计 28488 B；分配/释放差 2 对应仍运行的 benchmark TCB 和栈。该现场与分配器仅向后合并、留下一个额外空闲头的模型一致，8 B 差额属于空闲链分块元数据，不能据此认定新增的失联内存泄漏。也不能从这一现场推广为整个堆在任意负载下永不碎片或永不泄漏。

最初探针误用逐次 Under Reset 连接，读数之间复位了目标，不能代表已完成运行的 RAM。该无效过程在 [invalid_heap_probe_under_reset.json](../hil/lowlevel_20261012/invalid_heap_probe_under_reset.json) 单独标为 `valid_heap_snapshot=false`，不用于上述结论。有效依据仅为替代 HOTPLUG 记录，失败诊断与无效读数均保留。

## 8. 最终日常构建与恢复

日常构建已经恢复，最终源码的 61 个 RTOS 文件 SHA 与已测 isr_cached 清单逐一一致。日常 ELF 下载校验成功，随后捕获 15 秒启动输出，出现 `System init done`，未捕获失败行。证据为 [daily_validation.json](../hil/lowlevel_20261012/daily_validation.json)、[run 身份](../hil/lowlevel_20261012/run_lowlevel_20261012_final_daily.json)、[下载校验](../hil/lowlevel_20261012/flash_lowlevel_20261012_final_daily.txt) 和 [15 秒启动日志](../hil/lowlevel_20261012/log_lowlevel_20261012_final_daily.txt)。

| 最终交付项 | 实际结果 / 证据 |
| --- | --- |
| 日常 ELF SHA-256 | `6E1EEF7A252DE32239FCE7F79553471C64229EE52CC56B071165B92C24A0BA46` |
| text / data / bss | 199796 / 128 / 125952 B；[daily_size.txt](../hil/lowlevel_20261012/daily_size.txt) |
| 相对 phase1 daily | text 从 199736 增加 60 B（+0.03%）；data/bss 不变 |
| 生产源码 | 61 文件全部匹配已验证候选；[sources_final_daily.json](../hil/lowlevel_20261012/sources_final_daily.json) |
| 变体残留 | active_test_entries=0，active_test_rules=0；[daily_symbols.txt](../hil/lowlevel_20261012/daily_symbols.txt) |
| 日常完整构建 | 成功；[build_final_daily.txt](../hil/lowlevel_20261012/build_final_daily.txt) |
| 构建警告 | 两个原有 main.c 未使用项：ft_dispatch、audio_cnt；与 91 单元矩阵零警告分开计数 |

日常应用继续保留已有的 inactive USART selftest 和 TIM6 benchmark ISR hook，但没有启动审计/边界/低层测试任务或保留活跃测试构建规则。保留这些钩子的符号不等于测试变体仍在运行；同样，仅凭 15 秒启动成功也不能宣称长期应用或真实 USB 外设枚举已经验证。

最终六个生产文件的真实 SHA-256 如下，与性能、上下文测试和交付身份对应：

| 生产文件 | SHA-256 |
| --- | --- |
| RTOS/Core/Inc/rtos_config.h | `85A0AC30E4FB1ADE6672CE072ADAA6ECD684097BA78678C43CB0024456DA8DEC` |
| RTOS/Core/Inc/rtos_internal.h | `842C5A0016D01D261F9459E14D02E3AD911060A7FCEAC2268A7E6C310EF7A693` |
| RTOS/Core/Src/rtos_sched.c | `AA5B436C2400659265B2C74D44471DF5F2B56526CFEB724C62818DBCCF9DD14A` |
| RTOS/Core/Src/rtos_sem.c | `708C17FBB8DB8D3D8886B523AB97A1B998D24C9E9C2F3774384A1C519B764B89` |
| RTOS/Core/Src/rtos_task.c | `D040EE16284629FF8097713728548A8AE0D2486BE44F6A1BC4D345A67BD1FAFB` |
| RTOS/Port/GCC/ARM_CM7/port.c | `AEC70B72C105B7A02C5C1C2427059356152A86BCC6F2458B04F0BB50D8B97DBD` |

没有新增生产全局 RAM，日常 data/bss 实测不变。第 2.3 节的 32→24 B 只是信号量函数序言保存量变化，不能将其写成所有任务栈统一缩小 8 B。

## 9. 结论的适用范围

本报告支持：六个生产文件的局部优化在所列最终镜像和夹具下得到真实板上证据；完整回归未观察到 FAIL；默认及统计开启配置的 TCB 返回分支与真实 S16–S31 混合上下文测试通过；额外五种配置可独立 ARM 编译；Flash 开关对同镜像回调的影响可以复现。

它不支持全局加速、硬实时 WCET 上界、全部配置全部上板、物理 USB 外设传输验证、S1 已制造有效 PI 争用，或任意负载的无缺陷证明。前轮已确认的性能退步和本轮负向指标、动态诊断失败、无效探针记录均继续保留，以便后续复核和定向改进。


## 10. 进一步优化的候选方向

以下尚未实施和测量，不纳入本轮收益：

| 方向 | 已发现的成本 | 健壮性约束与下一次验证方法 |
| --- | --- | --- |
| 固定容量对象的分配索引 | 固定内存池仍需扫描占用项 | 在容量受限时试验空闲位图，覆盖空池、满池、末块、双重释放及超容量；先评估结构 ABI 和新增 RAM |
| 动态堆相邻空闲块合并 | 本轮真机确认仅向后合并会留下相邻空闲块 | 比较完整合并或不同分配策略对碎片、分配失败和关中断时长的影响；保持指针归属、对齐、越界与重复释放保护 |
| 多等待者信号量唤醒 | 位图定位优先级后仍需遍历链和维护共享优先级位 | 若优化扫描范围，需覆盖最高优先级位残留、FIFO、优先级变更、头中尾超时删除；不能把任意摘链都套用相同假设 |
| 热路径在 SRAM 执行 | 同 ELF 缓存实验表明取指成本值得继续研究 | 先评估 SRAM 余量及启动复制、链接重定位和异常入口依赖；比较多种链接布局，覆盖冷缓存与 IRQ/FPU 混合路径 |

当前日常 bss 已达 125952 B，增加静态索引或 SRAM 代码必须结合真实链接布局和栈/堆预算；不能只看某一个微基准。上述方向需要新的独立前后对照才能决定是否保留。
