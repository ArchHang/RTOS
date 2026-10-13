# RTOS 源码审计、修复与实板验证报告

日期：2026-10-12。工程：`E:\STM32_Project\CubeIDE\f446_rtos`。

本报告的性能指标和固件哈希属于优化前的审计历史阶段；后续优化结果与交付固件以 [性能优化报告](rtos_optimization_20261012.md) 为准。

## 1. 完成结果

本轮已确认的问题已完成修复，并在连接的 NUCLEO-F446RE 上完成专项回归、原有功能自测、边界测试、微基准和系统场景测试。最终已恢复普通 CubeIDE 构建规则，重新编译、校验烧录日常应用固件；15 秒串口观察中 task 0/1 持续运行，未出现故障输出。

| 验证项目 | 最终实测结果 | 判读 |
|---|---|---|
| 新增专项回归 | **117 PASS / 0 FAIL，连续三次** | 25 个调度/任务断言、15 个 IPC 断言、23 个内存/定时器断言、54 个 USB/端口断言；三次为同一个 ELF |
| 原有 selftest | **432 PASS / 0 FAIL / 10 INFO，总计 442 项** | INFO 单独记录，不能算作 PASS |
| 原有边界套件 | **103 PASS / 0 FAIL** | 参数边界、对象状态、资源与错误后系统稳定性 |
| 微基准 BM0–BM7 | 全部完成，到达 `BM\|DONE` | 本次版本的测量值见第 5 节 |
| 保留 USB 应用任务的 S1–S5 | 全部到达 `SC\|DONE`，`status=0` | S1 的争用覆盖局限见第 7 节 |
| PRIMASK 配置 | 13 个 ARM 编译单元成功，0 编译警告 | 隔离快照的编译及反汇编检查；未在该配置下跑板 |
| 日常固件恢复 | 编译、SWD 下载校验、启动和短时运行通过 | ELF 无 audit 任务、审计钩子或模拟 USB 传输包装符号 |

专项回归 ELF 的 SHA-256：

```text
FA0CFE9D2978F21FD4B8F9EBEF06866E351654134DA61512F06B1059D7DE75F0
```

最终日常 ELF 的 SHA-256：

```text
84A409C774D629D2D2B992A8908C38A9393236CF4EB78D360778D7FE60BC2A42
```

这些结果证明列出的缺陷及测试场景已得到验证。有限测试不能证明所有可能的程序状态均不存在缺陷；本报告不会将测试通过解释为这种保证。

## 2. 阅读范围与运行逻辑

完整阅读范围为 `RTOS/` 下 **61 个 C 源文件及头文件**，包括内核、移植层、USB 适配和随工程集成的 CherryUSB。随后按 [rtos_design.md](rtos_design.md) 核对设计意图，并阅读 HIL 说明、构建脚本和相关夹具。源码审阅按模块并行交叉检查，最终编译和硬件访问由主流程统一执行。

核对后的主要运行链路如下：

1. **就绪与切换**：32 个优先级，数值越小优先级越高；就绪位图定位最高优先级，同级就绪环维护轮转顺序。调度器选择候选 TCB，PendSV 保存当前上下文、提交候选并恢复下一任务的 PSP。浮点扩展上下文按 EXC_RETURN 决定保存与恢复。
2. **阻塞与超时**：IPC 等待节点属于 TCB，并同时关联对象等待链和任务延时链。对象唤醒或 tick 超时必须只完成一次解绑。事件超时结果需要在对象仍有效时保存到 TCB，不能等任务重新运行后再读取对象。
3. **调度挂起**：本内核挂起调度期间停止推进调度 tick；挂起期间可以执行满足条件的非阻塞操作，不能把当前任务放入等待态。嵌套 suspend/resume 需要计数，只有最外层 resume 才恢复调度。
4. **软件定时器**：TIMER 服务任务消费命令队列并串行运行回调。任务/ISR 通常提交异步命令，TIMER 自身的任务上下文可直接执行。回调允许改变定时器状态，所以不能复用活跃链指针组装一个随后会被回调修改的到期链。
5. **USB**：CherryUSB 通过自定义 OSAL 使用 RTOS 线程、同步对象和定时器；CDC/HID 应用接口还要处理传输预算、完成通知和 RESET 会话变化。中断处理、类请求解析与 OSAL 资源释放均属于本次审计范围。

