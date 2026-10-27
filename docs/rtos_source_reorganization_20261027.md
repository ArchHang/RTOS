# RTOS 源码重组与验证记录（2026-10-27）

本轮目标是使源码目录和编译单元符合职责边界，保持已修复的内核运行语义。
重组前基准为 `6602f8fa1b9acb9b12ba27e17f2ffc4fbcb8b03f`。
完整源码导览见 [RTOS/README.md](../RTOS/README.md)，机制说明见[设计手册](rtos_design.md)。

**最终结果：独立验收 PASS，145/145 个实板镜像完成，errors/pending 均为空。**
日常固件已恢复、重新烧录，串口确认任务 0 和任务 1 正常运行；`main.c` 保持原始字节。
完整验收见 [verification_summary.json](../hil/refactor_20261027/verification_summary.json)。

## 1. 拆分与合并的依据

- 将初始化、启动入口、版本和默认弱钩子从 scheduler 提到 `rtos_kernel.c`，使 scheduler 集中维护运行中的时间与调度。
- 将任务文件中的共用等待链、阻塞登记、唤醒与重挂提到 `rtos_wait.c`，供 IPC、超时清理和优先级继承共同使用。
- 任务地址登记、构造/删除、通知与延期回收继续归 task；任务通知直接依赖 TCB 和身份校验，不另造独立对象层。
- 信号量、互斥锁、队列、事件分别管理令牌、所有权、消息和位条件，只归入共同 IPC 目录，不机械拼接实现。
- heap 与 mempool 算法、元数据和时延不同，保留两个分配器；就绪与延时结构复用 TCB 链接，继续由 scheduler 统一维护。
- 合并 USB 的 RTOS 句柄封装和 CherryUSB 命名桥接；把复合设备会话及 CDC/HID 留在 device，把 MSC 存储后端单列。

本轮没有修改内核算法、TCB 布局、临界区协议或上下文栈协议；两个重复的 USB 毫秒转换 wrapper 合并为已有 inline。
等待尾插的 O(n)、阻塞登记返回后才在解除临界区时切换等注释随代码归属同步纠正。

## 2. 文件映射

下表路径均相对 `RTOS/`；函数及头文件名称以实际文件为准。

| 旧位置 | 新位置与范围 |
|---|---|
| `Core/Src/rtos_sched.c` | 保留调度；初始化入口和四个弱钩子提到 `Core/Src/rtos_kernel.c` |
| `Core/Src/rtos_task.c` | 保留任务生命周期与通知；九个共用等待函数提到 `Core/Src/rtos_wait.c` |
| `Core/Src/rtos_{sem,mutex,queue,event}.c` | `IPC/`，四个对象文件分别保留 |
| `Core/Src/rtos_heap.c` | `Memory/rtos_heap.c` |
| `Core/Src/rtos_mem.c` | `Memory/rtos_mempool.c`；公共 `rtos_mem.h` 不改名 |
| `Core/Src/rtos_{timer,perf}.c` | `Services/` |
| `Core/Src/rtos_usb.c` 与 `rtos_usb_osal.c` | `USB/rtos_usb_osal.c`、`rtos_usb_device.c`、`rtos_usb_msc.c` |
| `Core/Inc/rtos_internal.h` | `Core/Private/rtos_internal.h` |
| `Port/GCC/ARM_CM7/port.c` | `Port/GCC/ARM_CM4F_CM7F/port.c` |
| `Core/Inc/` 其余头文件、`lib/third/` | 保留原位置；第三方源码不借本轮改写 |

## 3. 兼容与构建迁移

公共头文件路径、函数名与对象布局保持兼容。依赖私有协议的内核及测试需增加 `Core/Private/` 搜索路径。
自行维护源码清单的工程须替换旧路径、补入新单元并删除旧条目，避免缺失或重复链接；只收集 `Core/Src/` 已不足以集成全部功能。

本工程同步 CubeIDE 配置、HIL 规则和恢复入口；[build_layout.ps1](../hil/build_layout.ps1) 集中维护构建目录。
当前构建包含 **15 个规则目录、57 个编译单元，其中自研 RTOS 为 16 个 C 单元**。
历史报告和 HIL 证据保留测试当时的路径及源码身份，不为消除旧路径而重写证据。

## 4. 已完成的结构与编译检查

| 检查 | 结果与边界 |
|---|---|
| 函数及状态声明 | 263 个函数定义与 63 项全局/静态声明进行 token 比较；一项局部任务 ID 原型迁入私有头 |
| 公共接口 | 16 个公开头文件 token 等价；同时检查 main 与第三方保护文件 |
| 严格编译 | 16 单元 × BASEPRI/PRIMASK × 断言开/关 × O0/O2/O3，共 192 项；另有一项 M7 端口，总计 193 条矩阵记录 |
| M7 记录补充 | 首轮记录缺少参数，已真实重跑并保存 `cm7_supplement.json`；仅编译端口，不代表 M7 上板通过 |
| CubeIDE Debug | 隔离工程实际导入、重新生成规则、编译与链接；GCC 14.3.1、O3、exit 0，57 单元 |
| CubeIDE Release | 同样在隔离工程生成和构建；GCC 14.3.1、Os、exit 0，57 单元 |

