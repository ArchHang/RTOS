# RTOS 生产模型综合压力测试报告（2026-10-13）

> 已完成：30 秒冒烟、三个 60 秒容量／种子场景及 900 秒持续运行，五轮实板均为 161 项通过、0 失败，共逐条验证 2433735 条消息。6 秒单包损坏负向对照被真实检出。日常固件已重新下载并通过 15 秒串口验证；独立终验确认全部 7 次实际板载采集的证据一致。
>
> 本轮新增测试夹具与测试入口，不修改 RTOS 内核。下述场景是工业采集与周期控制的合成负载；中断由 STM32F446 的真实 TIM6／TIM7 产生，数据值是确定性模拟值。

## 1. 为什么新增这套测试

此前完整回归是重新运行既有套件，覆盖的综合通信规模主要为 6 个任务。它不足以回答“十几个不同优先级任务、多路中断、多个等待者同时争用 queue／sem／event 时是否仍能正确运行”。本轮将这些机制放在同一个实际运行系统中，并要求每个角色进展、每条已接纳消息可核对、过载能恢复、停机能清理。

方法借鉴 FreeRTOS IntQueue 的不同优先级任务与两路嵌套中断共同访问队列、消息身份检查，采用本项目自己的 API 和返回契约。本轮没有移植该官方测试的全部受控优先级服务用例。[FreeRTOS IntQueue 官方源码](https://github.com/FreeRTOS/FreeRTOS/blob/main/FreeRTOS/Demo/Common/Minimal/IntQueue.c)

周期检查各个角色是否持续推进，参考 FreeRTOS comprehensive demo 的综合子系统运行与检查任务方法；总吞吐正常不能替代逐任务进展检查。[FreeRTOS comprehensive demo 官方源码](https://github.com/FreeRTOS/FreeRTOS/blob/main/FreeRTOS/Demo/Posix_GCC/main_full.c)

采用确定性随机种子、不同优先级、定时器上下文、有限运行与最低覆盖条件，参考 Zephyr Ztress；运行到时间本身不是成功判据。[Zephyr Ztress 官方文档](https://docs.zephyrproject.org/latest/develop/test/ztest.html#stress-test-framework)、[Ztress 官方头文件源码](https://docs.zephyrproject.org/latest/doxygen/html/ztress_8h_source.html)

详细设计依据与自研 event 语义差异见[方法说明](production_stress_method_notes_20261012.md)。

## 2. 实际任务和资源模型

16 个业务 worker 使用静态 TCB、8 字节对齐的 256 字栈。所有任务循环均含有限阻塞或周期延时，避免高优先级忙循环造成合法饥饿后又误判为内核故障。

| Worker ID | 优先级 | 生产角色 | 主要行为 |
|---|---:|---|---|
| 0 | 5 | 周期控制 | 每 10 tick 运行，共享配置锁内更新，检查完成响应时间 |
| 1–4 | 6–9 | 四路采集 | 获取共享资源信号量及配置 mutex，向同一 queue 发送模拟采样包 |
| 5–8 | 10–13 | 四路处理 | 同时竞争同一 queue，逐包检查来源、序号、内容与校验值 |
| 9–10 | 14–15 | 中断服务 | 同时获取同一个计数信号量，成功后延时以给另一任务服务机会 |
| 11 | 16 | ANY 告警 | 等待 IRQ 位 0／1 或任务位 4／5，CLEAR_ON_EXIT |
| 12 | 17 | ALL 同步 | 等待维护任务依次设置的位 6／7，CLEAR_ON_EXIT |
| 13 | 18 | 超时监测 | 等待永不设置的位 8，验证有限超时及未满足的返回快照 |
| 14 | 19 | 总线维护 | 持 mutex 延时 2 tick，形成真实竞争和优先级继承，再设置同步位 |
| 15 | 20 | 健康服务 | 周期取得配置 mutex，形成低优先级业务结果 |

controller 使用优先级 4、1024 字栈，负责编排、每秒检查所有任务心跳和角色完成量、检查等待链及验收。连续 2 秒没有心跳或角色结果会失败；超时监测角色以合法超时次数作为其业务结果。

controller 的同步 UART 日志统一通过 `stress_log()` 输出，打印期间临时调到优先级 21，打印结束检查并恢复原优先级。这样 UART 传输不会作为高优先级检查任务长期挡住优先级 5 的控制环；实际列表检查与阶段编排仍在优先级 4 执行。

加上 controller、idle、timer，系统共有 19 个任务。保持原生产配置时 perf 注册表容量为 18，不能以该表的条目数证明 19 个任务均被监测。本夹具直接持有 16 个 worker 的真实 TCB，逐一检查状态、进展、切换次数和栈水位，并检查 controller 栈。idle／timer 的全部历史栈余量不属于这套逐 worker 判据。

主要文件为[主夹具](../hil/fw/Src/production_stress.c)、[消息接口](../hil/fw/Inc/production_stress.h)、[中断夹具](../hil/fw/Src/production_irq.c)、[中断接口](../hil/fw/Inc/production_irq.h)。

## 3. 数据正确性与同步判据

### 3.1 六路消息共同访问一条真实 RTOS queue

来源 0–3 是四个采集任务，来源 4–5 是 TIM6／TIM7 ISR。16 字节消息包含四个 `uint32_t` 字段：source、sequence、payload、checksum。每个来源的 sequence 从 0 开始，只有发送 API 返回 RTOS_OK 才推进；满队列或超时属于拒收，不属于已接纳消息。

每个接收任务检查来源范围、checksum，再用 source／sequence／seed 重算预期 payload。发送和接收使用同一个纯 C payload helper，重算不能单独证明 helper 算法正确，因此 controller 还执行四组独立主机生成的 golden 向量，共 8 项 payload／checksum 检查，包含 32 位乘法溢出和最大序号。向量记录见[主机 golden 证据](../hil/production_20261012/oracle_vectors.json)。这个校验值用于检出本测试的数据破坏，不是密码学完整性或全部错误模式证明。

多个消费者的验证完成顺序可能不同于出队顺序。每个来源用 1024 位滑动身份窗口和连续已验证前缀追踪结果，不要求接收 API 返回后验证时间单调。窗口超界、重复身份、内容错误均失败。排空后每个来源必须同时满足：

```text
validated == successful_send_count
contiguous_prefix == successful_send_count
pending_window == 0
```

这比“生产数等于消费数”多检查了每个身份。最大队列容量 64、四个并行消费者远小于 1024 的正常在途窗口；若一个消息身份缺失，不能通过继续增加总计数掩盖它。

### 3.2 信号量、mutex 与 event

两路 IRQ give 同一个计数信号量，两个服务任务竞争 take。最终检查 `successful_give == successful_take_0 + successful_take_1 + remaining_count`；give overflow 单独记录，不能混入成功 give。另一个容量 2 的信号量模拟有限服务资源，成功 take 后才配对 give，最终两个资源都必须归还。

四个采集任务、控制、维护及健康角色访问同一个 mutex。维护任务在持锁状态下有限延时，让高优先级客户端实际进入等待链；controller 必须观测到锁持有者的有效优先级高于其基础优先级，不能以调用过 mutex 就宣称验证了优先级继承。收尾后 mutex 无 owner，各 worker 有效优先级恢复。

ANY 和 ALL 的消费位互不重叠。本内核 `event_set()` 按优先级逐个唤醒，并立即执行当前 waiter 的 CLEAR_ON_EXIT，因此不能照搬“同位多个清位等待者都成功”的广播语义。事件是可合并的状态位，不对 IRQ 设置次数与任务返回次数做消息式守恒。超时 worker 等待从不设置的位 8，返回必须未满足且实际等待至少 7 tick。本轮验证这些掩码下的并发 ANY／ALL／CLEAR／timeout 路径，未单独完成同位多 CLEAR 等待者的受控竞争穷举。

### 3.3 等待链和运行进展

controller 在内核临界区内检查 queue 的 send／recv、两个 sem、event、mutex 共 6 条等待链，验证节点身份、前后链接、归属、等待头地址、合法任务状态、排序与 sem 优先级位图，并反向检查 TCB 不存在宣称入链但头链无法到达的孤儿节点。有限 IPC 等待在当前内核的状态为 DELAYED，永久等待为 BLOCKED，两者均属合法等待状态。

最终还必须命中至少 4 个同时等待 queue 接收的任务、2 个同时等待 IRQ sem 的任务、3 个同时等待 event 的任务，以及真实阻塞发送、资源信号量竞争和优先级继承。链排序检查不等同于对每一次单消息唤醒做最高优先级身份的受控验证。

每阶段检查 16 个任务心跳有增长，controller 每秒检查所有角色进展，收尾逐角色要求至少一个真正业务结果。控制周期为 10 tick，预设完成 deadline 为 10 tick；`jitter_max` 是开始迟到，`response_max` 是成功释放配置 mutex 后相对本次周期释放点的完成响应时间，超过 10 tick 累计 deadline miss，不能只检查开始时间。

## 4. 两路真实硬件中断与三阶段压力

板子为 NUCLEO-F446RE，STM32F446，168 MHz，tick 1 kHz。通过 ST-LINK SWD 下载、Flash 校验及复位，从 COM3／115200 采集输出。

TIM6 使用 1 MHz 计数时钟、1000 µs 周期、NVIC 优先级 7；TIM7 使用 701 µs 周期、NVIC 优先级 6。两者均在允许调用 RTOS API 的优先级范围。ISR 的 queue send 使用 RTOS_NO_WAIT，不调用 mutex 或删除任务。

两路自然定时器中断持续并发运行。每 128 次 TIM6 中断，在临界区外受控 pending TIM7，形成硬件 NVIC 的真实嵌套抢占；这部分是受控软件触发覆盖，不是假装由外部设备产生的中断。固件检查 IPSR、NVIC 优先级及错误计数，要求实际活跃深度至少 2。内部 snapshot 分别记录自然／受控触发原因，但本轮 UART 只打印总入口、成功发送、拒收、嵌套、最大深度、嵌套中观测到的 forced 和 errors；不能从这份日志直接得到精确的自然入口数。natural 与 forced 在一次 TIM7 入口可能同时命中，不应简单相加宣称互不相交的中断数。

IRQ 总入口也不等于 IPC 尝试：handler 对没有有效触发原因的入口不会投递。容量 1 场景 TIM7 的 86159 次入口，比 60815 次成功发送加 25342 次拒收多 2 次。独立解析记录这个差值为 `inferred_entries_without_ipc`，并核对信号量 give 尝试数等于实际消息投递尝试数；未直接打印的 spurious 计数和具体触发原因不据此认定。

| 阶段 | 采集任务间隔 | queue 消费服务间隔 | 验收重点 |
|---|---|---|---|
| 正常 | 确定性随机 8–11 tick | 每轮 1 tick | 六路生产、四消费者及所有辅助角色推进 |
| 高压 | 确定性随机 1–4 tick | 每轮 4 tick | 接纳与拒收区分、队列满、任务发送超时、真实等待者竞争 |
| 恢复 | 确定性随机 12–15 tick | 每轮 1 tick | backlog 消化，必须再次观察到空 queue，业务继续推进 |

三阶段相邻运行，硬件 IRQ 贯穿三个阶段。高压通过降低处理服务能力模拟下游拥塞，没有挂起生产控制任务。编译参数 `PRODUCTION_STRESS_SEED`、`PRODUCTION_STRESS_SECONDS`、`PRODUCTION_STRESS_QUEUE_CAPACITY` 由[构建脚本](../hil/build_fw.ps1)设置。

## 5. 有界停机、资源检查与证据链

结束时先发布停止生产标志并停止／恢复 TIM6、TIM7 相关寄存器；等待任务生产者停机；保留消费者排空所有成功发送的消息；随后停止其余 worker。等待成功不仅要求 ack mask，还要求 16 个真实 TCB 均为 SUSPENDED。最后检查消息身份、信号量守恒、等待链为空、mutex 已释放、资源归还、栈余量，再删除静态 worker 并 deinit IPC。

中断夹具拒绝占用中的 TIM6／TIM7／DAC 和 NVIC 线路。本轮从复位后的空闲外设状态启动，检查的是可见寄存器、IRQ 优先级和时钟使能的恢复，不包含已有定时器的内部预分频相位或 ARPE 影子状态的无缝恢复。

排空最长等待 2000 tick，其余 worker 停机最长等待 200 tick，失败路径也会执行有界清理。每个 worker 的历史剩余栈要求至少 32 字，并校验 TCB stack_magic 与低端未使用水印。堆 free、最大连续可分配块和 alloc／free 累计次数必须和本轮开始前一致。这里验证的是该静态夹具运行没有额外堆分配或资源损失，并非动态堆全部碎片组合测试。

每个 case 都保存源码 manifest、ELF、ELF SHA256、构建日志、反汇编、size、下载校验、原始串口日志、run metadata、解析结果。参数切换前强制删除对应 RTOS／Core／HIL 的 `.o/.d`，避免 make 因编译宏变化不可见而继续用旧目标文件。缺失结束标志、任一 FAIL、coverage 不足或源／ELF 在运行中变化，都不得归类为正常通过。

证据主目录为[hil/production_20261012](../hil/production_20261012)，名称沿用启动时的 20261012，实际完成时间以 metadata 中 2026-10-13 为准。执行器是[run_production.ps1](../hil/production_20261012/run_production.ps1)。

压力构建的 FIRMWARE_APP_TASKS=0，日常 task_0／task_1 没有启动；压力固件的活动任务是上述 19 个。首轮日志在 STRESS BEGIN 前出现的旧 `task 0／1 running` 是串口打开后、SWD 下载前旧固件的输出，不能把它计为本轮业务负载。身份和计数均以本次 System init／STRESS BEGIN 后为准。

## 6. 已完成的实板结果

### 6.1 冒烟：seed 1、queue 容量 8、30 秒工作阶段

2026-10-13 09:45:31 至 09:46:05 完成下载、运行及串口收尾。三个阶段各 10000 tick，`STRESS|SUMMARY|pass=161 fail=0 worker_errors=0`，`STRESS|DONE|status=0`。

| 指标 | 实测 |
|---|---:|
| 已接纳并逐条验证的消息 | 65920 |
| 四任务源消息数 | 3308／3234／3222／3127 |
| TIM6／TIM7 源消息数 | 21293／31736 |
| 各来源最终 validated／prefix | 均等于各自 sent，pending 均为 0 |
| queue 同时等待接收／发送峰值 | 4／4 |
| IRQ sem／资源 sem 同时等待峰值 | 2／2 |
| event／mutex 同时等待峰值 | 3／4 |
| 观测到优先级继承的次数 | 1553 次采样 |
| 高压 queue 满／恢复空 queue 采样 | 8720／9796 |
| 控制开始迟到／完成响应最大值 | 0／1 tick |
| 完成 deadline miss／角色进展 watchdog miss | 0／0 |
| TIM6／TIM7 实际入口 | 30067／43126 |
| TIM6／TIM7 拒收发送 | 8774／11390 |
| 实际中断嵌套／最大深度 | 507／2 |
| 受控 TIM7 pending 已观测 | 234 |
| 最小 worker 剩余栈 | 210 字／840 字节 |
| controller 剩余栈 | 694 字／2776 字节 |
| 堆 free 与最大连续块，前／后 | 均为 28496／28496 字节 |

IRQ 信号量 accepted=60198，两个服务任务 taken=30087／30086，最后 remaining=25；三者守恒。另有 12995 次已记录的 sem give overflow，属于有限容量同步的背压。它们没有被错误计入成功令牌，也不能据此宣称每次 IRQ 都被任务处理。

该压力固件 text=45132、data=120、bss=62440 字节，处于链接脚本 128 KiB RAM 限制内。这是关闭日常应用／USB 的专用压力固件，不能把其 RAM 余量套用到日常固件。

ELF SHA256 为 `E10A59FA70F7F6B5A54D9310B569D2D976A2FD59B801971820AD4C70DEDCD060`。[原始串口日志](../hil/production_20261012/log_production_20261012_smoke_connected_1.txt)、[运行身份](../hil/production_20261012/run_production_20261012_smoke_connected_1.json)、[下载校验](../hil/production_20261012/flash_production_20261012_smoke_connected_1.txt)、[源码 manifest](../hil/production_20261012/sources_production_20261012_smoke_connected_1.json)、[解析结果](../hil/production_20261012/results_smoke_connected.json)。

### 6.2 负向对照：seed 1、queue 容量 8、6 秒

仅将任务源 0 的 sequence=3 那一条成功发送消息的 checksum 异或 1，不预置错误计数，不破坏内核数据结构。正常接收路径真实检出 `checksum_errors=1`，输出 `STRESS|INJECT|checksum_corruption|source=0|sequence=3` 和 `STRESS|FAIL|packet_integrity checksum_errors=1`。

该来源 sent=860、validated=859、prefix=3，身份窗口保留缺口；最终 SUMMARY pass=159 fail=3 worker_errors=1，DONE status=-1。三个失败计数包含消息缺口检查、总运行契约和真实 checksum 错误，不是三条被注入消息。解析器把该运行归类为 `expected_corruption_detected`，不是正常 PASS。

ELF SHA256 为 `BECE1B18F88FB329FFC293F3F19E19139E90A2C5C05448482E5B79204887273B`。[负向串口日志](../hil/production_20261012/log_production_20261012_negative_control_1.txt)、[负向解析结果](../hil/production_20261012/results_negative_control.json)。这个对照证明当前板载数据判据及主机解析能拒绝指定单包 checksum 损坏，并不证明它能检测所有可能故障。

### 6.3 容量／种子矩阵及延长运行状态

| 场景 | Seed | Queue 容量 | 三阶段工作时长 | 当前可确认结论 |
|---|---|---:|---:|---|
| matrix_final_1 | 1 | 1 | 60 秒 | PASS：161／0，逐条验证 130390 条 |
| matrix_final_2 | 0x12345678 | 8 | 60 秒 | PASS：161／0，逐条验证 131738 条 |
| matrix_final_3 | 0xA5A5A5A5 | 32 | 60 秒 | PASS：161／0，逐条验证 131756 条 |
| soak_final_1 | 0xDEADBEEF | 8 | 900 秒 | PASS：161／0，逐条验证 1973931 条 |
| 最终日常固件恢复 | — | — | 15 秒串口观察 | PASS：下载校验完成，复位后 task0／task1 持续输出 |

三个矩阵场景均有 queue 接收／发送等待峰值 4／4、IRQ sem／资源 sem 峰值 2／2、event／mutex 峰值 3／4，最大中断活跃深度均为 2；所有来源的 sent=validated=prefix、pending=0。堆 free 和最大连续块每次都从 28496 字节恢复到 28496 字节。各任务均真实推进且成功停机，没有运行错误或 watchdog miss。

| 实测指标 | 容量 1／seed 1 | 容量 8／seed 0x12345678 | 容量 32／seed 0xA5A5A5A5 |
|---|---:|---:|---:|
| 优先级继承采样 | 3130 | 3178 | 3241 |
| 高压满 queue 采样 | 19664 | 19666 | 18412 |
| 恢复空 queue 采样 | 19612 | 19615 | 19635 |
| TIM6／TIM7 拒收 | 20866／25342 | 19722／25290 | 18490／23738 |
| 实际嵌套／受控 pending 已观测 | 1038／469 | 1033／469 | 1024／469 |
| 控制启动 jitter／完成 response 最大值 | 0／1 tick | 0／1 tick | 0／1 tick |
| 控制 deadline miss | 0 | 0 | 0 |
| 最小 worker 剩余栈 | 210 字 | 208 字 | 210 字 |
| controller 剩余栈 | 694 字 | 694 字 | 694 字 |

矩阵的 ELF SHA256 分别为 `FCB1EB3DB3940AFCCFEC410C2B1F63202369E2FC18601A3626C0A902C7752FAD`、`E42F17B4BBD14AE005AAFAC4D5579E48CD95D4DEDFF74EEE9219025C7487BE14`、`3FAAAD75D29BE7321890B2BC5A759EC773CC42BCF739483CC5707CE8B53DD0C6`。[矩阵解析结果](../hil/production_20261012/results_matrix_final.json)、[容量 1 日志](../hil/production_20261012/log_production_20261012_matrix_final_1.txt)、[容量 8 日志](../hil/production_20261012/log_production_20261012_matrix_final_2.txt)、[容量 32 日志](../hil/production_20261012/log_production_20261012_matrix_final_3.txt)。

五次正常实板运行累计逐条验证 2433735 条消息。同一组 161 项验收检查在五轮中全部通过，共 805 次检查执行；负向对照单独归类，不计入正常通过次数。正常工作阶段配置总时长为 1110 秒（18 分 30 秒），另有 6 秒负向对照。

### 6.4 900 秒持续运行的实际结果

运行从 2026-10-13 09:53:24 至 10:08:27 完成下载、采集和收尾。正常、高压、恢复阶段各为 300000 tick，日志包含各阶段周期进度和最终 DONE status=0。独立只读复核确认 161 条 CHECK 均为 PASS，worker_errors=0，六来源及四消费者账本一致。

| 指标 | 实测 |
|---|---:|
| 六来源 sent／validated／prefix | 120468／110449／104455／89954／614340／934265，各来源三者相同 |
| 四消费者完成总和 | 1973931 |
| queue 接收／发送等待峰值 | 4／4 |
| IRQ sem／资源 sem／event／mutex 等待峰值 | 2／2／3／4 |
| 优先级继承采样 | 47334 |
| 高压满 queue／恢复空 queue 采样 | 285073／294300 |
| TIM6／TIM7 实际入口 | 900069／1291003 |
| TIM6／TIM7 拒收发送 | 285729／356736 |
| 真实嵌套／最大深度／嵌套中 forced 已观测 | 15477／2／7031 |
| IRQ 错误／控制 deadline miss／watchdog miss | 0／0／0 |
| 控制开始 jitter／完成 response 最大值 | 0／1 tick |
| 最小 worker／controller 剩余栈 | 208／692 字 |
| 堆 free 及最大连续块，运行前／后 | 均为 28496／28496 字节 |

IRQ 信号量守恒为 `1800201 = 900088 + 900087 + 26`，另有 390869 次 overflow。TIM7 的入口与实际 IPC 尝试也相差 2，仍按上文所述记录推导差值；所有成功接纳数据的身份窗口最终清空。停机、删除 worker、deinit 全部 IPC 及零额外堆分配检查均通过。

该 ELF SHA256 为 `4CE304DC46B64E36F8C6A080DFB502D2FF9D556A66B1B13090A69A52EF50B9E5`。[900 秒原始日志](../hil/production_20261012/log_production_20261012_soak_final_1.txt)、[运行身份](../hil/production_20261012/run_production_20261012_soak_final_1.json)、[解析结果](../hil/production_20261012/results_soak_final.json)。

上述“30／60／900 秒”是三个阶段配置的 tick 时长总和，不包含初始化、UART 检查结果输出、阶段间日志、排空清理、SWD 下载及主机工具开销。不能把 COM3 capture 的开始／结束墙钟差直接当作工作阶段执行时间。

## 7. 测试器自身修正与验证范围

| 检查时发现的问题 | 测试夹具修正 |
|---|---|
| PERF 填充整个未用栈，包括 word 0，原先要求 word 0 为 DEADBEEF 会全部伪失败 | 检查 TCB.stack_magic 的 DEADBEEF 与未用低端 A5 水印，直接扫描历史水位 |
| 有限 IPC timeout 被内核标为 DELAYED，原先只允许 BLOCKED 会误报链损坏 | 同时接受合法 DELAYED／BLOCKED，并反向检查 orphan |
| ack mask 先于 self-suspend 发布，controller 可能在两者之间抢占 | 同一临界区同时要求 ack mask 和全部真实 SUSPENDED |
| 多消费者完成顺序可能乱于出队顺序 | 使用滑动身份窗口和连续前缀，避免错误要求完成时间单调 |
| controller 高优先级同步串口日志可能自身扰动控制 deadline | 所有日志临时降至 21，检查并恢复优先级 |
| 只检查控制开始迟到会漏掉锁等待或完成过晚 | 分别记录开始 jitter 和完成 response，按完成 deadline 验收 |
| 仅在阶段末检查一次心跳不足以检出长时间停滞 | 每秒检查每个任务的心跳和角色结果，2 秒无进展失败 |
| 错误路径临界区外读取 ledger prefix，诊断可能变化 | 退出临界区前保存诊断快照 |
| 诊断器遇到坏 priority／未知 mutex owner 可能自行移位越界或解引用错误 | 检查 priority 范围及 owner 的已知 TCB 身份后再读取 |
| 发送和接收 helper 共用，可能同错而相互符合 | 增加四组独立主机 golden 的 8 项板载检查 |
| 只依赖 SUMMARY=0 无法证明检测器有效 | 增加单包 checksum 负向对照，要求具体注入与具体损坏证据 |
| Windows PowerShell 5.1 将顶层 JSON 数组保留为单个管道对象，复核脚本误报源码数量 | 读取 JSON 后显式枚举数组，恢复基线直接赋值为 object[]；通过实际 powershell.exe 执行验证 |

以上是夹具和证据工具的可信度修正，没有借此修改内核或放宽压力覆盖目标。本轮五次正常实板运行未观测到新的 RTOS 失败，负向对照按预期检出指定损坏。

两个新 C 单元分别在 O0／O2／O3 下通过 `-Wall -Wextra -Werror`，两个公开头文件单独包含也通过，正式严格编译共 8 项、0 警告。[正式编译记录](../hil/production_20261012/strict_compile/results.json)。另外代理隔离验证了 O2 负向宏分支，[隔离编译记录](../hil/production_stress_20261013/agent_compile_frozen/results.json)。O0 和 O3 属于编译检查；实板场景运行的是 O2。

完整压力固件构建还有 main 中 25 条既有未使用函数／变量警告，不能把“新夹具零警告”表述为“整个工程零警告”。首次 smoke_initial 构建成功，但 COM3 无法打开，随后枚举没有 ST-LINK，未执行下载或板载运行；这个环境连接失败既不算 RTOS PASS，也不算 RTOS FAIL。[保留的初始尝试分类](../hil/production_20261012/attempt_smoke_initial.json)

压力固件构建参数明确为 APP_TASKS=0、FAULT_TASKS=0、USB_APP=0。启动输出保留了原 main 的故障菜单文字，不表示对应故障任务已运行。冒烟日志开头的 task0／task1 两行来自监听已打开、SWD 复位尚未完成之间的旧固件输出；业务压力判据从唯一的 STRESS BEGIN 开始，不将这些残留计入并发任务。

## 8. 源码身份、日常恢复与结论边界

冻结的主夹具 SHA256 为 `A620AED13ECAB6DC8D878005FD358C5FD6D7D67D6C49883C8D036556678B23EC`，中断夹具为 `1A9B15079CB3AF73601F22C370FBFE3363A2316158C5A8BDA8FE0A267B64993E`。每次运行保存四个新夹具、main 和 build 脚本的身份。RTOS 下 61 个 `.c/.h` 与[本轮开始前 manifest](../hil/production_20261012/kernel_before.json)保持一致；本轮改动范围是测试源码和 main／HIL 构建集成。

所有场景结束后已执行[恢复脚本](../hil/production_20261012/restore_daily.ps1)：重新清理目标文件、恢复 CubeIDE 日常构建规则、核对无激活的测试入口，再真实下载、校验、复位并观察 COM3 15 秒。以最后一次 System init done 后的新启动段验收，task0／task1 分别持续输出 16／31 次，无失败行。现有未激活的 USART 自测和 TIM6 benchmark hook 保留，不代表测试任务仍在运行。

最终日常 ELF SHA256 为 `49BF44168E556C2157A30BD723323B536CE0F68EA63C82C89158A25AAF6E386E`，text=199796、data=128、bss=125952 字节；当前 Debug ELF、保存副本与下载记录的身份一致，61 个内核源码仍与本轮开始前完全相同。[日常恢复验证](../hil/production_20261012/daily_validation.json)、[恢复后串口日志](../hil/production_20261012/log_production_20261012_final_daily.txt)、[日常符号记录](../hil/production_20261012/daily_symbols.txt)。

独立[证据复核脚本](../hil/production_20261012/validate_evidence.ps1)已在 Windows PowerShell 5.1 实际执行通过。[完整终验结果](../hil/production_20261012/evidence_validation.json)逐项核对六次压力采集和一次日常恢复的 BEGIN／构建参数、COM3／115200、指定 ST-LINK 及 F446 板卡、下载校验／复位、保存 ELF SHA256、源码身份、全部 CHECK、任务与消息账本、信号量守恒、IRQ、deadline、栈和堆。初始未连接板卡的尝试单独排除；负向对照严格归类为指定单包损坏被检出。它们均未混入五次正常通过。

这套测试覆盖本报告列出的混合 IPC、实际中断嵌套、背压和恢复模型。它未验证实际传感器、DMA／总线电气输入、物理 USB 枚举或传输、掉电／复位恢复、全部 task 生命周期交错、全部同位 event 消费语义、所有动态堆碎片和全部优先级服务时序；也没有形成硬实时 WCET 证明、所有竞态不存在的证明或正式工业认证。900 秒持续运行属于有限时长压力证据。

## 9. 复现入口

[HIL README 第 9 节](../hil/README.md)提供构建、下载、采集和日常恢复命令。新批次必须使用新的 tag／输出目录，保留所有失败尝试，不覆盖本轮证据。已有批次的独立终验可直接只读运行单例，例如：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File hil\production_20261012\validate_evidence.ps1 `
    -InspectTag production_20261012_soak_final_1
```

正式整批验证会拒绝覆盖已经保存的 evidence_validation.json。后续若修改内核，必须重新建立源码基线并执行测试，不能沿用本轮 PASS。