开始工作时仓库已经存在用户修改和未跟踪文件。本轮保留了这些内容，采用任务开始时的快照比较，**没有把 Git HEAD 当作用户当前版本**。相对该快照，本轮改动了 **28 个 RTOS 源文件/头文件**；具体路径及前后 SHA-256 见 [source_manifest.json](../hil/audit_20261012/source_manifest.json)。`rtos_heap.c/.h` 在 Git 中已有修改，但本轮未修改其内容。

## 3. 发现的问题与修改方案

表中“原板测”指修复前或阶段性回退相关函数后在真实板子上复现；“加固”指源码检查发现的风险，以及受控输入/状态注入验证，不能据此声称正常 API 序列必然触发该状态。断言数量不等于独立 bug 数量。

### 3.1 调度器与任务生命周期

主要修改：[rtos_sched.c](../RTOS/Core/Src/rtos_sched.c)、[rtos_task.c](../RTOS/Core/Src/rtos_task.c) 及相关 TCB/内核头文件。

| 问题 | 影响与修复方案 | 验证依据 |
|---|---|---|
| 启动前 suspend/resume 改坏调度状态，挂起后 yield 仍可能切换，嵌套挂起过早恢复 | 启动前保持 NOT_STARTED；维护 `suspend_depth`；挂起态禁止切换，只有最外层恢复触发重调度；保留既有阻塞守卫并要求 current TCB 有效 | 原板测 `suspend_before_start`、`resume_before_start`、`yield_while_suspended`、`nested_suspend` 失败；最终通过 |
| PendSV 候选过时，取消切换后当前任务仍为 READY | 任务删除/挂起/优先级变化后重新评估候选；当前任务重新成为最佳候选时撤销过时切换，并恢复 RUNNING，保持后续轮转有效 | 原板测 `cancel_obsolete_switch` 和追加的 `canceled_switch_restores_running` 失败；最终通过 |
| 动态任务先抢占，调用者的返回句柄尚未发布 | 在创建的最终临界区内先写出句柄，再发布 READY 并允许抢占 | 原板测新任务已运行但看不到返回句柄；`handle_before_preemption` 最终通过 |
| 静态 TCB 构造期间可被公开 API 操作 | 活跃身份链先预留 TCB，状态设为 INITIALIZING；长栈初始化在临界区外执行；构造完成前拒绝 resume、改优先级、notify、suspend，拒绝重复占用同一 TCB | 受控构造窗口中四种操作原板测均被错误接受；最终全部拒绝 |
| 无效/已删除句柄、重复初始化及 idle 操作缺少一致保护 | delete/suspend/resume/set_priority/notify 先按地址查活跃身份链再读取 TCB；重复内核初始化不清空现有结构；禁止删除/挂起 idle 或改变其优先级；补齐 ISR、静态栈对齐与至少 96 words 的创建约束 | idle 挂起原板测失败；专项验证静态删除后重复删除、重新创建、重复 init 保留 idle 等；参数边界由 boundary 回归 |

身份链与性能跟踪链分离，关闭性能统计后生命周期保护仍然有效。它验证的是当前登记的 TCB 地址；删除后不得继续使用旧句柄，地址被新任务复用时，仅有指针的 API 无法区分旧句柄与新句柄。

### 3.2 IPC

主要修改：[rtos_event.c](../RTOS/Core/Src/rtos_event.c)、[rtos_sem.c](../RTOS/Core/Src/rtos_sem.c)、[rtos_mutex.c](../RTOS/Core/Src/rtos_mutex.c)、[rtos_queue.c](../RTOS/Core/Src/rtos_queue.c)。

| 问题 | 影响与修复方案 | 验证依据 |
|---|---|---|
| 事件超时后重新读取已经复用的对象存储 | 在超时解绑前调用等待节点的 `on_timeout` 保存事件位，恢复执行时只读 TCB 中的结果 | 原板测期望 `2`，实际读到复用存储值 `6636321`；最终为 `2` |
| 有等待者/持有者时重新 init 会清空链表，mutex 重复登记 | sem/queue/event 通过活跃 TCB 等待归属识别使用中的对象；mutex 通过已有登记判断；使用中返回 ERR_ALREADY，闲置 mutex 重复 init 保持成功且只登记一次 | 原板测活跃 init 返回 OK、mutex 登记数为 2；最终拒绝活跃 init、登记数为 1；原 selftest 的 `di_init` 仍返回 OK |
| 有效优先级没有变化也重新插入等待链 | 有效优先级相同时直接返回，避免同级等待者被移到队尾，保持 FIFO | sem/queue/event/mutex 四种原板测均唤醒第二个等待者；最终唤醒第一个 |
| 队列大小计算及直达拷贝缺少防御检查 | 校验 `capacity * item_size` 溢出；等待节点的地址/长度不合法时唤醒并返回 ERR_PARAM，再继续寻找合法节点 | 新增容量溢出、发送/接收等待节点长度不匹配三个加固断言全部通过 |

