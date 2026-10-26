# RTOS 临界区缺陷复现、修复与回归报告

> 日期：2026-10-25。工程：`f446_rtos`。实测平台：NUCLEO-F446RE，Cortex-M4F，168 MHz，1 kHz tick。
> 接口：原 ST-LINK/SWD、USART2/COM3、115200-8N1。证据目录：[hil/basepri_20261025](../hil/basepri_20261025/)。
> 本报告核对用户提供的 `RTOS_BASEPRI_CARRY_BUG_REPORT.md`，验证 F446 当前源码；未将 H743 应用的现场推断直接当作本工程结论。

## 1. 结论与原测试遗漏

**共享临界区保存槽位的竞态已在 F446 真机复现。** 原实现经过完整 enter/exit 配对后，BASEPRI 仍可能停在 `0x50`。修复前的 600 个确定性交错中有 **36 个失败**，分布与源码推导一致；百万次真实定时器干扰还发现 **2680 次线程屏蔽泄漏**。

**另外确认了出口同步缺口。** 在没有任何释放者的空信号量上，回放原始临界区代码后，100 次有限等待全部错误返回 `RTOS_OK`，而 TCB 实际记录 `RTOS_ERR_TIMEOUT`。仅给共享元数据增加 PRIMASK 保护仍未解决这个问题，最终修复必须同时包含退出时的 **ISB**。

此前测试确有遗漏：

- 顺序检查覆盖了更强 BASEPRI、PRIMASK 保留及嵌套平衡，没有在进入/退出的共享状态发布窗口注入抢占。
- 旧业务压力的两个 IRQ 为优先级 6、7，均受 `BASEPRI=0x50` 屏蔽，无法覆盖 0–4 优先级域。
- 大量正常信号量交接不能替代“无释放者时绝不能成功”的负向不变量；指令布局与编译配置也会影响未同步出口的表现。

有限测试不能证明“所有 BUG 都不存在”。本轮补齐的是明确的竞态、屏蔽继承、挂起异常同步、API 优先级边界及业务组合负载，并保留能检出旧错误的失败证据。

## 2. 原报告中成立与需要更正的内容

| 判断 | 本工程核对结果 |
|---|---|
| 全局 `saved_mask` 的进出发布可被高优先级重入破坏 | 成立；进入和退出窗口均有确定性失败 |
| 优先级 0–4 ISR 调用 `sem_give` 是合法操作，G-3 不会触发 | 不成立；这是优先级契约违约，默认断言会在 `rtos_sem.c` 入口拦截，已单独上板验证 |
| 高优先级 ISR 读到任意非零 BASEPRI 即表示泄漏 | 不成立；其抢占正常临界区时读到 `0x50` 是预期行为，需在线程配对退出后的安全点判漏 |
| BASEPRI 停在 `0x50` 时还能自然经 PendSV 传播到其他任务 | 不成立；最低优先级 PendSV 本身被屏蔽，不能在此前提下激活 |
| 当前 PendSV C 提交函数内部又调用相同 enter/exit | 不成立；当前 `rtos_sched_commit_context_switch()` 没有这组调用 |
| ISR 自平衡后，线程发布 `nest=1` 就凭空增加了一层 | 该示例不成立；1 正是线程拥有的一层。真实进入窗口是保存槽位已写、计数尚未发布 |
| 必须把 BASEPRI 加入 TCB 才能解决 | 不需要；本工程有效优先级配置下，持有临界区的任务不能被 PendSV 切出 |

