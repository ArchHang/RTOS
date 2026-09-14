# RTOS — 面向通用 Cortex-M 的自研嵌入式实时内核

一个从零设计、[逐行文档化](docs/rtos_design.md)的可移植抢占式 RTOS：内核与芯片解耦（`Core/` 提供全部内核逻辑、`Port/` 提供移植层），不绑定任何具体 MCU 或 SDK；应用侧附带 USB 设备栈（CDC 虚拟串口 + MSC U 盘 + HID 三合一复合设备）、端侧 AI 推理（int8 量化 CNN 说话人识别）与可实战的 HardFault 诊断体系，并配有与 FreeRTOS V10.4.6 官方源码逐段核对的[对照分析](docs/freertos_design.md)。

**当前验证平台**：STM32F446RET6（Cortex-M4F @ 180MHz，STM32CubeIDE + GCC）。内核代码不含任何 ST 头文件引用；换一颗 Cortex-M3/M4/M7 芯片只需替换移植层与目标 SDK 工程，见[移植指南](#移植到其他芯片)。

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

> 内核 C 侧约 3300 行 + 移植汇编约 120 行，全部中文注释；四个功能域（任务/IPC/定时器/USB）的每一行设计动机都可在 [docs/](docs/) 找到对应章节。

---

## 内核特性

### 调度

- **32 级抢占式优先级调度**：就绪表用位图 + `CLZ/CTZ` 单指令选任务，O(1) 决策；
- **同优先级时间片轮转**：tick 驱动，可配置关闭；
- **1kHz tick**（可配），`delay` / `delay_until`（周期任务防漂移）两种延时语义；
- **决策/执行分离**：调度决策收口在 `rtos_sched.c`，切换执行统一由 PendSV 完成——决策在临界区内、切换在临界区外，天然合并多次切换请求。

### IPC（五种机制，语义各有分工）

| 机制 | 文件 | 语义要点 |
|---|---|---|
| 信号量 | `rtos_sem.c` | 二值/计数；"等待者非空 ⟺ 计数为零"不变式杜绝双重记账 |
| 互斥锁 | `rtos_mutex.c` | **重算式优先级继承**（遍历等待者取最高，非 FreeRTOS 的简化版）；递归放行；ISR 禁用 |
| 消息队列 | `rtos_queue.c` | 环形缓冲 + 拷贝传递；ISR 投递非阻塞 |
| 事件组 | `rtos_event.c` | ANY/ALL 等待 + CLEAR_ON_EXIT + 满足位回传（信箱模式） |
| 任务通知 | `rtos_task.c` | TCB 内嵌直达，零链表零拷贝——ISR→任务最快路径 |

### 内存

- **全静态对象池**：任务/信号量/互斥锁/队列/事件组/定时器/USB 对象全部池化，无碎片、启动即确定；
- 双档固定块 USB 堆（小块 64B / 大块 512B，O(1)）+ first-fit 前向合并通用堆（可选）。

### 可观测性（调试即功能）

- **性能监视器**（`rtos_perf.c`，DWT 周期级精度）：CPU 占用率、每任务运行时间、切换频率、调度延迟、栈高水位（0xA5 染料法）；
- **栈溢出双检测**：栈底 magic 字 + 边界检查两档；
- **9 级 HardFault 活体测试**（串口发 `1`~`9` 触发）：BusFault（precise/imprecise）、INVSTATE、UNDEFINSTR、DIVBYZERO、非对齐、TCB 越界写、栈溢出——每类故障配寄存器级分流表，见 [hardfault_debug.md](docs/hardfault_debug.md)。

## USB 子系统

基于 **CherryUSB 裁剪版**（仅保留 CDC/MSC/HID 设备栈 + DWC2 端口，共 6 个 `.c`），通过 [rtos_usb_osal.c](RTOS/Core/Src/rtos_usb_osal.c) 的 26 函数 OSAL 契约对接自研内核：

- **三合一复合设备**：单 USB 口同时枚举出虚拟串口（CDC ACM）+ 32KB RAM 盘 U 盘（MSC）+ 自定义 HID（64B IN/OUT 报告）；
- **静态对象池 OSAL**：信号量/互斥锁/队列/线程/定时器全部池化，无堆分配；
- **DWC2 自定义 FIFO**：适配 F446 OTG_FS 的 320 字 FIFO 与 6 端点限制（三类功能共需 4 个 IN 端点，靠同端点号不同方向复用解决）；
- MSC 后端三个回调（`get_cap/sector_read/sector_write`）即存储介质抽象——当前是 RAM 盘，换 SPI Flash / SD 卡只改这三个函数。

完整移植步骤（OSAL 契约、胶水层、换芯片 IP 的 DCD 契约）见 [rtos_design.md 第十三章](docs/rtos_design.md)。

## 端侧 AI：int8 量化说话人识别

一条从训练到 MCU 推理的完整 TinyML 流水线，演示"RTOS 事件驱动 + 端侧推理"的组合：

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
- 部署后 `algo_task` 与 USB 任务、HardFault 测试任务并行运行，互不干扰——RTOS 任务隔离的实际演示。

## 移植到其他芯片

内核与硬件的全部耦合收敛在两处：

1. **`Port/` 移植层**（当前提供 GCC/ARM_CM7，兼容 Cortex-M4F；覆盖 STM32F4/F7/L4/H7 等同内核型号）：需要实现上下文切换（PendSV/SVC 汇编）、临界区（BASEPRI 或 PRIMASK——M0/M0+ 用后者）、tick 来源三件事；按 `__ARM_ARCH_7EM__` 等编译器预定义宏自动选择，无需用户指定型号；
2. **`rtos_config.h`**：全部编译期开关集中于此（任务池容量/tick 频率/BASEPRI 阈值/功能裁剪），关闭的功能编译为空、零 RAM 零代码。

内核不提供启动文件/链接脚本/时钟配置——这些属于目标 SDK（如 CubeMX 工程）的职责，用户只需把 `Core/` 与 `Port/` 源文件加入 SDK 工程编译。USB 子系统换其他 USB IP（fsdev/chipidea 等）的完整移植步骤见 [rtos_design.md 第十三章](docs/rtos_design.md)。

## 演示工程（STM32F446RET6 验证平台）

| 项 | 值 |
|---|---|
| MCU | STM32F446RET6（Cortex-M4F @ 180MHz，LQFP64） |
| 控制台 | USART2 @ PA2(TX)/PA3(RX)，115200-8N1 |
| USB | OTG_FS @ PA11(DM)/PA12(DP) |
| IDE | STM32CubeIDE 1.13+（GCC arm-none-eabi） |

上电后并行运行的任务：

| 任务 | 演示内容 |
|---|---|
| `task_uart` | 串口命令分发：数字键触发 HardFault 测试，其余字节攒入音频缓冲 |
| `algo_task` | 事件唤醒的 AI 推理（见上一节） |
| `task_usb_cdc` | 虚拟串口回显 |
| `task_usb_msc` | U 盘扇区读写统计 |
| `task_usb_hid` | 每秒 64B HID 输入报告，输出报告回显 |
| `ft1`~`ft8` | 九级 HardFault 测试（`1`~`9` 触发，每次后需复位） |

## 快速开始

1. **导入**：STM32CubeIDE → File → Import → Existing Projects，选择本仓库根目录（工程名 `f446_rtos`）；
2. **编译烧录**：Build → 下载 `Debug/f446_rtos.elf`（CherryUSB 头文件路径已在 `.cproject` 配好）；
3. **体验**：
   - USART2 终端出现系统菜单，`1`~`9` 触发各级 HardFault 测试；
   - 串口持续发送音频字节流，攒满 2048 字节后打印说话人识别结果与耗时；
   - 插 USB：主机枚举出 **COMx + 32KB U 盘 + HID 设备**；U 盘首次需格式化为 FAT；HID 每秒收到 64 字节报告（首字节 0x5A）。

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
├── docs/                    # ★ 四篇工程级中文文档
├── Drivers/                 # ST HAL + CMSIS(Apache-2.0, 仅演示工程用)
└── .cproject / .project     # CubeIDE 工程配置
```

> `Core/` 与 `Drivers/` 属于 F446 演示工程；把 `RTOS/Core/` 与 `RTOS/Port/` 拷到任何目标 SDK 工程即完成内核移植。

## 配置裁剪

全部开关集中在 [`RTOS/Core/Inc/rtos_config.h`](RTOS/Core/Inc/rtos_config.h)，常用项：

| 宏 | 默认 | 说明 |
|---|---|---|
| `RTOS_CONFIG_TICK_RATE_HZ` | 1000 | tick 频率 |
| `RTOS_CONFIG_MAX_PRIORITIES` | 32 | 优先级数（决定就绪表 RAM） |
| `RTOS_CONFIG_MAX_TASKS` | 16 | 静态任务池容量 |
| `RTOS_CONFIG_USE_TIMERS` | 1 | 软件定时器（关 = 零开销） |
| `RTOS_CONFIG_USE_PERF_MONITOR` | 1 | 性能监视器（DWT 周期级） |
| `RTOS_CONFIG_CHECK_FOR_STACK_OVERFLOW` | 2 | 栈溢出检测（0/1/2 三档） |
| `RTOS_CONFIG_USE_USB` | 1 | USB 子系统总开关 |
| `RTOS_CONFIG_USB_USE_MSC` / `_USE_HID` | 1 / 1 | 复合设备裁剪（全关 = 仅 CDC） |
| `RTOS_CONFIG_USE_BASEPRI` | 1 | M3/M4/M7 用 BASEPRI；**M0/M0+ 必须设 0** |

## 文档

| 文档 | 内容 |
|---|---|
| [rtos_design.md](docs/rtos_design.md) | 内核设计 22 章：调度/IPC/超时定位全流程解剖/USB 子系统与移植/性能监视/已知限制清单 |
| [freertos_design.md](docs/freertos_design.md) | FreeRTOS V10.4.6 逐段核对的源码剖析（23 章）+ 互斥锁/竞争/崩溃排障大全 |
| [hardfault_debug.md](docs/hardfault_debug.md) | HardFault 寄存器级诊断手册（CFSR 逐位/栈帧归因/任务 vs ISR 判定） |
| [compilation_pipeline.md](docs/compilation_pipeline.md) | 编译链接原理（从预处理到 map 文件） |

## 已知限制（诚实清单）

- 单延时链表，tick 计数约 49.7 天回绕（1kHz）——修复方向是 FreeRTOS 式双链对滚，见文档 L1；
- 优先级继承为一跳（链式反转未解，与 FreeRTOS 相同）；
- 通用堆无后向合并（L7）。

完整债务清单与 FreeRTOS 逐项对照见 [freertos_design.md 第十二章](docs/freertos_design.md)。

## 第三方组件

| 组件 | 目录 | 许可 |
|---|---|---|
| CherryUSB（裁剪版） | `RTOS/lib/third/CherryUSB` | Apache-2.0 |
| STM32 HAL / CMSIS | `Drivers/` | Apache-2.0 |

自研内核与应用代码的许可由仓库作者声明（待补充 LICENSE 文件）。