修改还在相关 ISR 修改入口保持中断优先级契约检查。没有把事件 CLEAR 的逐个等待者消费语义改成广播快照语义。

### 3.3 内存池、定时器与统计

主要修改：[rtos_mem.c](../RTOS/Core/Src/rtos_mem.c)、[rtos_timer.c](../RTOS/Core/Src/rtos_timer.c)、[rtos_perf.c](../RTOS/Core/Src/rtos_perf.c)。

| 问题 | 影响与修复方案 | 验证依据 |
|---|---|---|
| 内存池对齐/容量溢出，接受块内地址和重复 free | 检查基址对齐、向上对齐与乘法/地址范围溢出；free 必须指向完整块起点；扫描空闲链拒绝重复释放 | 原板测重复 free 成功、4 块池计数变为 5，且能分配出重复地址；最终 ERR_ALREADY、计数 4、地址互异 |
| 到期链复用活跃 `next`，回调重启会破坏遍历 | 每轮只移出一个到期定时器并执行回调；回调后重新判断状态及到期链，不维护可被回调改写的临时 next 链 | 原板测同批另一回调丢失，未来定时器在共同启动后 40 ticks 提前执行；最终另一回调执行，未来回调在共同启动后 100 ticks 执行 |
| 同地址定时器重建后，旧异步 DELETE 删除新实例 | 每次 create 分配 generation；队列命令携带 generation；服务任务丢弃旧代命令 | 原板测新实例回调计数 0；最终计数 1 |
| 周期定时器基于回调完成时刻续期，持续漂移 | 从上一到期时刻加 period，已错过的周期跳过，维持绝对相位 | 原板测相位检查值 135，期望约 100；最终 100；长回调后的下一截止时间另有回归 |
| TIMER 被 ISR 打断时，ISR 被误认成 TIMER 任务直接执行命令 | 只有非 ISR 且 current 为 TIMER 的上下文走直接命令路径；ISR 始终入队；每次回调后处理命令并重算等待时间 | 真实优先级 6 TIM6 IRQ 验证中，ISR 只入队，回调随后由服务任务执行 |
| 重复 timer_init 返回失败前已清空活跃链 | 成功标记在任何重置前检查；初始化失败可重试；拒绝 ISR 初始化 | 原板测重复 init 返回 ERR_ALREADY，但已有回调丢失；最终拒绝且已有回调仍执行 |
| 非法 timer mode 被接受 | create 显式检查模式 | 原板测非法模式返回 OK；最终 ERR_PARAM |
| 关闭热路径统计后 CPU 利用率显示 100% | 统计关闭时 CPU 利用率返回约定的 0；idle_usage_x100 仍为其补值 10000 | 原板测 CPU 值 10000，最终 0；这些约定值不表示测得的实际负载 |

内存池 alloc 保持 O(1)，free 因重复释放检测为 O(n)。API 没有缓冲区长度参数，调用者仍需提供 `块数 × 对齐后块大小` 的有效存储。定时器命令仍是异步的，generation 不会延长外部对象存储的生命期。

### 3.4 ARM 端口、USB 与 CherryUSB

主要修改：[port.c](../RTOS/Port/GCC/ARM_CM7/port.c)、[rtos_usb.c](../RTOS/Core/Src/rtos_usb.c)、[rtos_usb_osal.c](../RTOS/Core/Src/rtos_usb_osal.c)，以及 CherryUSB 的 MSC、设备核心和 DWC2 驱动。