[Arm CMSIS 寄存器说明](https://arm-software.github.io/CMSIS_6/main/Core/group__Core__Register__gr.html)解释屏蔽寄存器语义；[FreeRTOS 官方 ARM_CM4F 移植](https://github.com/FreeRTOS/FreeRTOS-Kernel/blob/main/portable/GCC/ARM_CM4F/port.c)同样具有全局临界区计数和 ISR 优先级使用约束，并不是报告所称的每任务 BASEPRI 方案。

## 3. 缺陷一：共享屏蔽槽位发布不原子

### 3.1 退出窗口

原路径先把全局嵌套计数从 1 写成 0，再从全局槽位恢复 BASEPRI。两者之间，高优先级 ISR 可看到 `nest=0`、继承 `BASEPRI=0x50`，并通过自己的临界区对把 `0x50` 写入全局保存槽。被打断线程随后恢复了这个错误值。

```text
线程                              优先级 0 IRQ
nest: 1 -> 0
BASEPRI 仍为 0x50
           ─────── 被抢占 ───────>
                                  enter: 采样 0x50；nest=0
                                  saved_mask = 0x50
                                  exit: 恢复 0x50；nest=0
           <────── IRQ 返回 ───────
恢复全局 saved_mask = 0x50
线程外层退出后仍屏蔽 SysTick/PendSV
```

进入侧的真实窗口位于 `saved_mask=previous_mask` 与 `crit_nest=nesting+1` 之间，结果同样是原值被重入者覆盖。

### 3.2 确定性复现与证据判读

专用 `RTOS_HIL_CRITICAL_TEST` 构建在五个状态边界调用测试钩子。钩子通过 NVIC 请求 **真实 TIM7 异常**，ISR 使用真实 MRS/MSR、真实异常栈及原移植层原语。没有模拟寄存器或在主机上伪造 ISR。

高优先级 ISR 在本用例中只配对调用底层临界区原语，**不操作 IPC 对象**。这是对端口元数据的对抗性重入测试，不是宣布高优先级 ISR 可以调用内核 API。API 违约另用 G-3 负向测试验证。

修复前的代表行：

```text
BP|CASE|FAIL|id=121 stage=2 prio=0 in_bp=0x00 in_pm=0 depth=1 out_bp=0x50 out_pm=0 nest=0 ...
BP|MATRIX|cases=600 fail=36 ...
BP|STRESS|pairs=1000000 ... irq=11647 irq_errors=0 thread_leaks=2680 ...
```

失败集合恰为：`stage 2/4 × IRQ 0/4 × 原 BASEPRI 0/0x80/0xF0 × PRIMASK 0 × 深度 1/2/8`，共 36 例。

原始记录：[确定性及随机干扰失败日志](../hil/basepri_20261025/log_baseline_injected_o2_final_fixture.txt)。

夹具**先采样，后显式修复测试状态**，以免泄漏阻止输出余下证据。日志明确标注 `repair=explicit_after_snapshot` / `explicit_on_failure`。这不是内核自愈，也没有把测试人为恢复后的任务进展当作原实现的正常运行证明。

### 3.3 修复

在 BASEPRI 模式下，仅对共享元数据的进出事务短暂使用 PRIMASK：

```text
进入：保存 PRIMASK -> cpsid i -> 采样并增强 BASEPRI
      -> 发布原屏蔽槽位和嵌套计数 -> 恢复原 PRIMASK

主体：继续由 BASEPRI 保护；高优先级纯硬件 IRQ 仍可抢占

退出：保存 PRIMASK -> cpsid i -> 递减计数
      -> 最外层恢复原 BASEPRI -> 恢复原 PRIMASK -> ISB
```

使用保存值恢复 PRIMASK，没有无条件 `cpsie i`。PRIMASK 替代配置继续保留其原有全屏蔽主体语义。进入 `UINT32_MAX` 或对零计数退出时，先恢复本次临时屏蔽再报告错误，禁止计数回绕；关闭断言时保持原状态返回，不承诺从错误配对恢复业务。

源码：[rtos_port.h](../RTOS/Core/Inc/rtos_port.h)、[port.c](../RTOS/Port/GCC/ARM_CM7/port.c)。任务栈、TCB 与 PendSV 现场 ABI 没有因此扩展。

## 4. 缺陷二：解屏蔽后的结果读取早于 PendSV

阻塞 API 把 `wait_result` 预置为成功，登记等待并挂起 PendSV，之后要在任务真正恢复后读取最终结果。未同步的端口出口允许以下实际汇编顺序：

```asm
msr    PRIMASK, r3       ; 解屏蔽，PendSV 已 pending
ldrsb  r0, [r4, #96]     ; 可能先读取初始 RTOS_OK
                        ; 然后才进入 PendSV，保存已经装载的 r0
```

等待到期后，TCB 已被写成超时，但先前装载的返回寄存器仍为成功。它不是缓存不一致，也不是给字段增加 `volatile` 就能解决。

[Arm AN321 §4.7](https://documentation-service.arm.com/static/5efefb97dbdee951c1cd5aaf)明确说明：Cortex-M3/M4 开中断后，进入挂起异常前还可能执行两条指令；若后续操作必须等待挂起异常被识别，应使用 ISB，MSR 解屏蔽同样适用。本 RTOS 的阻塞返回路径需要这个保证。

新增 `basepri_blocking.c` 使用私有对象，**没有释放者、消息生产者、事件设置者或通知发送者**，同时检查 API 返回值、真实 TCB 结果、超时标记、等待时长、任务状态及数据哨兵。

```text
原始端口：ret=0 expected=-3 elapsed=2 wait_result=-3 timed_out=1
SEM：attempts=100 fail=100 false_ok=100

最终端口：SEM attempts=100 fail=0 false_ok=0
BLOCK|SUMMARY|pass=516 fail=0 attempts=500
```

证据：[原始端口回放](../hil/basepri_20261025/log_blocking_original_port.txt)、[仅原子保护但没有出口 ISB 的中间版](../hil/basepri_20261025/log_blocking_without_exit_isb.txt)、[最终 ISB 版](../hil/basepri_20261025/log_blocking_final_isb.txt)。

原始端口回放是受控替换临界区头文件，其余当前依赖保持一致，并用 `try/finally` 还原修复源码；不表示回放了完整 H743 应用。该问题在原始端口和中间补丁均被复现，不能将第一次元数据矩阵通过视为最终修复完成。

最终在两种屏蔽配置的共同出口增加带 `memory` clobber 的 ISB，使阻塞返回后的读取位于挂起异常识别之后。queue/mutex/event/notify/delay 等也依赖此同步契约；某一编译布局没有失败，不等于可以省略屏障。

## 5. API 优先级检查与使用边界

同步补齐或前移 27 处 G-3 检查：heap 3 处、perf 6 处、USB 18 处，包括 USB 槽位分配、分发及事件/端点回调。在这些路径修改共享状态前检查调用上下文，避免等到下层 IPC API 才发现违约。

边界仍然明确：

- 默认 IRQ 优先级 0–4 保留给不访问受保护内核状态的硬实时处理。
- 支持 ISR 的内核 API 仅允许符合阈值的非阻塞用法；优先级 5 只是优先级许可，不代表所有 API 都允许 ISR 调用。
- G-3 依赖 `RTOS_CONFIG_ASSERT_ENABLED=1`。关闭断言不会使违规调用安全，也不会自动转换成错误码。
- NMI/HardFault 不受 PRIMASK 保护，不得重入此临界区原语或操作内核对象。
- 单字段快照、纯计算及硬件处理按各自接口契约使用；本次元数据保护不是任务与高优先级 ISR 之间的数据互斥机制。

`basepri_guard` 对 14 类 API 分别测试 IRQ 0、4、5，另测计数下溢/溢出，共 44 个独立镜像。预期断言必须同时匹配 **TIM7 的 IPSR、生产源码模块和具体优先级/计数条件**，测试断言接收器随后永久停止，不返回被断言的函数。它不是“遇到任何断言都算通过”。

USB 相关测试是本地 OSAL/API/回调及优先级契约检查。**没有把它们当成 USB 连接、枚举或 CDC/MSC/HID 主机传输验证。**

## 6. 最终测试矩阵

| 验证层 | 范围与判据 | 最终结果 |
|---|---|---|
| 精确交错 | 5 边界 × IRQ 0/4/5/15 × 原 BASEPRI 0/20/50/80/F0 × PRIMASK 0/1 × 深度 1/2/8 | 12 配置、7200 组合，零失败 |
| 异步干扰 | 每配置百万 pair，TIM7 真实 37–73 μs 周期，线程外层退出及 ISR 自身状态检查 | 12 配置、1200 万 pair，零泄漏 |
| 阻塞返回 | 空 sem、空 queue recv、满 queue send、空 event、无 notify；每类每配置 100 次 | 12 配置、6000 次，零假成功/提前返回 |
| 优先级与非法嵌套 | 14 API × IRQ 0/4/5，加两个非法计数用例 | 44 镜像，全部符合预期 |
| 关闭断言的非法计数 | 零计数退出/最大计数进入 × 5 BASEPRI × 2 PRIMASK，跨两种屏蔽和三个优化等级 | 6 镜像、120 组合，状态完整保留 |
| 配置交叉 | BASEPRI/PRIMASK × ASSERT 开/关 × O0/O2/O3 | 上述两套矩阵均完整覆盖 |
| 严格编译 | 4 个修改过的内核 C 文件 × 12 配置，`-Wall -Wextra -Werror` | 48 组零警告；CM7 port 另有 1 组仅编译检查 |
| 原功能、边界、FPU 回归 | audit / selftest / boundary / lowlevel，保留完整串口及下载记录 | 见本轮汇总 JSON 与下节 |
| 实际业务压力 | 16 任务、共享 IPC、三层 IRQ、正常/过载/恢复、负向校验和注入 | 见下节实际结果 |

专项共 74 个镜像。矩阵原始索引：[matrix_results.json](../hil/basepri_20261025/matrix_results.json)、[blocking_results.json](../hil/basepri_20261025/blocking_results.json)、[guard_results.json](../hil/basepri_20261025/guard_results.json)、[nesting_results.json](../hil/basepri_20261025/nesting_results.json)、[compile_results.json](../hil/basepri_20261025/compile_results.json)。

每个精确交错均检查 IRQ 确实发生、IPSR/优先级正确、外层 mask/count 完整恢复。主机还独立检查继承屏蔽未提前放宽，以及 BASEPRI 模式下 IRQ 未进入元数据原子事务。高 IRQ 在正常临界区主体内读取非零 BASEPRI 则有单独的正向用例，避免误报。

## 7. 实际业务压力与功能回归

业务夹具保留四生产者、四消费者、共享队列/信号量/互斥锁/事件、优先级继承、超时和停机清理，并增加：

- 优先级 0 的 TIM2，37/71 μs 硬件周期，只执行有限整数工作及寄存器采样，不调用 RTOS。
- TIM6(7) → TIM7(6) → TIM2(0) 的真实三层抢占，由 NVIC active 位证实。
- 所有 worker 顶层循环及 controller 的线程安全点检查 BASEPRI、PRIMASK、nesting 为零。
- 高 IRQ 的非零 BASEPRI 采样仅统计；不据此判泄漏。

扩展夹具的首轮烟测未通过：它同时发现信号量假成功，以及 TIM2 状态寄存器恢复失败。后者的新增诊断严格定位到 `restore_mismatch=0x40`（SR），当时 NVIC pending 全为零，不能继续把它归因为 pending。最终使用**有界 UG/UIF 完成握手、恢复 CNT 等状态后再最终清 SR、外设读回/DSB、清 NVIC**，仍保留全部逐项等值校验；没有删除失败断言。

本轮功能、压力、性能和日常恢复的最终数字汇总在 [verification_summary.json](../hil/basepri_20261025/verification_summary.json)。首轮失败日志也保留：[production_smoke](../hil/basepri_20261025/log_production_smoke.txt)、[清源顺序诊断](../hil/basepri_20261025/log_production_quiesce_only_no_isb.txt)。

最终验收结果：

| 套件 | 实际结果 | 证据 |
|---|---|---|
| 原 audit | SCHEDTASK 25、IPC 15、MEMTIMER 23、USBPORT 54，合计 117 PASS / 0 FAIL | [日志](../hil/basepri_20261025/log_final_audit.txt) |
| 原 selftest | 432 PASS / 0 FAIL / 10 INFO，共 442 条计数记录；INFO 不计作已执行断言 | [日志](../hil/basepri_20261025/log_final_selftest.txt) |
| 原 boundary | 103 PASS / 0 FAIL | [日志](../hil/basepri_20261025/log_final_boundary.txt) |
| 上下文/FPU/性能探针 | LC 24 PASS / 0 FAIL，PC 22 PASS / 0 FAIL | [日志](../hil/basepri_20261025/log_final_lowlevel.txt) |
| 容量 1，seed=1，30 秒 | 165 PASS / 0 FAIL；65,550 条消息、693 次三层嵌套 | [日志](../hil/basepri_20261025/log_final_production_cap1.txt) |
| 容量 8，seed=305419896，30 秒 | 165 PASS / 0 FAIL；65,939 条消息、690 次三层嵌套 | [日志](../hil/basepri_20261025/log_final_production_cap8.txt) |
| 容量 32，seed=2779096485，30 秒 | 165 PASS / 0 FAIL；65,960 条消息、686 次三层嵌套 | [日志](../hil/basepri_20261025/log_final_production_cap32.txt) |
| 容量 8，seed=3735928559，900 秒 | 165 PASS / 0 FAIL；1,973,922 条消息、20,475 次三层嵌套 | [日志](../hil/basepri_20261025/log_final_production_soak900.txt) |
| 已知校验和损坏负向控制 | 识别出且只出现预期故障：1 个 worker 错误、两项衍生不变量失败；总 FAIL=3 | [日志](../hil/basepri_20261025/log_final_production_negative.txt) |

900 秒批次的六个消息来源均满足 `sent=validated=prefix` 且 `pending=0`。信号量满足：

```text
accepted = 1,800,201
taken0 + taken1 + pending = 900,087 + 900,086 + 28 = 1,800,201
```

TIM2 共进入 16,685,057 次，强制请求/观测均为 17,261；线程安全点检查 6,661,896 次、零泄漏。
高优先级 ISR 的非零 BASEPRI 样本为 **1,290,404 次**，同时线程退出检查仍全部为零，直接说明不能用“ISR 样本非零”判污染。900 秒内控制截止期与角色进展看门狗均无超限，最大片段响应为 1 tick；这些是合成业务和设定负载的观测，不是所有硬实时工况的保证。

### 7.1 性能代价

以下来自同一 F446 的无注入 `bench` 变体，均为 `-O2`，不使用含钩子的 `BP|TIMING` 数据：

| 指标（CPU 周期） | 修复前 | 最终实现 |
|---|---:|---:|
| 临界区进出对 | 24 | 40 |
| yield 切换 | 229 | 238 |
| 信号量往返 | 1584 | 1666 |
| 信号量唤醒平均 | 445 | 496 |
| 队列发送唤醒平均 | 532 | 575 |
| ISR 唤醒平均 | 517 | 566 |

临界区对增加 16 周期，168 MHz 下约 **95.2 ns**；最终一对约 238.1 ns。屏蔽状态保留、原子发布和阻塞返回顺序优先于旧性能数字。该表是当前夹具的平均/汇总测量，受代码布局和配置影响，不能当作 WCET 或严格隔离的单指令成本。

证据：[修复前无注入基准](../hil/basepri_20261025/log_baseline_nohook_bench.txt)、[最终无注入基准](../hil/basepri_20261025/log_final_nohook_bench.txt)。修改过的内核 C 文件及新增夹具使用严格警告策略验证；整个演示固件仍有原有条件编译导致的未使用函数/变量警告，不把它们描述成“全工程零警告”。

### 7.2 日常固件恢复

全部测试后已重建并下载日常镜像，COM3 观察到 `task 1 running` 和 `task 0 running`。`main.c` 与任务开始时逐字节一致，恢复构建未撤销 RTOS 修复；日常 ELF 无本轮注入钩子、测试 controller 或断言包装符号，编译规则无测试宏及配置覆盖。

[恢复状态](../hil/basepri_20261025/restore_status.json)、[日常运行日志](../hil/basepri_20261025/log_final_daily.txt)、[日常 ELF 的 sem_take 反汇编](../hil/basepri_20261025/final_daily_sem_take.disassembly.txt)可分别核对源码保留、实际运行及 `MSR PRIMASK -> ISB -> LDRSB wait_result` 的最终指令顺序。

## 8. 复现与维护入口

```powershell
# 全部端口/阻塞/优先级配置矩阵；默认在新的 rerun_时间戳 目录保存证据
powershell -NoProfile -ExecutionPolicy Bypass -File hil\basepri_20261025\run_matrix.ps1

# 原有完整回归与无注入基准
powershell -NoProfile -ExecutionPolicy Bypass -File hil\basepri_20261025\run_regressions.ps1 -Suite functional

# 三个业务容量/种子组合和校验和损坏负向控制
powershell -NoProfile -ExecutionPolicy Bypass -File hil\basepri_20261025\run_regressions.ps1 -Suite production

# 连续 900 秒业务压力
powershell -NoProfile -ExecutionPolicy Bypass -File hil\basepri_20261025\run_regressions.ps1 -Suite soak

# 恢复 CubeIDE 日常构建；测试结束后还应下载日常镜像并核对串口
powershell -NoProfile -ExecutionPolicy Bypass -File hil\restore_project.ps1 -Rebuild
```

复现依赖真实板、COM3 和仓库脚本中的 CubeIDE 工具路径。不要并行运行两个连接同一串口或调试器的测试。脚本保存 ELF 的本地身份、下载验证、串口终止行与源码清单；ELF 仍按仓库规则留在本机，不作为二进制发布。

本轮保留原 API、任务现场与日常 `main.c`。`RTOS_HIL_CRITICAL_TEST`、断言链接包装及测试任务只出现在相应变体，最终日常恢复需核对没有残留测试钩子。源码和图文机制同步见 [rtos_design.md](rtos_design.md) 第四章。

## 9. 结论的适用范围

本轮是 F446 真机验证，CM7 只做了移植汇编编译检查，不能代替 H743 的缓存、存储、DMA、总线和应用级验证；没有进行 USB 主机物理传输测试。测试没有覆盖所有中断相位、任意非法内存写、全部对象组合或任意长运行时间。

本次交付的依据是可重复的失败对照、端口时序修复、正负边界测试、配置交叉和业务不变量验收。后续每次优化都必须保留这些用例，特别不能再仅凭平均周期减少就删除出口 ISB，或用大量正常交接替代“无生产者绝不成功”的测试。