严格编译的自研单元按零警告验收。IDE 两种配置各有 **3 条警告**：main 的 `ft_dispatch` 未使用、`audio_cnt` 未使用，以及链接器 LOAD 段 RWX；不能称为整工程零警告。
IDE 镜像未烧录，其 GCC 14.3.1 结果与 GCC 11.3 实板运行分别记录。
token 比较不证明预处理、时序或运行等价，仍需新布局下的真实 HIL。

结构证据见 [layout_equivalence.json](../hil/refactor_20261027/evidence/layout_equivalence.json)，
IDE 证据见 [summary.json](../hil/refactor_20261027/evidence/ide/summary.json)，
编译及实板执行方法见[本批 HIL 说明](../hil/refactor_20261027/README.md)。

## 5. 本轮测试过程中的错误与纠正

**USB 断言来源映射曾被错误迁移。** 本轮将 guard case 13 的 CDC available 误映射到 OSAL，实际实现已迁入 device。
真实 IRQ 触发的内核优先级断言正确，但夹具因预期源码文件不匹配报 FAIL；这是本轮整理引入的测试映射错误。
现已明确修正为 **case 13 → device、case 14 → OSAL**。

错误映射期间的 **103 份旧 capture** 完整保留在
[preflight/before_guard_mapping_fix](../hil/refactor_20261027/evidence/preflight/before_guard_mapping_fix/)，不混入最终验收。
修正后 [guard_fixed_smoke](../hil/refactor_20261027/evidence/preflight/guard_fixed_smoke/) 已通过；最终批次 **145 个镜像全部重新采样并完成验收**，
不复用先前 PASS，也不放宽源码身份检查。最终结果以独立验收输出为准。

首次日常采集另有结束文本错误：命令期待 `Task 0 is running`，源码实际输出为 `task 0 running`。
日志已显示两个任务持续运行，采集仍因结束文本不匹配而失败；记录和失败验收保留在
[daily_terminator_mismatch](../hil/refactor_20261027/evidence/preflight/daily_terminator_mismatch/)。核对 main 的真实文本后，
修正采集参数及离线验收条件，重新烧录同一 ELF 并成功采集，未修改应用或内核源码。

首次完整构建另暴露启动汇编 `-x assembler-with-cpp` 参数放在输入文件之后的警告，已修正权威模板的参数顺序。
[build_first_audit.txt](../hil/refactor_20261027/evidence/preflight/build_first_audit.txt) 保留原始输出；该修正属于构建规则，不修改内核代码。

## 6. 清理与最终验收

隔离 IDE 工作副本已清理 **702 个文件、51,317,351 字节**，保留构建日志、规则、配置与源码身份证据；
记录见 [IDE cleanup.json](../hil/refactor_20261027/evidence/ide/cleanup.json)。另清理严格编译目标、旧端口生成目录和
Python 缓存，以及有相同字节归档副本的重复构建日志，共 **403 个文件、1,762,391 字节**，并移除旧源码端口空目录；未删除 Git 跟踪文件。
本机 ELF、原始日志和 192 份 `.su` 证据保留，见[正式批次清理记录](../hil/refactor_20261027/evidence/cleanup.json)。

| 最终交付项 | 状态 |
|---|---|
| 复杂并发矩阵 | 60/60 镜像，35,232 项检查通过 |
| 端口、阻塞返回、ISR 契约及非法嵌套 | 74/74 镜像；7200 确定性交错、1200 万临界区配对、6000 阻塞尝试、120 非法嵌套用例及 44 guard 镜像通过 |
| 功能、边界与底层 | 审计 117、自测 432（另 10 INFO）、边界 103、LC 24、PC 22 均通过；BM0–BM7 完整 |
| 业务压力 | 容量 1/8/32 各 30 秒，每轮 165/0；校验和负向对照按预期检出 1 个错误，163 PASS/3 FAIL，单独分类 |
| 120 秒持续压力 | 263,308 条成功消息全部校验；165/0，worker_errors=0；高优先级 IRQ 2,225,807 次、三层嵌套 2735 次；888,178 次屏蔽检查无泄漏；堆及最大块 28,496 B 前后一致 |
| 最终源码与固件身份 | 每轮 160 项输入一致；串口、SWD、ELF、结束标记及配置独立核对，最终 145/145 验收通过 |
| 日常固件恢复 | 全部 19 项构建文件恢复模板；HIL 副本和 stamp 清除，真实重新烧录，两个演示任务输出正常 |
| 正式批次临时目标清理 | 已清理 403 个生成文件；7 份跟踪的全局构建日志还原初始版本；日常构建产物及新批次证据保留 |

同为 F446/O2 的单次微基准与 10 月 26 日保留记录相比：临界区 40→40、切换 238→239、信号量往返
1693→1732、信号量唤醒 489→497、队列唤醒 579→573、创建删除 6456→6444、delay1 168000→168000、
ISR 唤醒 553→562 周期。存在约 -1.0%～+2.3% 的观测变化；不同镜像布局和单次测量不足以归因或给出 WCET。

本轮不据目录重组声称性能提高，不声称消除全部 BUG，也不把 USB 包装层或寄存器测试等同于物理 USB 主机传输通过。