| 问题 | 影响与修复方案 | 验证依据 |
|---|---|---|
| 临界区削弱已有 BASEPRI，退出时清除调用者的屏蔽状态 | 使用 BASEPRI_MAX；最外层保存原屏蔽值并恢复，PRIMASK 分支同样保存/恢复；统一 get/set mask 对配置的含义 | 原板测 BASEPRI `32 → 80 → 0`；最终较强值不被削弱，较弱值退出后恢复，嵌套平衡 |
| 合法 RTOS IRQ 在 PendSV 提交任务窗口更新候选 | 在 C 候选消费/提交到 PSP 恢复的关键窗口屏蔽可调用 RTOS 的 IRQ；默认 BASEPRI 配置下优先级 0–4 仍可抢占；MSP 保存 mask 时保持 8 字节对齐；浮点上下文保存顺序保留 | 真实优先级 6 TIM6 IRQ 原来在窗口内执行，低优先级任务先运行；最终 IRQ 延后，高优先级任务先运行 |
| 毫秒换 tick 的 32 位乘法溢出 | 使用 64 位向上取整，保留 FOREVER，有限超时限制在有符号 tick 比较范围 | 原板测 `4294968 ms → 1 tick`；最终 `4294968 tick`（tick=1kHz） |
| MSC 线程栈 bytes/words 单位错误 | 配置向 OSAL 提供 `512 words × 4 = 2048 bytes` | 原板测实际只有 128 words；最终实际 512 words |
| CDC/HID 超时预算、RESET 与完成状态不一致 | 总预算覆盖 mutex 等待、已有 TX、分包及重复唤醒；NO_WAIT 不进入等待；会话代次与完成序号区分 RESET 和成功发送，HID RESET 丢弃旧 RX | HID 原板测 RESET 后错误返回发送/接收 64 字节；最终不报告完成且无旧 RX；另有 6 个预算断言 |
| MSC 接受不支持 LUN、扇区加法越界 | 拒绝非法 LUN；以减法判断区间，64 位检查总字节数 | 原板测 LUN=1、溢出扇区范围被接受；最终拒绝，合法 LUN 保持通过 |
| DWC2 RX 拷贝长度超过剩余请求长度 | 先检查剩余空间；读出整个当前包，丢弃超出容量部分并 stall，不越界写入或报告正常完成；不清空整个 FIFO | 在板上运行生产 IRQ 处理函数，寄存器/FIFO 用 RAM 影子注入；旧代码破坏尾部 guard，最终有效包/超长包均保持 guard |
| GET/SET_INTERFACE 未检查完整 wIndex | 在索引数组前验证 16 位接口号及接口登记 | 原板测接口 16、`0x100` 被错误接受；最终拒绝 |
| SET_INTERFACE 处理 alt=0 和非法 alt 时错误关端点 | 非法 alt 在切换前拒绝，保持原端点；合法 alt（包括 0）关闭旧端点并打开目标端点，成功后更新接口状态 | 阶段原板测 alt=0 缺少应有切换、非法 alt 改变关闭计数；最终均通过 |
| SET_CONFIGURATION 查找越过 wTotalLength | 先检查配置值；扫描受总长度与每个描述符长度约束 | 将相邻数据构造成另一个配置，阶段原板测错误接受；最终不越界扫描 |
| OSAL 线程自然返回/自删后槽位泄漏，失败删除提前释放槽 | 仅在静态 TCB 已 DELETED 且不再是 current 时回收；构造期保留槽；ISR 删除拒绝，其他删除只有内核接受后释放预约 | 原板测重复创建到第 5 次失败，只完成 4 次；最终可继续复用；真实 ISR 删除保留槽和任务 |
| timer DELETE 队列满时 inner/outer 仍被释放 | 新增返回状态的 `rtos_usb_timer_delete_checked`；保留原 void 接口；OSAL 只有提交删除成功后释放 outer，失败保留两层资源 | 在真实 IRQ 中填队列直到 ERR_TIMEOUT，回退原删除函数后复现两个错误释放；最终两层均保留 |
| active timer 的 DELETE 尚在队列中，create 错报整个池耗尽 | 遇到仍由内核占用、暂时无法重建的槽继续扫描其他未用槽，保留内核对活跃对象的保护 | 有其他 7 个空槽时原板测 create 返回 NULL；最终成功从其他槽创建 |

另修正 EXC_RETURN 注释：`0xFFFFFFFD` 的 bit4=1 为基本帧；bit4=0 才是扩展浮点帧。增加 NMI/HardFault 等特殊异常的优先级查询边界，避免负的 SHP 索引。这些注释修正/边界加固不计作已复现的运行故障。

## 4. 修复前后证据与测试夹具校正

