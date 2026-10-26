# RTOS — 面向通用 Cortex-M 的自研嵌入式实时内核

一个从零设计、配有[设计文档](docs/rtos_design.md)的可移植抢占式 RTOS：`RTOS/Core/` 提供调度、任务和 IPC 等服务，`RTOS/Port/` 提供处理器移植层；应用侧附带 USB 设备栈（CDC 虚拟串口 + MSC U 盘 + HID 三合一复合设备）、端侧 AI 推理（int8 量化 CNN 说话人识别）与 HardFault 诊断示例，并配有 FreeRTOS V10.4.6 的[对照分析](docs/freertos_design.md)。

**当前验证平台**：STM32F446RET6（Cortex-M4F @ 168MHz，STM32CubeIDE + GCC）。内核代码不含任何 ST 头文件引用；换一颗 Cortex-M3/M4/M7 芯片只需替换移植层与目标 SDK 工程，见[移植指南](#移植到其他芯片)。

**2026-10-13 验证汇总**：已完成内核及 USB 处理路径修复、底层性能优化和 16 任务／双硬件中断综合压力测试。五轮正常压力测试逐条验证 2,433,735 条成功接纳消息，161 项验收检查在每轮均通过；包含 15 分钟持续运行和单包损坏检出对照。日常固件已恢复。详细改动、性能收益与退步、验证范围及证据入口见[本次交付汇总](docs/rtos_release_20261013.md)。物理 USB 总线传输未包含在本轮验证中。

**2026-10-25 临界区修复**：进出临界区时以短暂 PRIMASK 事务保护共享计数、保存槽位和屏蔽恢复，并在退出恢复屏蔽后执行 ISB，保证已请求的 PendSV 先于阻塞 API 的后续结果读取被识别。默认 BASEPRI 模式的主体仍允许优先级 0–4 的纯硬件 ISR 抢占；这些 ISR 仍不得调用操作内核对象的 API，关闭断言也不放宽契约。缺陷复现、此前测试缺口及新增验证范围见 [BASEPRI 修复报告](docs/rtos_basepri_fix_20261025.md)。

**2026-10-26 复杂并发加固**：补充锁链、循环依赖、任务删除、ISR 销毁窗口及多等待者交接测试；修复传递优先级继承、对象状态检查窗口和递归计数溢出。复现方法、配置矩阵、业务压力及清理记录见[复杂并发测试报告](docs/rtos_adversarial_20261026.md)。

本批已复现缺陷、修复行为、验证结果和保留状态统一整理在[缺陷修复汇总](docs/rtos_fix_summary_20261026.md)。

```
+------------------------------------------------------------------+
| 用户任务        rtos_task_delay / rtos_sem_take / rtos_queue_send |
+------------------------------------------------------------------+
| API 层                                                         |
|   rtos_task.c   任务管理/删除/通知        rtos_sched.c  调度决策    |
|   rtos_sem.c    二值/计数信号量           rtos_mutex.c  互斥锁+继承 |
|   rtos_queue.c  消息队列                  rtos_event.c  事件组      |
|   rtos_timer.c  软件定时器(守护任务)      rtos_heap.c   内存堆      |
|   rtos_usb.c    USB子系统+OSAL适配        rtos_perf.c   性能监视    |
+------------------------------------------------------------------+
| 内部服务        rtos_internal.h —— 阻塞/唤醒/超时定位等调度服务     |
+------------------------------------------------------------------+
| 移植层          Port/GCC/ARM_CM7/port.c（兼容 M4F）                |
|                 PendSV / SVC / SysTick / BASEPRI / 上下文切换      |
+------------------------------------------------------------------+
```

> 当前架构、寄存器现场、API 契约及工程集成见 [设计手册](docs/rtos_design.md)，配有状态、等待归属和执行时序图；修复、优化及实板验证结果见 [交付汇总](docs/rtos_release_20261013.md)。

---

## 内核特性

### 调度

- **32 级抢占式优先级调度**：就绪表用位图 + `__builtin_ctz`（RBIT、CLZ 两条指令）定位最高就绪优先级，O(1) 选择；
- **同优先级时间片轮转**：tick 驱动，可配置关闭；
- **1kHz tick**（可配），`delay` / `delay_until`（周期任务防漂移）两种延时语义；
- **延时管理**：当前使用一条按唤醒 tick 排序的双向链，表头缓存表尾；摘除 O(1)，追加命中快路径时 O(1)，中间插入 O(n)。tick 比较采用带符号差值，有限等待范围见[当前边界](#当前边界与已知限制)；
- **决策/执行分离**：调度决策收口在 `rtos_sched.c`，切换执行统一由 PendSV 完成——决策在临界区内、切换在临界区外，天然合并多次切换请求。
- **挂起与切换保护**：调度挂起支持嵌套，挂起期间冻结内核 tick 并拒绝新阻塞；PendSV 提交切换前重新检查候选任务的状态。
- **临界区事务保护**：默认 BASEPRI 配置下，BASEPRI 屏蔽 syscall 域中断，PRIMASK 仅在进出事务中保护共享元数据；调用者更强的 BASEPRI 及原 PRIMASK 状态得到保留。出口 ISB 同步解屏蔽与后续执行，避免阻塞结果被提前读取。NMI/HardFault 不得调用临界区原语或操作内核对象；详见[设计手册第 4.5 节](docs/rtos_design.md#45-临界区-进出与屏障)。

### IPC（五种机制，语义各有分工）

| 机制 | 文件 | 语义要点 |
|---|---|---|
| 信号量 | `rtos_sem.c` | 二值/计数；存在等待者时计数为零，give 优先把令牌交给等待者，避免重复计数 |
| 互斥锁 | `rtos_mutex.c` | 有界求解锁依赖图，传播并撤销继承；递归溢出返回错误，删除持锁任务时移交；ISR 禁用，循环死锁仍需应用避免 |
| 消息队列 | `rtos_queue.c` | 环形缓冲 + 拷贝传递；ISR 投递非阻塞 |
| 事件组 | `rtos_event.c` | ANY/ALL 等待 + CLEAR_ON_EXIT + 满足位回传（信箱模式） |
| 任务通知 | `rtos_task.c` | 通知值内嵌 TCB，无需独立 IPC 对象或对象等待链；有限超时仍使用内核延时链 |

信号量使用等待者优先级位图，队列、互斥锁及事件组使用有序等待链。位图定位优先级不代表整条等待链操作为 O(1)：信号量尾插、寻找候选和清理共享优先级位仍可能扫描链表。

### 内存

- **静态资源与动态任务共存**：`rtos_task_create_static` 使用调用方提供的 TCB 和栈，`rtos_task_create` 从通用堆分配 TCB 和栈；IPC 对象及队列存储由调用方提供，USB OSAL 使用固定对象池；
- 双档固定块 USB 堆（小块 64B / 大块 512B）：分配 O(1)，释放为检测重复归还而做有界扫描，最坏 O(n)；通用堆使用 first-fit 和相邻高地址块合并。

### 可观测性（调试即功能）

- **性能监视器**（`rtos_perf.c`，DWT 周期级精度）：CPU 占用率、每任务运行时间、切换频率、调度延迟、栈高水位（0xA5 染料法）；
- **栈溢出双检测**：栈底 magic 字 + 边界检查两档；
- **HardFault 诊断与九类故障示例**：保留 BusFault、INVSTATE、UNDEFINSTR、DIVBYZERO、非对齐、栈边界及 ISR 故障示例；当前 `task_uart` 中的命令派发被注释，需启用 `ft_dispatch` 后按菜单触发，见 [hardfault_debug.md](docs/hardfault_debug.md)。

## USB 子系统

基于 **CherryUSB 裁剪版**（仅保留 CDC/MSC/HID 设备栈 + DWC2 端口，共 6 个 `.c`），通过 [rtos_usb_osal.c](RTOS/Core/Src/rtos_usb_osal.c) 的 26 函数 OSAL 契约对接自研内核：

- **三合一复合设备**：单 USB 口同时枚举出虚拟串口（CDC ACM）+ 32KB RAM 盘 U 盘（MSC）+ 自定义 HID（64B IN/OUT 报告）；
- **静态对象池 OSAL**：信号量/互斥锁/队列/线程/定时器全部池化，无堆分配；
- **DWC2 自定义 FIFO**：适配 F446 OTG_FS 的 320 字 FIFO 与 6 端点限制（三类功能共需 4 个 IN 端点，靠同端点号不同方向复用解决）；
- MSC 后端三个回调（`get_cap/sector_read/sector_write`）即存储介质抽象——当前是 RAM 盘，换 SPI Flash / SD 卡只改这三个函数。

完整移植步骤（OSAL 契约、胶水层、换芯片 IP 的 DCD 契约）见 [rtos_design.md 第十三章](docs/rtos_design.md)。

## 端侧 AI：int8 量化说话人识别

一条从训练到 MCU 推理的完整 TinyML 流水线，演示"RTOS 事件驱动 + 端侧推理"的组合：

下图描述应用预留的数据路径。当前 `main.c` 中音频攒帧与事件置位代码被注释，默认固件的 `algo_task` 等待事件，不会仅因串口收到 2048 字节就自动运行推理。

```
训练侧(PC)                                部署侧(MCU)
voice_classifier.pth ──cnn2c.py──▶  ai_param.c/h   int8 权重数组(约130K参数)
  (PyTorch, 1D-CNN)     --quant int8  ai_app.c/h    量化推理运行时
                                    conv.c/h      Conv1d/BN/ReLU/MaxPool/Linear 算子
                                        │
USART2 音频字节流 ─▶ uart2_rx_queue ─▶ task_uart 攒满 2048 字节
                                        │ rtos_event_set(位0x01)
                                        ▼
                              algo_task 被事件唤醒
                              ai_forward_u8(audio_buf, logits, &class_id)
                              → "无人声 / 其他人 / 注册说话人1 / 注册说话人2"
                              → 打印类别与推理耗时(tick)
```

- **模型**：1D-CNN 说话人分类器（3 个 Conv+BN+ReLU+MaxPool 块 → FC 4032→32→4），4 类输出；
- **[cnn2c.py](AI/model/cnn2c.py)**：通用的 PyTorch→C 转换脚本——递归遍历模型按实际层生成代码，不固定网络结构；`--quant int8` 生成**动态量化**版本（激活 scale 每帧在线计算，无需校准数据集）；
- **推理接口**：`ai_forward()`（float 输入）/ `ai_forward_u8()`（uint8 输入，省一次转换），返回 logits 与 argmax 类别；
- `algo_task`、USB 和诊断任务由 RTOS 调度；启用音频输入路径后可进行业务验证。各任务共享地址空间，当前内核不提供 MPU 内存隔离。

## 移植到其他芯片

内核与硬件的全部耦合收敛在两处：

1. **`RTOS/Port/` 移植层**（当前提供 GCC/ARM_CM7，已在 Cortex-M4F 上验证）：负责 PendSV/SVC 上下文切换、BASEPRI 或 PRIMASK 临界区和 tick。预定义宏用于选择部分处理器特性，不替代新目标的完整适配和验证；M0/M0+ 需要对应端口，不能仅修改屏蔽宏就视为移植完成；
2. **`rtos_config.h`**：配置 tick 频率、性能注册表容量、BASEPRI 阈值及功能裁剪。目标 MCU 的中断优先级位数、FPU 和缓存契约仍需结合 SDK 核对；本轮实板验证平台为 F446RE。

内核不提供启动文件/链接脚本/时钟配置——这些属于目标 SDK（如 CubeMX 工程）的职责，用户只需把 `Core/` 与 `Port/` 源文件加入 SDK 工程编译。USB 子系统换其他 USB IP（fsdev/chipidea 等）的完整移植步骤见 [rtos_design.md 第十三章](docs/rtos_design.md)。

## 演示工程（STM32F446RET6 验证平台）

| 项 | 值 |
|---|---|
| MCU | STM32F446RET6（Cortex-M4F @ 168MHz，LQFP64） |
| 控制台 | USART2 @ PA2(TX)/PA3(RX)，115200-8N1 |
| USB | OTG_FS @ PA11(DM)/PA12(DP) |
| IDE | STM32CubeIDE 1.13+（GCC arm-none-eabi） |

默认日常固件创建以下任务；测试变体通过 `FIRMWARE_*` 宏选择，默认关闭自测、性能和压力任务：

| 任务 | 演示内容 |
|---|---|
| `task0` / `task1` | 每 1000 / 500 tick 打印运行信息 |
| `task_uart` | 接收 USART2 队列字节；命令派发及音频攒帧当前被注释 |
| `algo_task` | 等待音频事件后推理；默认没有音频事件生产者 |
| `task_usb_cdc` | 虚拟串口回显 |
| `task_usb_msc` | U 盘扇区读写统计 |
| `task_usb_hid` | 每秒 64B HID 输入报告，输出报告回显 |
| `ft1`~`ft8` | 阻塞等待故障触发通知；第九类由 USART2 ISR 执行，需先启用命令派发 |

## 快速开始

1. **导入**：STM32CubeIDE → File → Import → Existing Projects，选择本仓库根目录（工程名 `f446_rtos`）；
2. **编译烧录**：Build → 下载 `Debug/f446_rtos.elf`（CherryUSB 头文件路径已在 `.cproject` 配好）；
3. **观察与验证**：
   - USART2（115200-8N1）应输出系统初始化菜单及 `task0` / `task1` 周期信息；
   - 若需要故障或音频演示，先在 `task_uart` 启用对应输入处理，再单独验证；故障触发后需复位；
   - USB 配置预期提供 **CDC + 32KB RAM 盘 + HID**。需连接目标 OTG_FS 数据接口，并实际检查枚举、控制请求和传输；ST-LINK 虚拟串口日志不能替代这些验证。

## 目录结构

```
.
├── Core/                     # 演示应用层（CubeMX 生成 + 手写演示）
│   ├── Src/main.c           # 任务编排/USB/AI推理/HardFault测试/命令菜单
│   ├── Src/usb_otg.c        # OTG_FS 时钟 GPIO NVIC(HAL 层 glue)
│   └── Src/stm32f4xx_it.c   # 中断向量(HardFault 诊断入口/USB ISR 挂接)
├── RTOS/
│   ├── Core/                # ★ 内核与 USB 子系统(芯片无关)
│   │   ├── Src/rtos_{sched,task,sem,mutex,queue,event,timer,heap,mem,perf}.c
│   │   ├── Src/rtos_usb.c   # USB 子系统(对象池/复合设备/MSC RAM 盘)
│   │   ├── Src/rtos_usb_osal.c  # CherryUSB OSAL 适配层
│   │   └── Inc/             # 对外 API + rtos_internal.h + rtos_config.h
│   ├── Port/GCC/ARM_CM7/    # ★ 移植层 port.c(PendSV/SVC/BASEPRI, 兼容 M4F)
│   └── lib/third/CherryUSB/ # CherryUSB 裁剪版(仅 CDC/MSC/HID/DWC2 设备栈)
├── AI/                      # ★ 端侧 AI 流水线
│   ├── model/cnn2c.py       # PyTorch→C 转换工具(支持 int8 动态量化)
│   ├── model/voice_classifier.pth  # 训练好的说话人分类模型
│   └── output/int8/         # 生成的 C 推理代码(param/app/算子库, 已编入固件)
├── docs/                    # 设计、历史分析、审计、优化与验证报告
├── hil/                     # 实板测试夹具、脚本及原始文本证据
├── Drivers/                 # ST HAL + CMSIS(Apache-2.0, 仅演示工程用)
└── .cproject / .project     # CubeIDE 工程配置
```

> `Core/` 与 `Drivers/` 属于 F446 演示工程。移植需要加入内核及端口源码、配置启动与中断，并核对时钟、栈、FPU 和内存布局后执行验证。

## 配置裁剪

全部开关集中在 [`RTOS/Core/Inc/rtos_config.h`](RTOS/Core/Inc/rtos_config.h)，常用项：

| 宏 | 默认 | 说明 |
|---|---|---|
| `RTOS_CONFIG_TICK_RATE_HZ` | 1000 | tick 频率 |
| `RTOS_CONFIG_MAX_PRIORITIES` | 32 | 优先级数（决定就绪表 RAM） |
| `RTOS_CONFIG_MAX_TASKS` | 16 | 用于派生性能注册表容量，不是内核静态任务池或创建数量上限 |
| `RTOS_CONFIG_PERF_TASK_REGISTRY_SIZE` | 18 | 默认 `MAX_TASKS + 2`；超过容量的任务仍可创建，但注册表观测不完整 |
| `RTOS_CONFIG_USE_TIMERS` | 1 | 软件定时器（关 = 零开销） |
| `RTOS_CONFIG_USE_PERF_MONITOR` | 1 | 性能监视器（DWT 周期级） |
| `RTOS_CONFIG_CHECK_FOR_STACK_OVERFLOW` | 2 | 栈溢出检测（0/1/2 三档） |
| `RTOS_CONFIG_USE_USB` | 1 | USB 子系统总开关 |
| `RTOS_CONFIG_USB_USE_MSC` / `_USE_HID` | 1 / 1 | 复合设备裁剪（全关 = 仅 CDC） |
| `RTOS_CONFIG_USE_BASEPRI` | 1 | 1 使用 BASEPRI 临界区，0 使用 PRIMASK；两种配置均在 F446 上验证，不能据此推断已有 M0/M0+ 端口 |

## 文档

| 文档 | 内容 |
|---|---|
| [rtos_fix_summary_20261026.md](docs/rtos_fix_summary_20261026.md) | **当前修复总览**：五项已复现缺陷、契约加固、最终验证及提交范围 |
| [rtos_design.md](docs/rtos_design.md) | **当前架构与实现手册**（16 章）：调度不变式 / PendSV 与 FPU 栈帧 / IPC 契约与寿命 / USB 适配 / 统计边界 / API 上下文矩阵 / 资源预算与业务关停 |
| [rtos_performance_report.md](docs/rtos_performance_report.md) | **性能测试报告**：微基准 BM0–BM7 + 系统级场景 S1–S5 + 与 FreeRTOS V10.3.1 的逐项对照；含布局敏感性与已作废结论的更正记录 |
| [rtos_testing_report.md](docs/rtos_testing_report.md) | **历史测试汇总**：基础功能与边界夹具、早期审计取证、方法教训及覆盖缺口；后续终验见下列带日期的报告 |
| [rtos_basepri_fix_20261025.md](docs/rtos_basepri_fix_20261025.md) | **BASEPRI 临界区修复**：确定性交错复现、出口 ISB 与阻塞返回同步、优先级契约守卫及新增综合 IRQ 验证范围 |
| [rtos_adversarial_20261026.md](docs/rtos_adversarial_20261026.md) | **复杂并发回归**：传递继承、销毁窗口、递归边界、60 配置与 900 秒业务压力 |
| [hardfault_debug.md](docs/hardfault_debug.md) | HardFault 寄存器级诊断手册（CFSR 逐位 / 栈帧归因 / 任务 vs ISR 判定） |
| [freertos_design.md](docs/freertos_design.md) | FreeRTOS V10.4.6 逐段核对的源码剖析（23 章）+ 互斥锁 / 竞争 / 崩溃排障大全 |
| [compilation_pipeline.md](docs/compilation_pipeline.md) | 编译链接原理（从预处理到 map 文件） |
| [rtos_release_20261013.md](docs/rtos_release_20261013.md) | **2026-10-13 交付汇总**：该批改动分类、真实验证结果、证据范围和复现条件 |
| [rtos_audit_20261012.md](docs/rtos_audit_20261012.md) | 源码审计、调度／IPC／定时器／USB 修复与专项回归 |
| [rtos_optimization_20261012.md](docs/rtos_optimization_20261012.md) | 第一阶段性能试验、保留方案、撤回方案与测量局限 |
| [rtos_lowlevel_optimization_20261012.md](docs/rtos_lowlevel_optimization_20261012.md) | 当前底层实现、PendSV／信号量优化和上下文／FPU 检查 |
| [rtos_production_stress_20261013.md](docs/rtos_production_stress_20261013.md) | 16 任务、两路真实定时器中断、混合 IPC、背压恢复及 900 秒压力运行 |

> 传统测试套件的**一键复现在 `hil\`**：`powershell -ExecutionPolicy Bypass -File hil\repro_all.ps1`
> （自动切换固件变体、烧录、抓串口）。后续优化与综合压力使用独立入口，见 [HIL 说明](hil/README.md)。历史版本报告归档在 `hil\archive_docs\`。

## 当前边界与已知限制

- **有限等待范围仍为 `1..0x7FFFFFFF` tick**（1kHz 下最大约 24.8 天）：当前调度器只有一个 `delay_head`，维护一条双向延时链；未实现 current / overflow 双延时链。`rtos_sched_block()` 将更大的等待值设为永久等待，`0xFFFFFFFF` 是 `RTOS_WAIT_FOREVER`。软件定时器拒绝超过有限范围的周期，USB 毫秒转换饱和到该上界；`delay_until` 调用方也应遵守有限周期范围，详见[设计手册的延时与定时器章节](docs/rtos_design.md)；
- **tick 回绕与有限范围是两个问题**：当前采用有符号差值比较来处理限定范围内的跨回绕等待；不能据此声称支持任意 32 位有限延时；
- **延时链中间插入为 O(n)**，表尾快路径需新唤醒点不早于当前表尾，周期任务并非必然命中；
- **继承不提供死锁检测**：当前会沿锁依赖传递及撤销捐赠；循环锁依赖仍须采用统一加锁顺序或有限超时，图重算成本随锁和等待者数量增长；
- **通用堆释放时仅尝试合并紧邻的高地址空闲块**，不会向低地址查找或循环合并整段连续空闲块，长期分配仍需关注碎片。

完整债务清单与 FreeRTOS 逐项对照见 [freertos_design.md 第十二章](docs/freertos_design.md)。

## 第三方组件

| 组件 | 目录 | 许可 |
|---|---|---|
| CherryUSB（裁剪版） | `RTOS/lib/third/CherryUSB` | Apache-2.0 |
| STM32 HAL / CMSIS | `Drivers/` | Apache-2.0 |

自研内核与应用代码的许可由仓库作者声明（待补充 LICENSE 文件）。
