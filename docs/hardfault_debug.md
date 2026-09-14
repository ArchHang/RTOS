# HardFault 诊断完整技术手册

> **适用平台**：STM32F446RE（Cortex-M4F，ARMv7-M）+ 自研 RTOS + GCC (arm-none-eabi)
> **文档目标**：从架构原理出发，逐位剖析 HardFault 排查涉及的每一个寄存器，并给出可直接集成的完整诊断代码
> **内存布局基准**（见 STM32F446RETX_FLASH.ld）：FLASH 0x08000000（512K）、RAM 0x20000000（128K，0x20000000~0x2001FFFF）
> **控制台**：USART2 @ 115200-8-N-1（`_write` 经 HAL_UART_Transmit 重定向，本方案不依赖它）

---

## 目录

- [第一章 故障模型：错误如何走到 HardFault](#第一章-故障模型错误如何走到-hardfault)
- [第二章 异常入口：硬件自动保存的现场](#第二章-异常入口硬件自动保存的现场)
  - [2.1 压栈帧布局](#21-压栈帧布局)
  - [2.2 EXC_RETURN 逐位解码](#22-exc_return-逐位解码)
- [第三章 故障状态寄存器逐位详解](#第三章-故障状态寄存器逐位详解)
  - [3.1 CFSR — MMFSR（bit0~7）](#31-cfsr--mmfsrbit07)
  - [3.2 CFSR — BFSR（bit8~15）](#32-cfsr--bfsrbit815)
  - [3.3 CFSR — UFSR（bit16~31）](#33-cfsr--ufsrbit1631)
  - [3.4 HFSR](#34-hfsr)
  - [3.5 MMAR / BFAR](#35-mmfar--bfar)
  - [3.6 SHCSR](#36-shcsr)
  - [3.7 CCR](#37-ccr)
  - [3.8 AFSR / DFSR（简述）](#38-afsr--dfsr简述)
  - [3.9 栈帧中的 xPSR](#39-栈帧中的-xpsr)
- [第四章 本项目上下文特记](#第四章-本项目上下文特记)
- [第五章 排查决策树](#第五章-排查决策树)
- [第六章 常见故障指纹速查表](#第六章-常见故障指纹速查表)
- [第七章 完整实现代码](#第七章-完整实现代码)
  - [7.1 使能代码（main.c）](#71-使能代码mainc)
  - [7.2 诊断模块（stm32f4xx_it.c）](#72-诊断模块stm32f4xx_itc)
  - [7.3 四个异常入口的改写](#73-四个异常入口的改写)
- [第八章 输出示例与符号化](#第八章-输出示例与符号化)
- [第九章 验证方法：故意触发各类故障](#第九章-验证方法故意触发各类故障)
- [第十章 零代码方案与生产期扩展](#第十章-零代码方案与生产期扩展)

---

# 第一章 故障模型：错误如何走到 HardFault

Cortex-M 实现了**嵌套故障升级链**：

```
MemManage 故障 (CFSR.MMFSR) ──┐
                              │ 禁用 / 处理中再出错 / 优先级不够
BusFault    (CFSR.BFSR)  ────┼──→ HardFault (HFSR.FORCED=1)
                              │
UsageFault  (CFSR.UFSR)  ────┘
```

**关键事实**：复位后 MemManage/BusFault/UsageFault 三类故障默认**禁用**，任何此类错误都直接升级为 HardFault，且**对应状态位不会写入 CFSR**（证据丢失）。因此诊断的第一前提是在初始化时使能它们（见 7.1 节）——使能后错误进入各自的处理函数，状态寄存器留下完整记录。

HardFault 自身的优先级固定为 **-1**，仅次于 NMI(-2) 和复位：

- 进入 HardFault 后，所有可配置优先级的中断（0~15）全部被阻塞 → SysTick 停、PendSV 不再执行、**RTOS 调度天然冻结**。诊断代码单线程运行，无任何并发问题，也**不受本项目 BASEPRI 临界区影响**（BASEPRI 只能屏蔽 0~15 级）。
- 若诊断代码自身再次触发故障（如解引用无效 SP），处理器进入 **LOCKUP** 状态，只有复位/调试器能救——因此代码必须先验证 SP 合法性再读栈帧（见 7.2 节 `fault_sp_valid`）。

---

# 第二章 异常入口：硬件自动保存的现场

进入任何异常（含 HardFault）**之前**，硬件自动把 8 个寄存器压入**当时正在使用的栈**（PSP 或 MSP，由被打断上下文决定）。

## 2.1 压栈帧布局

```
高地址
┌──────────────┐
│    xPSR      │ sp+0x1C  bit24=Thumb位, bit[7:0]=IPSR(被打断上下文的异常号)
│    PC        │ sp+0x18  ★ 出错指令(精确故障) / 出错点附近(非精确故障)
│    LR        │ sp+0x14  △ 条件有效: 故障在被调函数内=调用点; 任务顶层=残留值(见2.2)
│    R12       │ sp+0x10
│    R3        │ sp+0x0C
│    R2        │ sp+0x08
│    R1        │ sp+0x04
│    R0        │ sp+0x00
├──────────────┤ ← 传给 C 分析函数的 SP(压栈后的栈指针)
│ (扩展帧才有) │   S0~S15(16字) + FPSCR(1字) + 对齐保留(1字) = 再加 17 字
└──────────────┘
低地址
```

- **基本帧**：8 字（32 字节），EXC_RETURN.bit4=1。
- **扩展帧**：26 字（104 字节），任务执行过 FPU 指令后中断进入时为该格式，EXC_RETURN.bit4=0。本项目 FPCCR 开了惰性堆叠（ASPEN|LSPEN），S0-S15 的实际保存延迟到 FPU 指令真正执行，但**帧空间仍会预留**——解析 PC/LR 用不到 S 寄存器，扩展帧只需知道"SP 比基本帧低 68 字节"即可。
- **SP 本身**：`SP_at_fault = 传给 C 函数的帧指针`。栈溢出判定就用它对比任务栈边界。注意若压栈失败（MSTKERR/STKERR），此值可能已非法。

## 2.2 栈帧中 LR 的真实语义（2026-08 真机实测教训）

**常见误解**：*"LR 指向出错位置的调用者，用 LR 定位错误"*。这只在一半场景下成立。

**机制**：LR(R14) 是 AAPCS 的 **caller-saved 寄存器**——任何 `BL` 都会覆盖它；函数经 `pop {pc}` 返回时**不恢复 LR 寄存器**。因此任意时刻的 LR 值 = **最近一次执行的 BL 所设置的返回地址**。

| 故障位置 | LR 的含义 | 有效性 |
|----------|----------|--------|
| 在**被调函数内部**出错（如 `memcpy` 里访问野指针） | LR = 该函数的返回地址 = **调用点**（如调用 memcpy 的那行代码） | ✅ 有效，配合 PC 可还原"调用点→出错指令"两级现场 |
| 在**任务顶层函数自身**出错（如任务代码里的野指针访问） | LR = 该任务此前最后一次调用链中，**最深函数内部的 BL 返回地址**——残留垃圾 | ❌ 无效，与故障位置无关 |

**实测案例**（测试 1）：`ftask_bus` 在 `puts` 返回后执行 `ldr r3,[r4,#0]`（读 0x20020000）触发故障。此时 LR=0x0800649B 指向 `_puts_r+0x72`——那是 `_puts_r` 内部 `bl __retarget_lock_release_recursive` 的返回地址（puts 调用链的最后残留），与故障毫无关系。而 **PC=0x0800249C 正是出错的那条 ldr**，addr2line(PC) 直接命中源码行。

**定位方法论**：
1. **PRECISERR（精确故障）：PC 就是出错指令**——addr2line(PC) 一步到位，这是第一优先；
2. LR 只作辅助：仅当故障发生在被调函数内时，addr2line(LR) 给出调用点；
3. 更深的调用链需要**栈回溯**（在栈内存中扫描 Flash 地址范围内的字并逐个符号化），当前 dump 未实现（见第十章扩展方向）。

## 2.3 EXC_RETURN 逐位解码

异常进入后，LR 被硬件改写为 **EXC_RETURN**（0xFFFFFFE1~0xFFFFFFFD），低 5 位携带返回信息：

| 位 | 掩码 | 值 | 含义 | 本项目典型场景 |
|----|------|----|------|----------------|
| bit4 | 0x10 | 1 | **基本帧**（8 字，无 FPU 上下文） | 任务未执行过 FPU 指令 |
|      |      | 0 | **扩展帧**（26 字，含 S0-S15+FPSCR） | 任务用过 float（EXC_RETURN=0xFFFFFFED） |
| bit3 | 0x08 | 1 | 返回**线程模式**（错误发生在任务里） | 任务上下文（0xFFFFFFFD/ED） |
|      |      | 0 | 返回 **Handler 模式**（错误发生在 ISR 里） | ISR 上下文（0xFFFFFFF1/E1） |
| bit2 | 0x04 | 1 | 使用 **PSP**（帧在任务栈上） | 本项目所有任务 |
|      |      | 0 | 使用 **MSP**（帧在主栈上） | ISR、启动阶段（0xFFFFFFF1/F9） |
| bit1~0 | 0x3 | - | ARMv7-M 中保留/仅 ARMv8-M 安全扩展使用 | 恒为 0b01 |

本项目实际取值速查：

| EXC_RETURN | 帧位置 | 模式 | 帧型 |
|-----------|--------|------|------|
| 0xFFFFFFF1 | MSP | ISR | 基础 |
| 0xFFFFFFE1 | MSP | ISR | 扩展 |
| 0xFFFFFFF9 | MSP | 线程（调度器启动前） | 基础 |
| 0xFFFFFFFD | PSP | 线程（任务，未用 FPU） | 基础 |
| 0xFFFFFFED | PSP | 线程（任务，用过 FPU） | 扩展 |

**判读要点**：`bit3=0`（ISR 内出错）时，`rtos_kernel.current_tcb` 只是被打断的任务，**不是肇事者**——此时要用栈帧 xPSR 的 IPSR 字段定位是哪个中断。

---

# 第三章 故障状态寄存器逐位详解

以下寄存器全部位于系统控制块（SCB，基址 0xE000ED00）。除特别说明外，状态位均为**粘滞位：写 1 清除**——诊断代码只读不清，复位前证据一直在。

## 3.1 CFSR — MMFSR（bit0~7）

CFSR（0xE000ED28）低字节为存储器管理故障状态（MPU 违规 / XN 区域执行）：

| bit | 名称 | 置位含义 | 排查方向 |
|-----|------|----------|----------|
| 0 | IACCVIOL | 取指地址违反 MPU 规则或落入 XN（永不执行）区 | PC 是否跑进数据区/外设区 |
| 1 | DACCVIOL | 数据读写违反 MPU 权限（如写只读区） | 看 MMFAR 指向；查越界写 |
| 2~3 | - | 保留 | - |
| 4 | **MSTKERR** | 异常**压栈**时总线/权限错误 | **任务栈溢出第一症状**：SP 已越界，压栈写失败 |
| 5 | MUNSTKERR | 异常**出栈**时错误 | 栈在异常处理期间被破坏 |
| 6 | MLSPERR | 惰性 FPU 状态保存失败 | 任务栈溢出（S 寄存器落栈时越界） |
| 7 | **MMARVALID** | MMFAR（bit 对应 0xE000ED34）内容有效 | =1 才可信任 MMFAR |

## 3.2 CFSR — BFSR（bit8~15）

中字节为总线故障状态（取指/数据访问的总线层错误）：

| bit | 名称 | 置位含义 | 排查方向 |
|-----|------|----------|----------|
| 8 | IBUSERR | **指令预取**总线错误（通常紧随跳转野指针） | 查 PC 与跳转来源 LR |
| 9 | **PRECISERR** | **精确**数据总线错误：PC 就是出错指令，BFAR 有效 | ★ 最好定位的一类：addr2line(PC) + 查 BFAR |
| 10 | **IMPRECISERR** | **非精确**（写缓冲异步）错误：**PC 与 BFAR 均不可靠** | 回溯最近的"写外设/野指针"代码；调试期可置 ACTLR.DISDEFWBUF 换精确性（性能换定位） |
| 11 | UNSTKERR | 异常出栈时总线错误 | 栈被写穿/破坏 |
| 12 | **STKERR** | 异常压栈时总线错误 | **栈溢出第一症状**（同 MSTKERR） |
| 13 | LSPERR | 惰性 FPU 保存的总线错误 | 任务栈溢出 |
| 14 | - | 保留 | - |
| 15 | **BFARVALID** | BFAR（0xE000ED38）内容有效 | =1 才可信任 BFAR |

> 本工程无 MPU，IACCVIOL/DACCVIOL 基本不会出现；总线类故障主要来自访问保留地址区（RAM/FLASH 之外）。

## 3.3 CFSR — UFSR（bit16~31）

高半字为用法故障状态：

| bit | 名称 | 置位含义 | 排查方向 |
|-----|------|----------|----------|
| 16 | **UNDEFINSTR** | 未定义指令（PC 落进**数据**，如字面量池、被改写的代码） | 查反汇编该地址是否合法指令；本项目 port.c 文档记录过 `.ltorg` 放错位置即此症 |
| 17 | **INVSTATE** | EPSR.T=0 状态下执行（典型：跳转目标地址 bit0=0，非 Thumb） | 函数指针被破坏/跳转表越界；栈帧 xPSR.bit24 通常=0 可佐证 |
| 18 | INVPC | 非法 EXC_RETURN 使用（如在中断里 `BX 0xFFFFFFF9`） | 检查手写汇编/异常返回逻辑 |
| 19 | **NOCP** | 协处理器禁用时执行协处理器指令（CP10/11=FPU） | FPU 初始化前用了 float（CPACR 未开）；查启动顺序 |
| 20~23 | - | 保留（ARMv7-M；STKOF 属 ARMv8-M） | - |
| 24 | UNALIGNED | 非对齐访问（仅 CCR.UNALIGN_TRP=1 时陷阱） | 强转指针对齐问题 |
| 25 | DIVBYZERO | SDIV/UDIV 除零（仅 CCR.DIV_0_TRP=1 时陷阱） | 未判除数 |

## 3.4 HFSR

0xE000ED2C，HardFault 状态（**非粘滞，读清除由硬件语义管理**）：

| bit | 名称 | 置位含义 | 排查方向 |
|-----|------|----------|----------|
| 1 | VECTTBL | 取异常向量时总线错误 | VTOR 配错/向量表区损坏 |
| 30 | **FORCED** | **由可配置故障升级而来** → 必须去读 CFSR 找根因 | 本手册的主路径 |
| 31 | DEBUGEVT | 调试事件（断点/观察点）升级 | 调试器相关，非程序 bug |

## 3.5 MMFAR / BFAR

| 寄存器 | 地址 | 有效条件 | 内容 |
|--------|------|----------|------|
| MMFAR | 0xE000ED34 | MMFSR.MMARVALID(bit7)=1 | MemManage 故障的访问地址 |
| BFAR | 0xE000ED38 | BFSR.BFARVALID(bit15)=1 | 总线故障的访问地址 |

**必须先查 VALID 位再用地址**，否则读到的是上次故障的陈旧值。

## 3.6 SHCSR

0xE000ED24，系统 Handler 控制与状态：

| bit | 名称 | 说明 |
|-----|------|------|
| 0 / 1 / 3 | MEMFAULTACT / BUSFAULTACT / USGFAULTACT | 对应故障处理**正在执行**（诊断中=故障嵌套） |
| 11 / 15 / 10 | PENDSVACT / SVCALLACT / SYSTICKACT | 对应异常 active（判断故障打断点） |
| 12~14 | *FAULTPENDED | 故障被挂起（被更高优先级抢占） |
| **16** | **MEMFAULTENA** | 使能 MemManage 故障（脱离升级链） |
| **17** | **BUSFAULTENA** | 使能 BusFault |
| **18** | **USAGEFAULTENA** | 使能 UsageFault |

7.1 节的初始化就是置位 bit16~18（`SCB->SHCSR |= 7UL<<16`）。

## 3.7 CCR

0xE000ED14，配置与控制（诊断相关的两个调试陷阱开关）：

| bit | 名称 | 置位含义 |
|-----|------|----------|
| 3 | UNALIGN_TRP | 所有非对齐访问触发 UsageFault（默认 0=允许） |
| 4 | DIV_0_TRP | 除零触发 UsageFault（默认 0=返回 0 不报错） |

## 3.8 AFSR / DFSR（简述）

- **AFSR**（0xE000ED3C）：辅助故障状态，厂商自定义（ST 用于实现 Fault Analyzer 扩展，一般读 0）。
- **DFSR**（0xE000ED30）：调试故障状态（HALT/ breakpoints 等），调试器使用，软件诊断不依赖。

## 3.9 栈帧中的 xPSR

栈帧 `sp+0x1C` 处是被打断上下文的组合程序状态字：

| 位域 | 名称 | 诊断意义 |
|------|------|----------|
| bit24 | **T 位（Thumb）** | =0 → 必然 INVSTATE（跳去了非 Thumb 地址），与 UFSR.INVSTATE 互为佐证 |
| bit[7:0] | **IPSR（异常号）** | 被打断上下文当时所在的异常号：**0=线程模式（任务）**；3=HardFault（嵌套）；14=PendSV；15=SysTick；16+n=外部中断 n（如 USART2_IRQn=38 → IPSR=54） |

> EXC_RETURN.bit3 告诉你"硬件层面"是线程还是 Handler；栈帧 IPSR 告诉你"被打断瞬间"是否在某个 ISR 里——两者结合即可精确定位故障上下文。

---

# 第四章 本项目上下文特记

1. **栈使用约定**：所有任务运行于 **PSP**（EXC_RETURN=0xFFFFFFFD/ED）；所有中断与启动阶段运行于 **MSP**（0xFFFFFFF1/F9）。诊断代码本身在 Handler 模式、用 MSP。
2. **栈溢出判定的陷阱**：`RTOS_CONFIG_PERF_STACK_WATERMARK=1` 时，任务创建流程会先用 `0xDEADBEEF` 写栈底、随后被 0xA5A5A5A5 水印填充**覆盖**（见 rtos_task.c 注释）。因此**不能**通过读 `*(uint32_t*)stack_base` 判断溢出；应检查 **TCB 字段 `tcb->stack_magic`**（永不被覆盖）或直接比较 SP 与 `stack_base`。
3. **任务栈元数据**：`rtos_tcb_t` 中 `stack_base`（栈底=低地址）、`stack_size`（**单位：字，×4 才是字节**）、`task_id`、`name[16]`、`priority` 均可直接打印；`rtos_kernel.current_tcb` 全局可访问。
4. **调度已冻结**：HardFault 期间 SysTick/PendSV（优先级 0xFF）无法抢占，`current_tcb` 稳定可读。
5. **输出经弱函数 fault_output（默认寄存器直写）**：诊断模块所有文本经唯一的弱函数 `fault_output` 送出（7.2 节）。默认实现是 **USART2 寄存器轮询直写**——故障上下文中唯一安全的输出方式（printf/HAL 在故障处理内可能引发级联故障，见 5.1 节实战案例）。用户可在任意 .c 定义同名强符号覆盖为 ITM/SWO、其他 UART 或 RAM 黑匣子等通道。
6. **SP 合法区间**：本链接脚本 RAM 为 0x20000000~0x2001FFFF。CCM（0x10000000，64K）当前未用；若日后任务栈放入 CCM，需扩展 7.2 节 `fault_sp_valid` 的区间表。

---

# 第五章 排查决策树

```
进入 fault_report(sp, exc_return)
│
├─ 1. 快照 CFSR/HFSR/SHCSR/MMFAR/BFAR（只读不清）
│
├─ 2. SP 合法？(0x20000000 ≤ sp ≤ 0x2001FFFF-32, 4 字节对齐)
│     └─ 否 → 压栈失败(MSTKERR/STKERR?) → 只报 SP 与寄存器状态, 不解析帧
│
├─ 3. 解析帧: PC / LR / xPSR
│     ├─ xPSR.T=0 ? → 跳到非 Thumb 地址(配合 UFSR.INVSTATE)
│     └─ xPSR.IPSR≠0 ? → 出错点在某个 ISR 内(异常号=IPSR)
│
├─ 4. EXC_RETURN.bit3 = 1 (线程模式)?
│     ├─ 是 → current_tcb 归因任务名/优先级/栈边界
│     │        └─ sp < stack_base ? → ★ 栈溢出实锤
│     └─ 否 → "错误在 ISR 内", current_tcb 仅供参考
│
├─ 5. HFSR.FORCED = 1 ? → 解码 CFSR 三段
│     ├─ UFSR: UNDEFINSTR/INVSTATE/NOCP/UNALIGNED/DIVBYZERO
│     ├─ BFSR: PRECISERR(+BFAR★) / IMPRECISERR(不可定位) / STKERR(栈溢出)
│     └─ MMFSR: DACCVIOL(+MMFAR) / MSTKERR(栈溢出)
│
├─ 6. 输出全部证据 → addr2line(PC/LR) 符号化
│
└─ 7. 处置: 开发期 while(1) 等调试器 / 生产期 noinit 黑匣子+复位
```

---

## 5.1 实战案例：printf 默认实现引发的级联故障（2026-08 真机实测）

**现象**（测试 1：任务读 0x20020000 触发 BusFault）：

1. 先进 BusFault_Handler，再进 HardFault_Handler，打印了两次 dump 头部；
2. 第一次 dump 在 `EXC_RETURN=` 处截断，十六进制值始终缺失；
3. 第二次 dump 只有头部一行，随后 CubeIDE 调试会话自行终止（LOCKUP）。

**完整因果链**（工程当时 `fault_output` 默认实现为 printf）：

```
ft1 读 0x20020000 → BusFault(优先级0) → dump 开始
 ① printf("===== FAULT DUMP =====\r\n")  含'\n'且 _isatty=1 → stdout 行缓冲
    刷新 → 输出可见 ✓
 ② printf("EXC_RETURN=")                无'\n' → 滞留 stdout 缓冲(不可见)
 ③ fault_puthex32 → printf("%s", buf)   ← 库代码(vfprintf/strlen/memcpy
    的非对齐字访问优化 + UNALIGN_TRP 已开启)触发第二次故障
    → hex 值从未进入缓冲 → 输出中永久缺失
    → 故障发生在 BUSFAULTACT(优先级0)期间、新故障同级 → 升级 HardFault
 ④ HardFault_Handler → 第二次 dump
    printf("\r\n\r\n=====...=====\r\n") 含'\n' → 刷新 → 把滞留的
    "EXC_RETURN=" 一并带出 → 与现象 2 的输出完全吻合
 ⑤ 第二次 dump 中再次故障 → HARDFAULTACT(-1) 期间再故障 → LOCKUP
    → 调试器掉线(现象 3)
```

**三条教训**：

1. **故障处理器内禁止任何库函数**——printf/vfprintf/strlen/memcpy 均为正常上下文编译，其优化（非对齐字访问）在故障上下文 + UNALIGN_TRP 环境下就是新的故障源；
2. **故障处理运行在优先级 0，其中任何故障都必然级联升级**（同级 pending → HardFault → LOCKUP），因此 dump 路径必须零依赖且配嵌套保护；
3. **stdout 行缓冲会静默吞掉无 '\n' 的输出**，使故障现场的截断点产生误导——寄存器直写逐字节即时输出，无此问题。

**修复**（已实施）：`fault_output` 默认实现改为 USART2 寄存器直写（零库依赖）；`fault_report` 入口增加嵌套故障保护——若 dump 路径再次故障，用原始通道报告第二次故障的 CFSR/HFSR 后停机（不再执行可能再次故障的完整 dump），杜绝 LOCKUP 且调试器保持连接；printf 降级为用户可选的覆盖实现（注释中注明风险与 fflush 建议）。

### 5.2 第二轮：GCC store-merging 生成的非对齐写（同日真机实测）

第一轮修复后再次实测测试 1，仍出现 BusFault → HardFault 级联，但这次**嵌套保护捕获了元凶**：

```
*** NESTED FAULT inside fault dump! ***
2nd CFSR=0x01008200  HFSR=0x40000000  ...
           │ │└─ BFSR: BFARVALID|PRECISERR(首次 BusFault 的粘滞位)
           │ └── UFSR: 0x0100 = UNALIGNED  ← 第二次故障实锤
```

**根因**（反汇编定位）：`fault_puthex32` 用 `char buf[11]` 局部缓冲拼接输出，-O3 的 **store-merging** 把 `buf[2..5]` 四个字节写合并成一条：

```asm
str.w  r2, [sp, #10]     ; SP 4对齐 +10 → 地址恒为 2 mod 4, 非对齐整字写
```

这在 Cortex-M3/M4/M7 上是**架构支持的合法操作**（普通 LDR/STR 支持非对齐，GCC 依赖此特性优化），但 `CCR.UNALIGN_TRP=1` 把它陷阱化 → 故障处理内再触发 UNALIGNED UsageFault → 同级（优先级 0）无法抢占 → 升级 HardFault。

**修复**（已实施，双管齐下）：

1. **按用户建议移除 UNALIGN_TRP**——非对齐 ldr/str 在主流 MCU 上不是错误，该陷阱只该用于专项排查对齐敏感代码，常态开启连 GCC 自己生成的合法代码都会误伤（`DIV_0_TRP` 保留：除零不改代码生成行为，语义明确）；
2. **dump 打印函数根治为"逐字符输出"**（`fault_puthex32`/`fault_putdec` 不再使用局部缓冲拼接）：每字符"单字节写 + 外部函数调用"，字节写天然无对齐要求、调用屏障阻止任何 store-merging——即使将来重新开启陷阱，故障处理也不会再死（防御纵深）。

**测试 6 相应变更**：普通 `ldr` 非对齐已不会触发，改用 **LDRD**——`ldrd/ldm/stm/ldrex` 类指令架构上**永久禁止**非对齐（与 UNALIGN_TRP 无关），非对齐基址必触发 UFSR.UNALIGNED，测试覆盖不缩水。

**第四条教训**：开启任何"把合法行为变成故障"的调试陷阱（UNALIGN_TRP/DIV_0_TRP/MPU）前，必须意识到它会作用于**全部**代码——包括编译器优化生成的指令和故障处理器自身；故障处理器这类极端上下文应做到对其免疫。

### 5.3 最终方案：printf 默认 + 三层防护

两轮事故后确立的架构（当前实现）：

| 层 | 机制 | 防护对象 |
|----|------|----------|
| 1 | `fault_output` 默认 printf + **fflush** | 行缓冲丢尾段；UNALIGN_TRP 已关闭使库代码非对齐优化不再构成故障源 |
| 2 | 打印函数**逐字符输出**（无局部缓冲拼接） | store-merging 生成的非对齐写——即使将来重开陷阱也免疫 |
| 3 | **嵌套故障保护** + fault_raw_* 零依赖原始通道 | 任何残余故障源（如故障恰发生在 UART/HAL/stdio 内部）——兜底报告后停机，不进 LOCKUP |

设计逻辑：日常开发享受 printf 的便利（走 `_write` 重定向，与系统日志同通道）；极端场景由 2、3 层兜底，保证"最坏情况也有输出、调试器不掉线"。用户仍可定义强符号 `fault_output` 切换到寄存器直写等更鲁棒通道。

---

# 第六章 常见故障指纹速查表

| 指纹组合 | 根因（按概率排序） | 修复方向 |
|----------|--------------------|----------|
| UFSR.INVSTATE + 栈帧 xPSR.T=0 | 函数指针/跳转表被写坏；调用地址 bit0=0 | 检查 PC 来源的函数指针初始化与越界写 |
| UFSR.UNDEFINSTR | PC 落进数据（字面量池/常量表）；代码区被改写 | 反汇编 PC 附近；核对 asm 中 `.ltorg` 位置 |
| UFSR.NOCP | 调度器启动前（FPU 未使能）执行了 float 指令 | float 移到任务内或提前开 CPACR |
| BFSR.PRECISERR + BFARVALID | 野指针/数组越界/访问保留区 | `addr2line(PC)` 定位指令，BFAR 即非法地址 |
| BFSR.IMPRECISERR（无 BFARVALID） | 写缓冲异步错误：外设寄存器非法写、DMA 目标错 | 不可直接定位；临时置 ACTLR.DISDEFWBUF=1 复测 |
| MMFSR.MSTKERR 或 BFSR.STKERR | **任务栈溢出**（压栈失败） | 增大该任务栈；用 perf 栈水印核实 |
| UFSR.DIVBYZERO（需 DIV_0_TRP） | 除数为 0 | 补除数检查 |
| UFSR.UNALIGNED（需 UNALIGN_TRP） | 强转指针非对齐访问 | 修正对齐 |
| HFSR.VECTTBL | 向量表取指失败 | 检查 VTOR 与向量表链接位置 |
| SP 落在 RAM 区间外 / PC=0x00000000 | 空指针调用；栈彻底写穿 | 检查 NULL 指针；查栈边界 |

---

# 第七章 完整实现代码

全部代码落在 **stm32f4xx_it.c 的 USER CODE 区**（CubeMX 重新生成时保留）+ **main.c 一行使能**。不需要新建文件、不改动构建系统。

## 7.1 使能代码（main.c）

```c
int main(void)
{
  /* ... HAL_Init / 时钟 / 外设初始化 ... */

  /* USER CODE BEGIN 2 */
  /* HardFault 诊断前置: 使能三类可配置故障。
   * 复位默认它们是禁用的 —— 任何此类错误都会升级为 HardFault,
   * 且 CFSR 不留任何记录(HFSR 只剩 FORCED=1, 根因不可知)。
   * 使能后错误进入各自 Handler, CFSR 留下完整位级证据。 */
  SCB->SHCSR |= ( 7UL << 16 );    /* bit18 USAGEFAULTENA | bit17 BUSFAULTENA | bit16 MEMFAULTENA */

  /* DIV_0_TRP: 把静默除零(默认返回0)变成可定位故障(测试3需要)。 */
  SCB->CCR |= SCB_CCR_DIV_0_TRP;

  /* ★ 不要开启 UNALIGN_TRP(2026-08 真机事故教训, 见 5.1 节):
   * 普通 ldr/str 的非对齐访问在 M3/M4/M7 上是架构支持的合法操作,
   * GCC 的 store-merging 优化会主动生成之 —— 开启陷阱会误伤正常
   * 代码甚至故障处理自身。需要测试 UFSR.UNALIGNED 时, 用 ldrd/ldm
   * 类永久禁止非对齐的指令触发(见第九章测试6)。 */
  /* SCB->CCR |= SCB_CCR_UNALIGN_TRP; */
  /* USER CODE END 2 */

  /* ... rtos_init / rtos_start ... */
}
```

## 7.2 诊断模块（stm32f4xx_it.c）

```c
/* USER CODE BEGIN Includes */
#include "rtos_sched.h"    /* rtos_kernel.current_tcb (任务归因) */
#include "rtos_task.h"     /* rtos_tcb_t 完整定义(name/stack_base/...) */
#include "rtos_config.h"   /* RTOS_CONFIG_CHECK_FOR_STACK_OVERFLOW */
/* USER CODE END Includes */
```

```c
/* USER CODE BEGIN 0 */
/* ==================================================================== */
/*                        HardFault 故障诊断模块                          */
/* ==================================================================== */

/* ---------- 异常入口硬件压栈帧(基本帧 8 字, ARMv7-M 固定布局) ---------- */
typedef struct
{
    uint32_t r0;      /* sp+0x00 */
    uint32_t r1;      /* sp+0x04 */
    uint32_t r2;      /* sp+0x08 */
    uint32_t r3;      /* sp+0x0C */
    uint32_t r12;     /* sp+0x10 */
    uint32_t lr;      /* sp+0x14 ★ 调用者返回地址 */
    uint32_t pc;      /* sp+0x18 ★ 出错指令(precise)/附近(imprecise) */
    uint32_t xpsr;    /* sp+0x1C   bit24=T位, bit[7:0]=IPSR */
} fault_frame_t;

/* ---------- 故障现场快照(全局 volatile: 调试器/Live Expressions 直接看) ---------- */
volatile struct
{
    uint32_t      cfsr, hfsr, shcsr, ccr;
    uint32_t      mmfar, bfar;
    uint32_t      sp, exc_return;
    fault_frame_t frame;
    uint8_t       frame_valid;
} g_fault;

/* ---------- 原始输出通道: USART2 寄存器轮询直写(零库依赖) ---------- */
#define FAULT_UART      USART2
#define FAULT_TXE_GUARD 100000U

static void fault_raw_putc(char c)
{
    uint32_t guard = FAULT_TXE_GUARD;
    while (((FAULT_UART->SR & USART_SR_TXE) == 0U) && (--guard != 0U)) {
        /* 等发送数据寄存器空 */
    }
    FAULT_UART->DR = (uint8_t)c;
}

static void fault_raw_puts(const char *s)
{
    while (*s != '\0') {
        fault_raw_putc(*s++);
    }
}

static void fault_raw_puthex32(uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    fault_raw_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        fault_raw_putc(hex[(v >> i) & 0xFU]);
    }
}

/* ---------- 故障文本输出口: 弱函数, 用户可重定义 ----------
 * fault_output 是诊断模块唯一的文本出口, 所有报告内容都经它送出。
 *
 * 默认实现: printf + fflush(stdout)。
 *   - fflush 逐段强制刷新, 规避 stdout 行缓冲在停机时丢失尾部无换行
 *     输出的问题(2026-08 实测教训, 见 5.1 节);
 *   - 前提: CCR.UNALIGN_TRP 已关闭——库代码的非对齐字访问优化不再
 *     构成故障源(见 5.2 节)。若重新开启该陷阱, 请覆盖本函数。
 *
 * 残余风险(可接受): 故障若恰好发生在 UART/HAL/stdio 内部, 本通道可能
 * 失效。此时嵌套保护(fault_report 入口)自动切换到零依赖的
 * fault_raw_* 寄存器直写通道报告嵌套故障, 系统不会进入 LOCKUP。
 *
 * 用户覆盖方式: 在任意 .c 定义同名强符号(需先声明原型):
 *     void fault_output(const char *str);
 * 可选通道:
 *   - fault_raw_puts: 本文件提供的 USART2 寄存器直写(最鲁棒);
 *   - ITM/SWO:        while (*str) ITM_SendChar(*str++);
 *   - 其他 UART:      仿照 fault_raw_puts 修改寄存器基地址;
 *   - RAM 黑匣子:     拷入 .noinit 缓冲, 复位后回放。 */
__attribute__((weak)) void fault_output(const char *str)
{
    (void)printf("%s", str);
    (void)fflush(stdout);
}

/* 内部统一出口(便于将来增加过滤/缓冲层) */
static void fault_puts(const char *s)
{
    fault_output(s);
}

/* 32 位十六进制: 栈上拼 "0x........" 后一次性输出 */
static void fault_puthex32(uint32_t v)
{
    static const char hex[16] = "0123456789ABCDEF";
    char buf[11];

    buf[0] = '0';
    buf[1] = 'x';
    for (uint32_t i = 0; i < 8U; i++) {
        buf[2U + i] = hex[(v >> (28U - 4U * i)) & 0xFU];
    }
    buf[10] = '\0';
    fault_output(buf);
}

/* 无符号十进制(最长 10 位): 逆序生成后原地反转 */
static void fault_putdec(uint32_t v)
{
    char buf[11];
    uint32_t i = 0;

    if (v == 0U) {
        fault_output("0");
        return;
    }
    while ((v > 0U) && (i < 10U)) {
        buf[i] = (char)('0' + (v % 10U));
        v /= 10U;
        i++;
    }
    for (uint32_t j = 0; j < (i / 2U); j++) {
        char t = buf[j];
        buf[j] = buf[i - 1U - j];
        buf[i - 1U - j] = t;
    }
    buf[i] = '\0';
    fault_output(buf);
}

/* ---------- SP 合法性校验(防止二次异常进 LOCKUP) ----------
 * 读帧之前必须确认: SP 在 RAM 内、留得下 8 字帧、4 字节对齐。
 * 本工程 RAM: 0x20000000 ~ 0x2001FFFF(见 STM32F446RETX_FLASH.ld)。
 * 若压栈失败(MSTKERR/STKERR), SP 可能已指向非法地址。 */
static uint8_t fault_sp_valid( uint32_t sp )
{
    if( ( sp < 0x20000000UL ) ||
        ( sp > ( 0x20020000UL - sizeof( fault_frame_t ) ) ) )
    {
        return 0U;
    }
    return ( ( sp & 0x3U ) == 0U ) ? 1U : 0U;
}

/* ---------- CFSR 解码: 把置位翻译成人话 ---------- */

static void fault_decode_mmfsr( uint32_t cfsr )
{
    uint8_t v = (uint8_t)( cfsr & 0xFFU );        /* CFSR bit0~7 */
    if( v == 0U ) { return; }
    fault_puts( "\r\n  [MMFSR] " );
    if( v & ( 1U << 0 ) ) { fault_puts( "IACCVIOL(取指违反XN/MPU) " ); }
    if( v & ( 1U << 1 ) ) { fault_puts( "DACCVIOL(数据访问违规) " ); }
    if( v & ( 1U << 4 ) ) { fault_puts( "MSTKERR(压栈失败, 极可能栈溢出!) " ); }
    if( v & ( 1U << 5 ) ) { fault_puts( "MUNSTKERR(出栈失败) " ); }
    if( v & ( 1U << 6 ) ) { fault_puts( "MLSPERR(惰性FPU保存失败) " ); }
    if( v & ( 1U << 7 ) ) { fault_puts( "MMARVALID " ); }
}

static void fault_decode_bfsr( uint32_t cfsr )
{
    uint8_t v = (uint8_t)( ( cfsr >> 8 ) & 0xFFU );  /* CFSR bit8~15 */
    if( v == 0U ) { return; }
    fault_puts( "\r\n  [BFSR] " );
    if( v & ( 1U << 0 ) ) { fault_puts( "IBUSERR(指令预取错误) " ); }
    if( v & ( 1U << 1 ) ) { fault_puts( "PRECISERR(精确总线错, PC/BFAR可信) " ); }
    if( v & ( 1U << 2 ) ) { fault_puts( "IMPRECISERR(非精确, PC/BFAR不可信!) " ); }
    if( v & ( 1U << 3 ) ) { fault_puts( "UNSTKERR(出栈失败) " ); }
    if( v & ( 1U << 4 ) ) { fault_puts( "STKERR(压栈失败, 极可能栈溢出!) " ); }
    if( v & ( 1U << 5 ) ) { fault_puts( "LSPERR(惰性FPU保存失败) " ); }
    if( v & ( 1U << 7 ) ) { fault_puts( "BFARVALID " ); }
}

static void fault_decode_ufsr( uint32_t cfsr )
{
    uint16_t v = (uint16_t)( ( cfsr >> 16 ) & 0xFFFFU );  /* CFSR bit16~31 */
    if( v == 0U ) { return; }
    fault_puts( "\r\n  [UFSR] " );
    if( v & ( 1U << 0 ) ) { fault_puts( "UNDEFINSTR(未定义指令/执行了数据) " ); }
    if( v & ( 1U << 1 ) ) { fault_puts( "INVSTATE(跳到非Thumb地址) " ); }
    if( v & ( 1U << 2 ) ) { fault_puts( "INVPC(非法EXC_RETURN) " ); }
    if( v & ( 1U << 3 ) ) { fault_puts( "NOCP(FPU未使能却执行浮点指令) " ); }
    if( v & ( 1U << 8 ) ) { fault_puts( "UNALIGNED(非对齐访问) " ); }
    if( v & ( 1U << 9 ) ) { fault_puts( "DIVBYZERO(除零) " ); }
}

static void fault_decode_hfsr( uint32_t hfsr )
{
    fault_puts( "\r\n  [HFSR] " );
    if( hfsr & ( 1UL << 30 ) ) { fault_puts( "FORCED(由可配置故障升级, 根因在CFSR) " ); }
    if( hfsr & ( 1UL << 1 ) )  { fault_puts( "VECTTBL(向量表取指失败) " ); }
    if( hfsr & ( 1UL << 31 ) ) { fault_puts( "DEBUGEVT(调试事件) " ); }
}

/* ---------- 主分析函数(运行于 Handler 模式/MSP, 全部中断已被冻结) ----------
 * used 属性: 本函数仅被 naked Handler 内的汇编 "b fault_report" 引用,
 * GCC 不解析 basic asm 内容, 会误判"未使用"而不生成符号 → 必须强制保留。 */
static __attribute__( ( used ) ) void fault_report( uint32_t sp, uint32_t exc_return )
{
    /* 1. 立即快照故障寄存器(只读不清: 状态位写1才清除, 保留现场)
     *    后续解析代码万一二次异常, 快照里证据仍在, 调试器可查 g_fault。 */
    g_fault.cfsr    = SCB->CFSR;                              /* 0xE000ED28 */
    g_fault.hfsr    = SCB->HFSR;                              /* 0xE000ED2C */
    g_fault.shcsr   = SCB->SHCSR;                             /* 0xE000ED24 */
    g_fault.ccr     = SCB->CCR;                               /* 0xE000ED14 */
    g_fault.mmfar   = ( g_fault.cfsr & ( 1UL << 7 ) ) ? SCB->MMFAR : 0xFFFFFFFFU;
    g_fault.bfar    = ( g_fault.cfsr & ( 1UL << 15 ) ) ? SCB->BFAR : 0xFFFFFFFFU;
    g_fault.sp          = sp;
    g_fault.exc_return  = exc_return;

    /* 2. EXC_RETURN 解码(见手册第二章) */
    uint8_t from_psp  = ( ( exc_return & ( 1UL << 2 ) ) != 0U ) ? 1U : 0U;
    uint8_t from_thr  = ( ( exc_return & ( 1UL << 3 ) ) != 0U ) ? 1U : 0U;
    uint8_t ext_frame = ( ( exc_return & ( 1UL << 4 ) ) == 0U ) ? 1U : 0U;

    /* 3. 头部 */
    fault_puts( "\r\n\r\n===== FAULT DUMP =====\r\n" );
    fault_puts( "EXC_RETURN=" ); fault_puthex32( exc_return );
    fault_puts( from_psp ? "  frame:PSP" : "  frame:MSP" );
    fault_puts( from_thr ? "  ctx:Thread(task)" : "  ctx:Handler(ISR)" );
    fault_puts( ext_frame ? "  FP-frame(26w)\r\n" : "  basic-frame(8w)\r\n" );

    /* 4. SP 校验 + 栈帧解析 */
    g_fault.frame_valid = fault_sp_valid( sp );
    fault_puts( "SP=" ); fault_puthex32( sp );
    if( g_fault.frame_valid == 0U )
    {
        fault_puts( "  <-- 非法SP! 压栈失败或栈已越界, 寄存器帧不可信\r\n" );
    }
    else
    {
        const fault_frame_t *f = (const fault_frame_t *)sp;
        g_fault.frame = *f;

        fault_puts( "\r\nPC=" );  fault_puthex32( f->pc );
        fault_puts( "  LR=" );    fault_puthex32( f->lr );
        fault_puts( "\r\nR0=" );  fault_puthex32( f->r0 );
        fault_puts( "  R1=" );    fault_puthex32( f->r1 );
        fault_puts( "  R2=" );    fault_puthex32( f->r2 );
        fault_puts( "  R3=" );    fault_puthex32( f->r3 );
        fault_puts( "  R12=" );   fault_puthex32( f->r12 );
        fault_puts( "\r\nxPSR=" ); fault_puthex32( f->xpsr );

        /* T位=0: 跳到了非 Thumb 地址(INVSTATE 的直接佐证) */
        if( ( f->xpsr & ( 1UL << 24 ) ) == 0U )
        {
            fault_puts( "  [T=0 非Thumb状态!]" );
        }
        /* IPSR: 被打断上下文当时所在异常号(0=线程/任务, 14=PendSV, 15=SysTick, 16+n=IRQn) */
        uint32_t ipsr = f->xpsr & 0xFFU;
        fault_puts( "  IPSR=" ); fault_putdec( ipsr );
        if( ipsr >= 16U )
        {
            fault_puts( "(IRQn=" ); fault_putdec( ipsr - 16U ); fault_puts( ")" );
        }
    }

    /* 5. RTOS 归因 —— 仅线程模式(bit3=1)时 current_tcb 才是肇事任务 */
    if( ( from_thr != 0U ) && ( g_fault.frame_valid != 0U ) )
    {
        rtos_tcb_t *tcb = rtos_kernel.current_tcb;
        if( tcb != NULL )
        {
            fault_puts( "\r\nTask: \"" ); fault_puts( tcb->name );
            fault_puts( "\"  prio=" );    fault_putdec( tcb->priority );
            fault_puts( "  id=" );        fault_putdec( tcb->task_id );

            /* 6. 任务栈溢出专项判定:
             *    栈向下生长, [stack_base, stack_base + stack_size*4)。
             *    注意 stack_size 单位是"字"(x4 才是字节)。 */
            fault_puts( "\r\nstack: base=" ); fault_puthex32( (uint32_t)tcb->stack_base );
            fault_puts( "  size=" );         fault_puthex32( tcb->stack_size * 4U );
            if( sp < (uint32_t)tcb->stack_base )
            {
                fault_puts( "  *** SP已低于栈底: 栈溢出! ***" );
            }
            else
            {
                fault_puts( "  margin=" );
                fault_puthex32( sp - (uint32_t)tcb->stack_base );
            }
#if RTOS_CONFIG_CHECK_FOR_STACK_OVERFLOW
            /* TCB 内魔数(栈底副本会被0xA5A5水印覆盖, 不可用, 见手册第四章) */
            if( tcb->stack_magic != 0xDEADBEEFU )
            {
                fault_puts( "  [stack_magic毁坏: 栈底曾被写穿]" );
            }
#endif
        }
    }
    else if( from_thr == 0U )
    {
        fault_puts( "\r\n上下文: 错误发生在 ISR 内, current_tcb 仅是被打断的任务\r\n" );
    }

    /* 7. 故障寄存器解码(为什么错) */
    fault_puts( "\r\nCFSR =" ); fault_puthex32( g_fault.cfsr );
    fault_decode_mmfsr( g_fault.cfsr );
    fault_decode_bfsr( g_fault.cfsr );
    fault_decode_ufsr( g_fault.cfsr );
    fault_puts( "\r\nHFSR =" ); fault_puthex32( g_fault.hfsr );
    fault_decode_hfsr( g_fault.hfsr );
    fault_puts( "\r\nMMFAR=" ); fault_puthex32( g_fault.mmfar );
    fault_puts( "  (MMARVALID=1 时有效)" );
    fault_puts( "\r\nBFAR =" ); fault_puthex32( g_fault.bfar );
    fault_puts( "  (BFARVALID=1 时有效)" );
    fault_puts( "\r\nSHCSR=" ); fault_puthex32( g_fault.shcsr );

    /* 8. 符号化提示 */
    fault_puts( "\r\n>> arm-none-eabi-addr2line -e Debug/f446_rtos.elf -f -C <PC/LR>" );

    /* 9. 等最后一位字节移出移位寄存器(避免复位截尾) */
    {
        uint32_t guard = FAULT_TXE_GUARD;
        while( ( ( FAULT_UART->SR & USART_SR_TC ) == 0U ) && ( --guard != 0U ) ) { }
    }

    /* 10. 处置: 开发期停机等调试器接管。
     * 生产期可改为: 把 g_fault 拷入 .noinit 段 + NVIC_SystemReset() */
    for( ;; )
    {
    }
}
/* USER CODE END 0 */
```

## 7.3 四个异常入口的改写（naked 纯汇编版）

**为什么必须 naked —— 序言对故障帧定位的破坏**：

硬件在进入 Handler 第一条指令之前，就把异常帧压到"故障时活跃的栈"（EXC_RETURN.bit2 决定 PSP/MSP）；而 Handler 本身永远运行在 MSP 上，**C 序言的 push 只作用于 MSP**。因此：

- 任务故障（帧在 PSP）：序言碰不到 PSP，`mrs psp` 永远精确，naked 与否无关紧要；
- **ISR 内故障（帧在 MSP）：普通 C 函数的序言会把 MSP 再压低若干字节**（本工程 -O3 实测为 `stmdb sp!, {r3,r4,r5,r6,r7,r8,r9,lr}` 共 36 字节），此时 `mrs msp` 读到的是"帧基 − 36"，帧解析整体错位 —— 这正是社区流传的非 naked 写法中隐藏的陷阱。

**naked 的实现要点**：

1. naked 属性放在 USER CODE PFP 的**前置声明**上（属性随声明作用于后文定义），CubeMX 生成的函数签名行无需修改；
2. naked 函数体只允许纯 basic asm（禁止带操作数的扩展 asm 和 C 局部变量），因此采集代码写成纯汇编，r0/r1 按 AAPCS 传参，`b fault_report` 尾跳转；
3. `fault_report` 必须加 `__attribute__((used))`：它现在只被汇编字符串引用，GCC 不解析 basic asm，会误判"未使用"而删除符号（链接报 undefined reference）。

```c
/* USER CODE BEGIN PFP */
void HardFault_Handler( void )  __attribute__( ( naked ) );
void MemManage_Handler( void )  __attribute__( ( naked ) );
void BusFault_Handler( void )   __attribute__( ( naked ) );
void UsageFault_Handler( void ) __attribute__( ( naked ) );
/* USER CODE END PFP */
```

```c
void HardFault_Handler(void)
{
  /* USER CODE BEGIN HardFault_IRQn 0 */
  /* naked 纯汇编: 此刻 SP 未被任何软件触碰, MRS 读到的是硬件压栈帧
   * 精确基址; LR 仍是 EXC_RETURN(无 push 改写/无 BL 覆盖)。 */
  __asm volatile
  (
      " tst lr, #4            \n"   /* EXC_RETURN.bit2: 0=MSP, 1=PSP */
      " ite eq                \n"
      " mrseq r0, msp         \n"   /* r0 = 帧基(ISR 内故障场景) */
      " mrsne r0, psp         \n"   /* r0 = 帧基(任务故障场景) */
      " mov   r1, lr          \n"   /* r1 = EXC_RETURN */
      " b     fault_report    \n"   /* 尾跳转, 不返回(AAPCS: r0/r1 传参) */
  );
  /* USER CODE END HardFault_IRQn 0 */
  while (1)                          /* 不可达 */
  {
  }
}
```

MemManage/BusFault/UsageFault 三个入口在各自 USER CODE 区插入同款汇编（仅注释不同）。构建后反汇编验证（应无任何 push）：

```asm
0800358c <HardFault_Handler>:
800358c:  tst.w  lr, #4
8003590:  ite    eq
8003592:  mrseq  r0, MSP
8003596:  mrsne  r0, PSP
800359a:  mov    r1, lr
800359c:  b.w    <fault_report>
```

---

# 第八章 输出示例与符号化

**报告格式**（2026-08 最终版，分段结构、字段对齐、术语标准化）：

```
==================== FAULT REPORT ====================

--- Exception context ---
EXC_RETURN = 0xFFFFFFFD
Mode       : Thread (task code)
Stack frame: PSP, basic frame (8 words)
Frame SP   = 0x20009A88 (valid)

--- Saved registers (exception frame) ---
PC   = 0x0800249C    faulting instruction (reliable on precise faults)
LR   = 0x0800649B    return address (valid only if fault is inside a callee)
R0   = 0x0000000A    R1  = 0x000003E9
R2   = 0x00000000    R3  = 0x22400000
R12  = 0x12121212
xPSR = 0x01000000    IPSR = 0 (Thread)

--- RTOS task ---
Task       : "ft1_bus" (priority 7, id 7)
Task stack : base 0x200096B8, size 0x400 bytes
Stack usage: 0x3D0 bytes in use at fault

--- Fault status registers ---
CFSR = 0x00008200
  BusFaults:
    PRECISERR   precise data bus error, PC and BFAR reliable
    BFARVALID   BFAR holds the faulting address
MMFAR = not valid
BFAR  = 0x20020000 (valid)
SHCSR = 0x00070002

--- Action ---
System halted. Fault context also mirrored in g_fault (debugger access).
Locate source: arm-none-eabi-addr2line -e f446_rtos.elf -f -C <PC>
======================================================
```

**读法（按证据强度排序）**：

1. **BFAR = 0x20020000 (valid)**：被访问的非法地址——正是测试代码要读的地址，实锤；
2. **PC = 0x0800249C**：出错指令。addr2line 指向 `ftask_bus` 中的 `ldr r3,[r4,#0]`（r4=0x20020000），即源码 `v = *(volatile uint32_t *)0x20020000UL;` 那一行——**一步精确定位**；
3. **Task: "ft1_bus"**：肇事任务归因（正确）；
4. **LR = 0x0800649B**：本次为**残留值**（指向 `_puts_r` 内部）——故障在任务顶层函数自身，LR 无效（详见 2.2 节）。**不要用它定位本次故障**。

符号化命令：

```powershell
E:\ST\STM32CubeIDE_1.13.2\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.11.3.rel1.win32_1.1.1.202309131626\tools\bin\arm-none-eabi-addr2line.exe -e Debug\f446_rtos.elf -f -C 0x0800249C
```

加 `-i` 可展开内联。也可直接在 `Debug/f446_rtos.list` 里搜地址。

> 若故障发生在**被调函数内部**（如库函数里的野指针），则 PC=库内出错指令、LR=调用点——此时两个地址都有价值，配合使用可还原"调用点→出错指令"两级现场。
>
> 栈溢出场景的关键行：`Frame SP   = ... (INVALID: outside RAM or misaligned)`（压栈失败时）、`OVERFLOW   : SP is 0x... bytes below stack base`（SP 越过栈底）、`STKERR/MSTKERR ... (stack overflow suspected)`（故障状态佐证）、`CORRUPTION : task stack magic destroyed`（越界写波及 TCB）。
>
> ISR 内故障场景的关键行：`Mode : Handler (interrupt code)`、`IPSR = 54 (Exception 54, IRQ 38)`、RTS 段显示 "current_tcb denotes the interrupted task, not the faulting code"。

---

# 第九章 验证方法：故意触发各类故障

**工程已内置 9 项 UART 命令测试**（main.c 的 ft_* 任务，串口发数字 '1'~'9' 触发，每项触发后停机需复位）：

| 命令 | 触发内容 | 预期关键点 |
|------|---------|-----------|
| 1 | 读 0x20020000 | BFSR.PRECISERR + BFARVALID, BFAR=出错地址, PC=出错指令 |
| 2 | 调用 bit0=0 地址 | UFSR.INVSTATE, 栈帧 xPSR 显示 [T=0 非Thumb状态!] |
| 3 | 100/0 | UFSR.DIVBYZERO（需 CCR.DIV_0_TRP, 已开启） |
| 4 | 递归+破坏 stack_magic+总线故障 | margin 报告 + [stack_magic毁坏] 报警 |
| 5 | 执行 UDF 半字 (.short 0xDE00) | UFSR.UNDEFINSTR, PC 指向该指令 |
| 6 | 内联汇编 ldrd 非对齐地址 | UFSR.UNALIGNED（LDRD 永久禁止非对齐，无需 UNALIGN_TRP） |
| 7 | 写 0x20020000 | BFSR（写缓冲异步 → 常见 IMPRECISERR, PC/BFAR 不可信） |
| 8 | 512B 小栈递归 4×160B 后触发故障 | *** SP已低于栈底: 栈溢出! ***（真实溢出判定路径） |
| 9 | 命令后发任意字节, 在 USART2 ISR 内触发 | ctx:Handler(ISR), 帧在 MSP, IPSR=54(IRQn=38), "错误发生在 ISR 内" |

以下为手工验证的等价代码片段（原理同上，可直接嵌入任意任务）：

```c
/* 测试1: 精确总线错误 → BFSR.PRECISERR + BFARVALID + BFAR=0x20020000
 * (RAM 末尾外一字, 属保留地址区) */
{
    volatile uint32_t *bad = (volatile uint32_t *)0x20020000UL;
    *bad = 0x12345678U;
}

/* 测试2: INVSTATE → 跳转到非 Thumb 地址(把函数地址 bit0 清零)
 * 预期: UFSR.INVSTATE, 栈帧 xPSR.T=0, PC=目标地址 */
{
    typedef void ( *fn_t )( void );
    fn_t f = (fn_t)( ( (uint32_t)some_function ) & ~1UL );
    f();
}

/* 测试3: 除零(需先打开 SCB->CCR |= SCB_CCR_DIV_0_TRP) → UFSR.DIVBYZERO */
{
    volatile int z = 0;
    volatile int r = 100 / z;
    (void)r;
}

/* 测试4: 栈溢出 → 递归爆栈, 预期 BFSR.STKERR 或 SP 越界 + margin 报警 */
{
    volatile char pad[ 512 ];
    pad[ 0 ] = 1;               /* 阻止编译器优化掉递归 */
    recurse_forever();          /* void recurse_forever(void){ volatile char pad[512]; ... } */
}
```

每项预期结论对照第六章指纹表逐一核对 dump 输出。

---

# 第十章 零代码方案与生产期扩展

## 10.1 STM32CubeIDE Fault Analyzer（调试器附加时）

调试运行中 halt（或断点触发）后，IDE 自动弹出 **Fault Analyzer** 视图：自动解码 CFSR/HFSR、显示压栈帧 PC/LR/SP 与 EXC_RETURN。开发期在线调试优先用它；本手册的 Handler 方案解决的是**脱机运行 / 现场偶发 / 无调试器**场景。

## 10.2 生产期黑匣子（方向性设计）

```
fault_report 最后一步(替代 while(1)):
  1. 校验和后把 g_fault 拷入 .noinit 段(链接脚本加一段不被启动清零的 RAM)
  2. 写入魔数 0xFA17FA17 标记"有未上报的故障记录"
  3. NVIC_SystemReset() 复位(或等 IWDG 兜底)
main() 早期:
  if (黑匣子魔数有效) { 经正常日志通道上报记录; 清魔数; }
```

要点：`.noinit` 在软复位后保留（掉电丢失；掉电保留需用 F446 的 VBAT 备份 SRAM）；复位前务必等 `USART_SR_TC`（7.2 节第 9 步已含）；可配合 IWDG 保证"必然复位"。

---

*基准工程：f446_rtos | 参考架构：ARMv7-M Reference Manual (DDI0403) §B3/B4、ARM/Feabhas Generic HardFault Handler、ST 社区 "How to debug a HardFault on STM32"*