原来的 selftest 在任务开始版本上给出 **431 PASS / 0 FAIL / 11 INFO**，但新增第一批 64 个断言在板上得到 **23 PASS / 41 FAIL**。这说明原套件的覆盖不足，也说明 41 是失败断言数量，不能写成“41 个独立 bug”。

| 阶段证据 | 用途 |
|---|---|
| [baseline_selftest](../hil/audit_20261012/log_audit_20261012_baseline_selftest.txt) | 任务开始版本的原自测结果 |
| [before](../hil/audit_20261012/log_audit_20261012_before.txt) | 第一批缺陷的真实修复前失败 |
| [new_before](../hil/audit_20261012/log_audit_20261012_new_before.txt) | 构造窗口、取消切换状态、USB 接口越界、RX guard、线程池泄漏的追加复现 |
| [lifecycle_before2](../hil/audit_20261012/log_audit_20261012_lifecycle_before2.txt) | timer 重复 init 与 USB alt/config 的阶段复现；其中早期 queue-full 用例不能作为队列已满的证据 |
| [usb_timer_full_before](../hil/audit_20261012/log_audit_20261012_usb_timer_full_before.txt) | 在最终正确夹具下临时回退原两层删除函数，复现真正队列满时的释放错误 |
| [active_delete_before](../hil/audit_20261012/log_audit_20261012_active_delete_before.txt) | 删除尚待消费时错误池耗尽的复现 |
| [最终 audit 第一次](../hil/audit_20261012/log_audit_20261012_verified_audit1.txt)、[第二次](../hil/audit_20261012/log_audit_20261012_verified_audit2.txt)、[第三次](../hil/audit_20261012/log_audit_20261012_verified_audit3.txt) | 同一最终 ELF 的三次完整通过 |

测试过程中也发现了夹具自身的问题，已区分处理：

- 最初用固定次数的 stop 假设定时器命令队列已满，但首条命令可以直达等待中的服务任务。最终改为在真实 IRQ 中持续提交到实际返回 ERR_TIMEOUT，记录 `successes=17,status=-3`，然后检查删除失败是否保留资源。此前未确认队列满的失败不作为生产缺陷证据。
- `lifecycle_before` 曾因 RAM 寄存器影子的 FIFO 范围不足触发断言，未到达 DONE；该日志保留，但不作为 RTOS 故障证据。补全影子后重新执行。
- 首轮 mutex 修复过度拒绝闲置对象的重复 init，原自测 `di_init` 暴露兼容性回归。最终调整生产代码为闲置 init 成功且不重复登记，保留原自测的 OK 期望。
- 内存池重复 free 的旧用例以 OK/INFO 记录错误行为。它现在明确要求 ERR_ALREADY、计数不超过容量，因此从 INFO 转为 PASS；这是 selftest 从 431/11 变为 432/10 的原因。没有批量修改期望值以消除失败。
- 原 selftest 仍保留预启动阻塞等 INFO 跳过项，其中部分解释文案属于旧版本；新增 audit 单独覆盖启动前 notify_wait 等已修正的路径。INFO 不计入通过数。

## 5. 边界与性能结果

专项覆盖包括：启动前状态、嵌套 suspend、创建抢占及构造中访问、同级等待顺序、对象超时后复用、活跃对象重初始化、内存池内部地址/重复释放/算术溢出、定时器回调重入及对象代次、真实 IRQ 命令提交、保持原 interrupt mask、PendSV 合法 IRQ 竞争、超长 USB 包、非法接口/alt/config、RESET、完整超时预算、资源池反复复用和真实命令队列满。

原边界套件的最终 [103 项日志](../hil/audit_20261012/log_audit_20261012_verified_boundary.txt) 均通过，错误操作后的 current TCB、tick 推进及 yield 稳定性检查也通过。原 [功能自测日志](../hil/audit_20261012/log_audit_20261012_verified_selftest.txt) 包含基本优先级继承/恢复、互斥锁、队列、事件、定时器、任务通知、时间片及 ISR 交互回归。

本次 BM 采用 Cortex-M4F、168MHz、`-O2`，单位为 CPU cycles：

| 项目 | 样本 N | 平均 | 最小 | 最大 |
|---|---:|---:|---:|---:|
| critical enter/exit | 1 | 30 | 30 | 30 |
| yield switch | 1 | 241 | 241 | 241 |
| sem roundtrip | 1 | 1393 | 1393 | 1393 |
| sem wake latency | 1000 | 488 | 487 | 766 |
| queue send/wake | 5000 | 571 | 571 | 810 |
| task create/delete | 1 | 6175 | 6175 | 6175 |
| delay(1) period | 1 | 167999 | 167999 | 167999 |
| ISR wake latency | 100 | 546 | 542 | 606 |

来源：[本次 BM 原始日志](../hil/audit_20261012/log_audit_20261012_verified_bench.txt)。表中 N=1 表示夹具输出一条批量平均结果，并非只执行一次：BM0 平均 10000 对 critical enter/exit，BM1 平均 20000 次 yield，BM2 平均 5000 个 sem roundtrip，BM5 平均 500 对 task create/delete，BM6 平均 500 次 delay(1)。这些项目的 min/max 也复写该批量均值，并非逐次调用的极值。这些数字不构成最坏执行时间界限，也没有与同环境的修复前版本进行完整性能对照，不能据此宣称性能改善。

系统场景保留 USB 应用任务：S2 完成 0/1/4/8 等待者测试，其 `bad=0` 的阈值设为 0，未启用超限计数，不能据此判断满足延迟上限；S3 生产/消费均为 9000 items，平均队列延迟 16223 cycles，吞吐 7895 items/s；S4 在 20 秒内采集 10000 个 2ms 周期样本，平均实际周期 335983 cycles；S5 成功创建 8 个 256 words 的任务。所有场景 `status=0`。原始 [S1](../hil/audit_20261012/log_audit_20261012_verified_S1.txt)、[S2](../hil/audit_20261012/log_audit_20261012_verified_S2.txt)、[S3](../hil/audit_20261012/log_audit_20261012_verified_S3.txt)、[S4](../hil/audit_20261012/log_audit_20261012_verified_S4.txt)、[S5](../hil/audit_20261012/log_audit_20261012_verified_S5.txt) 已保存。

构建资源占用：

| 固件 | text | data | bss |
|---|---:|---:|---:|
| audit 专项固件 | 70576 | 120 | 76032 |
| 最终日常固件 | 200112 | 128 | 125952 |

单位为字节，是 GNU `size` 分类值，不能把 ELF 文件大小当作 Flash 占用。audit 构建的 25 个 warning 为关闭演示应用后的 main.c 未使用符号；日常恢复重建有 2 个 main.c 未使用符号 warning。修改的内核、端口及 CherryUSB 单元没有新增编译警告。

## 6. 硬件真实性与工程交付

实际工具与连接信息：

| 项目 | 实际值 |
|---|---|
| 板卡/MCU | NUCLEO-F446RE / STM32F446xx，Cortex-M4 |
| ST-LINK 序列号 | `066DFF555775514867092907` |
| 供电读数 / SWD | 3.26V / 4000kHz，Under Reset |
| 编译工具 | CubeIDE 1.13.2 安装中的 GNU Arm 11.3 系列，hard-float，Cortex-M4 |
| 烧录工具 | STM32CubeProgrammer CLI 2.15.0 |
| 串口 | COM3，115200，8N1 |
| 系统时钟 / tick | 168MHz / 1kHz |

测试不是仅在 PC 上模拟 RTOS。固件实际编译为 ARM ELF，经 SWD 下载并执行校验，MCU 复位后由真实串口采集；TIM6 竞争/入队场景使用真实优先级 6 IRQ。USB 专项中的异常包、控制请求、RESET 和传输完成则由夹具注入生产代码，部分底层寄存器使用 RAM 影子。

HIL 工具同时进行了证据与退出状态修正：

- 构建脚本保存完整编译日志；运行脚本保存 ELF 副本、SHA-256、烧录校验、串口原文和时间/连接 JSON。
- 缺少结束行或出现失败断言会返回错误，防止旧 ELF、旧日志或未完成测试被记为成功。修复前预期失败需要显式 `-AllowFailures`，仍必须到达结束行。
- audit 宏、PendSV 窗口钩子和 USB link wrapper 仅属于测试变体。恢复脚本还原普通编译/链接规则及 canonical objects.list，清理经过路径检查的生成目录，保留测试源码。
- 修改了 main.c 的条件测试入口，新增四组 audit 夹具；更新了设计文档中涉及本次修复的调度、临界区、任务、定时器、内存池和历史性能说明。

可核查的交付证据：

- [最终汇总 verified_results.json](../hil/audit_20261012/verified_results.json)、[源码清单 source_manifest.json](../hil/audit_20261012/source_manifest.json)。
- [audit 烧录校验](../hil/audit_20261012/flash_audit_20261012_verified_audit1.txt)、[对应连接/哈希 JSON](../hil/audit_20261012/run_audit_20261012_verified_audit1.json)。其余每次运行有同名配套文件。
- [PRIMASK 编译日志](../hil/audit_20261012/primask_compile/compile.log)、[配置快照编译清单](../hil/audit_20261012/primask_compile/manifest.json) 及该目录下 port/sched 反汇编。
- [日常固件编译日志](../hil/audit_20261012/build_final_daily.txt)、[日常下载校验](../hil/audit_20261012/flash_audit_20261012_daily.txt)、[日常串口日志](../hil/audit_20261012/log_audit_20261012_daily.txt)、[符号表](../hil/audit_20261012/daily_symbols.txt)。

基线快照及本地 ELF 保留在 `hil/audit_20261012/`，该目录的 .gitignore 排除大型固件和回退快照，不会将它们混入源码提交。本轮没有创建提交，也没有撤销任务开始前的用户修改。

## 7. 覆盖局限与保留的设计约束

- **尚未完成物理 USB 主机链路测试**：没有主机端 CDC 回环、HID 收发校验或 MSC 大文件读写校验结果。scenario_usb 中 USB 栈/应用任务真实启用，但不代表 USB 主机业务已经传输成功。
- **S1 未形成 PI 争用**：20 个样本均为 167 cycles，不能用它证明有中优先级干扰时的继承效果。基本 PI 与恢复的实板依据来自 selftest 的 `pi_inherited`（4）、`pi_high_still_wait`、`pi_priority_restored`（20）、`pi_high_got_lock`；传递式 PI 仍是现有设计限制。
- **PRIMASK 分支只做编译验证**：当前最终实板固件仍采用 BASEPRI。BASEPRI 固件中的“保留 PRIMASK 已关闭状态”用例不能替代 `RTOS_CONFIG_USE_BASEPRI=0` 固件的实板验证。
- **长时间压力未覆盖**：最长场景中的 S4 为 20 秒；三次 audit 是重复稳定性检查，不是小时级压力试验。微基准最大观测值不是 WCET 保证。
- **异步生命周期契约仍适用**：底层 `rtos_timer_t` 存储必须保持有效直到命令消费、回调完成；不得在异步删除刚提交后就释放其存储。OSAL 的 outer 包装对象在删除提交成功后可以释放，静态 inner 存储仍保留。任务删除后也不能继续使用旧地址句柄。
- **结构体 ABI 有变化**：TCB/等待节点/内核/定时器新增成员，应用必须与当前头文件一起完整重编译；本次最终构建已重编译受影响单元。新增身份查找和内存池 free 扫描会增加相应路径开销。
- **原有取舍保留**：事件 CLEAR 顺序消费、非传递式 PI、heap 只向前合并导致的碎片化、串行 timer 回调对后续回调的延迟均未改成另一套语义。它们需要在应用设计中考虑，不应与本轮已复现的链表/状态错误混淆。

## 8. 复现方法

从工程根目录执行；脚本使用当前工程安装路径、COM3 和已连接的 STM32 板卡。全套会多次重建并烧录测试固件。

```powershell
Set-Location E:\STM32_Project\CubeIDE\f446_rtos

# 本轮全部最终测试；任一编译/运行/汇总不符合预期即失败
powershell.exe -NoProfile -ExecutionPolicy Bypass -File hil\audit_20261012\run_final.ps1

# 隔离的 PRIMASK 配置编译检查
powershell.exe -NoProfile -ExecutionPolicy Bypass -File hil\audit_20261012\compile_primask.ps1

# 测试结束后恢复正常 CubeIDE 工程并重建
powershell.exe -NoProfile -ExecutionPolicy Bypass -File hil\restore_project.ps1 -Rebuild

# 再将日常固件下载到板子，观察启动输出
powershell.exe -NoProfile -ExecutionPolicy Bypass -File hil\run_hil.ps1 `
    -Elf Debug\f446_rtos.elf -Tag daily_repeat -Seconds 15
```

测试入口及夹具说明见 [hil/README.md](../hil/README.md)。当前工作区和板上已经完成上述恢复步骤；再次跑全套后需要再次恢复。
