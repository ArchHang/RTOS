# 测试入口与文件管理

本目录保留当前测试夹具、构建模板、执行脚本和独立验收器。按任务选择入口，
不要把旧批次目录名当成当前源码版本。API 文档与示例检查见
[ai_docs/README.md](ai_docs/README.md)，架构说明见
[rtos_design.md](../docs/rtos_design.md)。

测试结论和归档范围按日期记录。本次交付所用验证入口、覆盖和结果见
[工程交付报告](../docs/logs/2026-11-11.md#hardening-release)，长时间压力与加速回绕的
结果见 [F446 长时并发与回绕回归](../docs/logs/2026-11-11.md#long-soak)。
本批保留的 [实测证据与复跑入口](evidence/2026-11-11/README.md) 集中记录 F446、H743 及宿主结果。

## 1. 目录与职责

```text
hil/
├── README.md                         本入口
├── build_fw.ps1 / build_layout.ps1    变体构建及日常构建布局
├── run_hil.ps1                       SWD 下载、串口采集及结束条件检查
├── repro_all.ps1                     常用回归组合入口
├── run_scenarios.ps1 / repeat_run.ps1 场景及重复运行辅助入口
├── restore_project.ps1               恢复日常构建规则，可选择重建
├── run_memory_host.py                内存边界、小堆配置与 LTO 可见性宿主验证
├── run_usb_device_host.py            USB 资源回滚、协议入口及会话宿主验证
├── run_uart_pipeline.py              真实串口业务流与独立模型结果核对
├── fw/                              权威测试源码，必须保留
├── templates/                       权威 make 模板，必须保留
├── h743/                            Cortex-M7 隔离 BSP、构建与受保护烧录/恢复
├── evidence/2026-11-11/              必要原始文字证据、身份清单与离线验收器
├── refactor_20261027/                当前布局的矩阵、编译与验收脚本
├── basepri_20261025/                 保留原字节的基础日志验收器
├── adversarial_20261025/             保留原字节的复杂并发日志验收器
└── ai_docs/                         API 契约检查、示例编译及摘录核对
```

`basepri_20261025/verify_evidence.py` 和
`adversarial_20261025/verify_evidence.py` 仍被当前验收器按固定摘要导入，
因此保留原路径、原字节；这两个目录不再保存旧工程、旧运行脚本或旧结果。
当前矩阵入口集中在 [refactor_20261027](refactor_20261027/README.md)。

运行时生成 `hil/logs/`、`hil/fwsrc/` 和工程根目录的 `Debug/`。
`hil/fw/` 保存测试源码，`hil/fwsrc/` 保存构建时复制的变体文件。
`hil/logs/` 不入库，保留的 ELF 统一以 `images/<标签>/firmware.elf` 为准；本轮清除了两个
批次根目录的相同 ELF 副本。旧的一次性归档脚本仅记录当时汇编证据的过程，复跑采用本文的
现行构建/采集入口与 `evidence/` 中的验收器。

## 2. 选择入口

| 需求 | 入口 | 是否需要板卡 |
|---|---|---|
| 核对 API 文档、索引与本地链接 | `ai_docs/check_contracts.py` | 否 |
| 编译独立 C 示例和支持实现 | `ai_docs/compile_examples.py` | 否，需 ARM GCC |
| 核对源码重组的结构与历史基线 | `refactor_20261027/verify_layout.py` | 否，需保留 Git 基线 |
| 16 个 RTOS C 单元的配置编译矩阵 | `refactor_20261027/compile_checks.ps1` | 否，需 ARM GCC |
| 常用功能、端口和复杂并发组合 | `repro_all.ps1` | 是 |
| 复杂等待、继承、销毁窗口和容量矩阵 | `refactor_20261027/run_matrix.ps1` | 是 |
| BASEPRI、阻塞返回、IRQ 优先级边界 | `refactor_20261027/run_port_matrix.ps1` | 是 |
| 功能、生产负载、负向对照和长时间运行 | `refactor_20261027/run_regressions.ps1` | 是 |
| 单个变体构建 | `build_fw.ps1` | 否，需 ARM GCC 与 make |
| 单个镜像下载与串口采集 | `run_hil.ps1` | 是 |
| 恢复日常构建规则 | `restore_project.ps1` | 否；重建需工具链，重新下载需板卡 |
| STM32H743 Cortex-M7 实测、Flash 备份与恢复 | [h743](h743/README.md) | 是，ST-LINK V3 与 USART1 VCP |
| 堆、固定池边界与 LTO 统计可见性 | [内存宿主复跑](#host-memory) | 否，需宿主 GCC |
| 通用堆的时延、连续容量和检查点 | [F446 堆夹具](#f446-heap-latency) | 是 |
| USB 复合设备启动回滚、CDC/HID 回调与 MSC 介质边界 | `python -B hil/run_usb_device_host.py --opt O2` | 否，需本机 GCC |

历史验收器按当时的字段和原始批次工作。当前实板复测应选择表中相应入口，使用新的
输出目录，并保留源码身份、构建、固件、下载和原始日志。

<a id="host-memory"></a>
### 内存宿主复跑

`run_memory_host.py` 对生产分配器运行边界夹具和对齐检查。`--heap-configurations` 另编译
10 种 8～41 字节的小堆配置，检查未初始化、24 字节门槛、向下对齐、精确耗尽和重复初始化。
Windows 上使用 `--lto-poll visible` 运行 O3+LTO 观察回归。负对照可用 `--source-root`
指定仍包含旧版堆实现的源码树，并将期望设为 `stale`。仅当写方确认释放、读方超时才算
复现成功。

```powershell
python -B hil/run_memory_host.py --opt O3 --heap-configurations --lto-poll visible
```

随机内存账本使用 [宿主模型复跑](evidence/2026-11-11/replay/README.md)。宿主验证用
端口桩，真实 TIM6 中断见 [H743 夹具](h743/README.md)。

## 3. 执行环境与示例命令

从工程根目录运行 Windows PowerShell。脚本当前配置为 NUCLEO-F446RE、ST-LINK SWD、
COM3、115200 / 8N1，工具路径指向本机 STM32CubeIDE 1.13.2。
`build_fw.ps1`、`restore_project.ps1` 从自身路径定位工程；部分辅助脚本仍含
`E:\STM32_Project\CubeIDE\f446_rtos` 固定路径。换电脑前先检查编译器、make、
CubeProgrammer、项目路径和串口配置，不能只修改命令中的串口参数。

以下命令从工程根目录执行。

```powershell
# 常用全部组合：audit、port、adversarial、bench、boundary、selftest、scenario。
powershell -NoProfile -ExecutionPolicy Bypass -File hil/repro_all.ps1 -Tag rerun_YYYYMMDD

# 仅功能与契约边界。
powershell -NoProfile -ExecutionPolicy Bypass -File hil/repro_all.ps1 `
    -Only selftest,boundary -Tag rerun_YYYYMMDD

# 单独构建、下载并采集生产压力变体。
powershell -NoProfile -ExecutionPolicy Bypass -File hil/build_fw.ps1 `
    -Variant production_stress -StressSeed 3735928559 `
    -StressSeconds 900 -StressQueueCapacity 8
powershell -NoProfile -ExecutionPolicy Bypass -File hil/run_hil.ps1 `
    -Elf Debug/f446_rtos.elf -Tag production_01 -Seconds 950 `
    -Terminator 'STRESS\|DONE\|' -OutDir hil/logs/rerun_YYYYMMDD

# 回归多 IPC 在 Tick 0 的超时、交付与资源清理。
powershell -NoProfile -ExecutionPolicy Bypass -File hil/build_fw.ps1 `
    -Variant wrap_ipc_regression -Optimization O3 -CriticalMask primask -DisableAssertions
powershell -NoProfile -ExecutionPolicy Bypass -File hil/run_hil.ps1 `
    -Elf Debug/f446_rtos.elf -Tag wrap_ipc_01 -Seconds 20 `
    -Terminator 'WRAP\|DONE\|pass=16\|fail=0' -OutDir hil/logs/rerun_YYYYMMDD

# 恢复规则并重建日常固件；该命令本身不会将日常固件重新烧录。
powershell -NoProfile -ExecutionPolicy Bypass -File hil/restore_project.ps1 -Rebuild
powershell -NoProfile -ExecutionPolicy Bypass -File hil/run_hil.ps1 `
    -Elf Debug/f446_rtos.elf -Tag daily_01 -Seconds 15 `
    -OutDir hil/logs/rerun_YYYYMMDD
```

每次运行使用新的批次目录和 Tag。部分底层辅助脚本允许覆盖同名文件，不能依赖所有
入口都会阻止覆盖；执行者应先确认目标目录未使用。需要可严格复核的矩阵批次时，
按 [矩阵方法](refactor_20261027/README.md)保存全部输入与输出。

## 4. 夹具与固件变体

`build_fw.ps1` 通过预处理宏和额外编译单元选择变体，不要求手工替换生产源码。
它将 `fw/Src/` 中所需夹具复制到 `fwsrc/`，头文件使用 `fw/Inc/`。
`Core/Src/selftest.c`、`Core/Src/benchmark.c` 仍是现有工程中的测试入口。

| 变体 | 主要覆盖 | 结束标记 |
|---|---|---|
| `daily` | 日常应用、故障演示和 USB | 按应用输出判读 |
| `selftest` | 功能回归；真实 USART 中断需上位机回发 `X` | `ST\|SUMMARY` |
| `boundary` | API 参数、上下文及边界契约 | `BT\|SUMMARY` |
| `portable_fixture` | 用 F446 的审计任务运行一个共享 Cortex-M 定向夹具 | `F4HIL\|READY` 后按夹具结束标记验收 |
| `audit` | IPC、内存、定时器、USB 包装及端口回归 | `AT\|DONE` |
| `wrap_ipc_regression` | 12 个并发等待者跨过 Tick 0，真实 SysTick 与 NVIC EXTI1 验证摘链、交付和迟到通知 | `WRAP\|DONE\|pass=16\|fail=0` |
| `usb_osal_priority_regression` | 让步期间保留基础优先级，持锁继承和解锁回退 | `UPR\|DONE\|pass=9\|fail=0` |
| `usb_cdc_cursor_regression` | 512/768 字节接收环的逻辑计数回绕、物理折返、满载有序和清空。使用 `-CdcRxCapacity 512` 或 `768` | `CDCWR\|DONE\|pass=4\|fail=0` |
| `robustness_msp` | 首任务 MSP、单次真实 PendSV 与 32 轮阻塞恢复 | `MSP\|DONE` |
| `robustness` | 任务栈、屏蔽、协作调度、堆/固定池及服务 | `RB\|DONE` |
| `wake_bench` | 常规 BM0–BM7 加 1000 次 PendSV 前信号量 give 周期 | `BM\|DONE` |
| `mutex_release_bench` | 1 或 8 把已注册锁时的空等待最终释放；另验递归、其他锁继承和真实交接 | `MRB\|DONE` |
| `tick_timeout_bench` | 0 或 8 把无关注册锁、1/4/8/16 个信号量或队列等待者同 tick 超时 | `TTB\|DONE` |
| `sem_wake_bench` | 真实等待者的优先级/FIFO 交付、观测位图篡改对照与超时清位；计时到 PendSV 前 | `SWB\|DONE` |
| `task_registry_bench` | 1、8、16、32 个登记任务的通知，覆盖哈希碰撞、非法地址与同址重建 | `TRB\|DONE\|pass=28\|fail=0` |
| `systick_empty_bench` | 真实 SysTick 无到期的空链和未来截止链首，各采 128 次调度器周期 | `STB\|DONE\|pass=2\|fail=0` |
| `timer_dispatch_bench` | 命令排空到永久或绝对截止等待入口，验证回调时刻及屏蔽状态 | `TDB\|DONE` |
| `perf_snapshot_bench` | 单条及 1/8/16 条任务快照，验证容量、字段和屏蔽恢复；可加 `-PerfHotpath` | `PSB\|DONE` |
| `pool_free_bench` | 36/64 字节块、0/31 个空闲节点的归还计时及非法地址、重复归还检查 | `PFB\|DONE` |
| `startup_clock` | 100 Hz 下 SysTick 重载边界、失败状态保持和可重试启动 | `STARTUP\|DONE` |
| `fault_diagnostics` | 真机 UDF，Thread/PSP、FPU 扩展或 Handler/MSP | 54 个等号组成的结束行 |
| `uart_pipeline` | USART2 真实收发、遥测变换和 2044 字节 CNN 推理 | 主机按请求 ID、CRC、数值与计数器判定 |
| `queue_copy` | 单字节与非对齐五字节、队列回绕及两侧真实阻塞 | `QCOPY\|DONE` |
| `bench` / `perf` | 微基准及性能观察 | `BM\|DONE` |
| `lowlevel` / `flash_diag` | 上下文/FPU 保存、Flash 缓存诊断 | `BM\|DONE` |
| `basepri` / `basepri_blocking` | 精确屏蔽交错、真实等待返回值 | `BP\|DONE` / `BLOCK\|DONE` |
| `basepri_guard` / `basepri_nesting` | IRQ 契约、断言及非法嵌套 | `BP\|DONE` / `NEST\|DONE` |
| `adversarial_mutex` | 锁依赖链、优先级继承与撤销 | `AM\|DONE` |
| `adversarial_wait` | 多等待者、通知、事件、超时与 ISR 交错 | `AW\|DONE` |
| `adversarial_lifetime` | 对象检查窗口内的合法中断销毁 | `OF\|DONE` |
| `adversarial_mutex_boundary` | 递归溢出、持锁者删除、有限循环依赖 | `AMB\|DONE` |
| `adversarial_registry` | 最大互斥图容量及多锁删除 | `AR\|DONE` |
| `production_stress` | 16 个业务 worker、共享 IPC、三层 IRQ | `STRESS\|DONE` |
| `scenario` / `scenario_usb` / `diag` | 场景、保留 USB 的场景及夹具诊断 | `SC\|DONE` |

具体宏、预期检查数及完整结束格式以当前构建脚本、夹具和验收器为准。
USB 控制请求替身、寄存器观察和生产函数调用不能证明物理 USB 总线互操作；
销毁窗口测试使用仍可访问的对象存储，不能证明任意旧指针或提前释放内存安全。
有限循环依赖通过真实超时解除，不表示内核实现了死锁检测。

<a id="f446-portable-fixtures"></a>
### 在 F446 上复跑共享夹具

`portable_fixture` 使用现有审计任务启动指定入口。`-FixtureDefines` 和 `-FixtureWrap` 使用
逗号分隔，前者传入测试宏，后者指定链接包装符号。`-UseVectorWrappers` 选择汇编向量包装，
`-LinkTimeOptimization` 对共享夹具及 RTOS C 单元启用 LTO，裸汇编移植层单独编译。
烧录前核对 ST-LINK 与 MCU 身份并保留完整备份。

```powershell
pwsh -NoProfile -File hil/build_fw.ps1 -Variant portable_fixture `
    -FixtureSource hil/h743/h743_heap_poll_regression.c -FixtureEntry f446_heap_poll_regression_run `
    -FixturePriority 4 -Optimization O3 -LinkTimeOptimization
pwsh -NoProfile -File hil/run_hil.ps1 -Elf Debug/f446_rtos.elf -Tag heap_lto `
    -Probe '<ST-LINK serial>' -Terminator 'F4MP\|DONE\|pass=4\|fail=0' -Seconds 20
```

其他共享夹具入口及测试宏见 [H743 分模块复测](h743/README.md#h743-functional-reproduction)。
F446 使用 `production_timer_target.h` 验证 APB1 与 TIM 输入频率。

MSC 资源回归使用真实 F446 任务与 USB OSAL 对象池，通过链接包装控制器启停。成功结束行
为 `UMSC|DONE|pass=24|fail=0`。

```powershell
pwsh -NoProfile -File hil/build_fw.ps1 -Variant portable_fixture `
    -FixtureSource hil/fw/Src/f446_usb_msc_resource.c -FixtureEntry f446_usb_msc_resource_run `
    -FixturePriority 4 -FixtureDefines HIL_PORTABLE_BEFORE_START=f446_usb_msc_resource_prepare `
    -FixtureWrap 'usb_dc_init,usb_dc_deinit'
```

[F446 实测矩阵](../docs/rtos_testing_report.md#f446-functional-retest)

<a id="f446-heap-latency"></a>
### F446 通用堆时延与容量

`f446_heap_latency_run` 在审计任务内测量线性堆的时延并检查容量和数据完整性。对照不同
源码快照时，保持夹具、板卡、优化级、中断屏蔽、断言及堆配置一致，分别保存构建参数、
ELF、映射和串口记录。

在隔离工程副本中执行以下命令。烧录前按共享夹具流程确认板卡身份并保存原固件，运行结束
恢复原固件。将 ST-LINK 序列号、串口和批次目录替换为本次实际值。

```powershell
pwsh -NoProfile -File hil/build_fw.ps1 -Variant portable_fixture -Optimization O2 `
    -FixtureSource hil/fw/Src/f446_heap_latency.c -FixtureEntry f446_heap_latency_run `
    -FixturePriority 4
pwsh -NoProfile -File hil/run_hil.ps1 -Elf Debug/f446_rtos.elf -Tag heap_first_fit_o2 `
    -Probe '<ST-LINK serial>' -Port COM3 -OutDir hil/logs/new_heap_batch -Seconds 20 `
    -Terminator '(?m)^HLAT\|DONE\|'
```

从 `F4HIL|READY|entry=f446_heap_latency_run|priority=4` 起核对六项 `HLAT|CASE`。

| 判据 | 核对内容 |
|---|---|
| `setup` | DWT 计数器推进，任务入口屏蔽状态正常，初始最大连续块可申请并归还 |
| `few` | 4 个 96 字节块形成 2 个小孔，160 字节请求取自护栏后的空闲段 |
| `fragmented` | 256 个 32 字节块形成 128 个小孔，1024 字节请求取自护栏后的空闲段 |
| `task` | 4 组任务大小的 TCB/栈块反复替换，栈请求轮换 128、192、256、320 字，存活块哨兵完整 |
| `wide_bucket` | 物理 128、152 字节块之间保留活块，尾部耗尽。145 字节请求被拒绝且账目不变，144 字节请求命中大孔，余下最大块为 120 字节。释放中间活块后可申请 304 字节 |
| `recovery` | 最大可分配容量及活跃分配数恢复到入口值，最大块实分配和归还成功，屏蔽状态恢复 |

`few` 和 `fragmented` 的 `HLAT|LAYOUT` 须为 `verified=1`。基本配置有八条
`HLAT|STAT`。少块和碎片用例各两条，TCB 和栈各有分配、归还两条。每条要求
`n=256`、`invalid=0`、`0 <= tick_crossings <= n`、`min <= mean <= max`，
以及 `mean == sum / n` 的整数商。
每种负载另有 8 次预热。DWT 时间段只包围堆 API，保留正常中断处理，`tick_crossings`
报告采样附近跨 Tick 的次数。周期极值随采样条件变化。

不相邻空闲段的容量用例在三组负载完成计时后执行。验收值为
`HLAT|BUCKET|layout=1|holes_max=144|rejected=1|deep_fit=1|remaining_max=120|joined_max=304|joined_fit=1|guards=1`。
`HLAT|RECOVERY` 中 `exact_probe=1`，三个最大块字段及三个活跃分配数字段分别一致。
相邻空闲块可能保留额外块头，`initial_free` 和 `restored_free` 用于观察该账目变化。
基本配置的成功结束行为 `HLAT|DONE|status=0|checks=6|failures=0`。

在构建命令中加入 `-FixtureDefines 'HEAP_LATENCY_SIZE_CASES=1'`，可测量
等长块复用与大块拆分。使用新 Tag 采集。两项尺寸用例都从一个 32 字节小孔、一个活护栏
和一段连续大尾块开始，在计时外核对申请落点、拆分账目、护栏及归还后的状态。

| 新增判据 | `HLAT|SIZE` 中的 `request` | `allocated_max` | 核对内容 |
|---|---:|---:|---|
| `exact32` | 32 | 等于 `tail_max` | 每次申请命中指定小孔，尾块容量保持 |
| `split4096` | 4096 | 等于 `tail_max - 4104` | 每次申请取自护栏后的尾块，拆分计入 8 字节块头，归还恢复尾块 |

两条 `HLAT|SIZE` 都应满足 `hole_size=32`、`layout=1`、`probe=1`、
`layout_restored=1` 和 `heap_restored=1`。每项尺寸用例各增加一条 `alloc` 和一条
`free` 的 `HLAT|STAT`，每条仍采 256 个样本并沿用上述统计判据。开启时应有十二条
`HLAT|STAT`、八项 `HLAT|CASE` 通过，以及 `HLAT|DONE|status=0|checks=8|failures=0`。

<a id="f446-heap-hybrid"></a>
历史自适应堆的工作负载与原始结果见[实测报告](../docs/rtos_performance_report.md#f446-heap-hybrid)。

<a id="f446-heap-failure"></a>
#### 线性堆分配失败与恢复

```powershell
pwsh -NoProfile -File hil/build_fw.ps1 -Variant portable_fixture -Optimization O2 `
    -FixtureSource hil/fw/Src/f446_heap_failure.c -FixtureEntry f446_heap_failure_run `
    -FixturePriority 4
pwsh -NoProfile -File hil/run_hil.ps1 -Elf Debug/f446_rtos.elf -Tag heap_failure `
    -Probe '<ST-LINK serial>' -Port COM3 -OutDir hil/logs/new_heap_batch -Seconds 30 `
    -Terminator '(?m)^HFL\|DONE\|'
```

夹具先耗尽堆，再释放 128 个由活块分隔的空闲段。最大载荷为 48 字节，56 和 80 字节
申请均应失败，失败前后的空闲载荷与计数应一致。验证较小请求命中低地址首块，释放隔离块
后 80 字节申请恢复，并检查活块哨兵和最终容量。

从 `HFL|STAT` 读取耗尽失败、首次碎片失败、紧接的首次最大块查询（`max_after_miss`）、
重复同尺寸和较大尺寸失败，以及重复失败后的最大块查询（`max_after_query`）。
重复失败两组各 32 次，其余各一次。DWT 计时只包围堆 API。验收五个
`HFL|CASE` 通过及 `HFL|DONE|status=0|checks=5|failures=0`。

<a id="f446-heap-checkpoint-mixed"></a>
#### 线性堆检查点混合负载

```powershell
pwsh -NoProfile -File hil/build_fw.ps1 -Variant portable_fixture -Optimization O2 `
    -FixtureSource hil/fw/Src/f446_heap_checkpoint_mixed.c `
    -FixtureEntry f446_heap_checkpoint_mixed_run -FixturePriority 4
pwsh -NoProfile -File hil/run_hil.ps1 -Elf Debug/f446_rtos.elf -Tag heap_checkpoint_mixed `
    -Probe '<ST-LINK serial>' -Port COM3 -OutDir hil/logs/new_heap_batch -Seconds 45 `
    -Terminator '(?m)^HCPM\|DONE\|'
```

少块负载 128 轮，交错负载 192 轮，后者维持 24 个常驻分配并
轮换请求尺寸。每轮安排一次较低地址释放，令相应检查点失效。统计全量申请和释放，
另记录失效释放及失效后首次申请。独立有序模型逐次核对返回地址、空闲载荷、最大连续
载荷、分配释放计数与在用数据。计时区间只包围堆 API。
验收标记为 `HCPM|DONE|status=0|checks=3|failures=0`。

<a id="f446-heap-boundary"></a>
#### 当前堆的边界和所有权压力

两个夹具均通过 `portable_fixture` 选择，入口优先级为 4。使用独立构建目录，并按
本节的构建、采集和恢复流程操作。F446 实测配置与原始证据见
[边界验证报告](../docs/rtos_testing_report.md#f446-heap-boundary-regression)。

| 夹具 | 入口 | 验收 |
|---|---|---|
| [f446_heap_boundary.c](fw/Src/f446_heap_boundary.c) | `f446_heap_boundary_run` | 七组边界全部 PASS，结束行为 `HBN\|DONE\|status=0\|checks=7\|failures=0` |
| [f446_heap_irq_stress.c](fw/Src/f446_heap_irq_stress.c) | `f446_heap_irq_stress_run` | 四个来源接纳数与接收数逐项相等且均大于零，结束行为 `HBIRQ\|DONE\|status=0\|failure=0` |

边界夹具覆盖请求上下限和对齐边界、92 种普通尺寸、实际容量附近逐字节请求、24 B 拆分门槛、
最小块耗尽、碎片失败与容量恢复。独立物理块模型检查账目，受损块头在修复后重试。
失败、坏头中止、零容量缓存和无效释放还组合两层临界区与既有严格屏蔽值验证。

压力夹具包含两个任务生产者、一个消费者、一个耗尽压力任务。TIM2 以 1 kHz 产生
合法 IRQ，每八次通过 NVIC 置 pending 触发更高优先级的合法 EXTI4 嵌套 IRQ。数据载荷为 8..513 B，
实际包申请包含 16 B 头，因此为 24..529 B。成功发送转交指针所有权，接收者逐字节
检查后归还。发送失败由发送者归还。另有 33 B 临界区探针和最大连续块压力申请。

默认运行 20 秒，`HIRQ_SECONDS` 可配置为 5..120。前两秒消费者即时处理，压力任务
等待，以覆盖各来源的成功路径。随后开启消费背压与连续块占用，覆盖队列满、申请失败、
中断嵌套和释放。运行前 TIM2 与 EXTI4 应空闲，TIM2 使用本工程 168 MHz 板级时钟配置。
采集超时可设为 `HIRQ_SECONDS + 15` 秒。采集须从本轮 `F4HIL|READY` 起核对完整
结束状态、分源账本、阶段及恢复容量。

[复跑与证据核对](../docs/logs/2026-11-13.md#heap-boundary)

<a id="f446-heap-weighted"></a>
#### 业务频次与平均堆操作成本

[f446_heap_weighted.c](fw/Src/f446_heap_weighted.c) 的入口为
`f446_heap_weighted_run`，使用 `portable_fixture` 和优先级 4。常规流量、
突发积压、突发诊断各运行 32 轮，每轮 96 个变长报文。长期对象初始化、全部有效
归还、失败申请和重试分别计时，结束时所有业务对象均已释放。

主统计 `HWB|STAT` 的 `alloc_ok/alloc_fail/free/query` 相互排斥，
`HWB|SUBSTAT` 与 `HWB|RETRY` 表达其中的重试子集。周期总和由
`sum_hi/sum_lo` 两个 32 位字段还原为 64 位值。总体平均用所有主分类的
原始周期总和除以实际调用次数。窗口外校准和验证调用数量由 TOTAL 显式输出。

每场景成功申请和释放各 3,120 次。常规负载失败与查询均为零，突发负载失败
1,247 次，诊断负载失败 787 次、查询 2,034 次。还须核对三个场景的 trace hash、
全部类的 invalid=0、容量恢复和 `HWB|DONE|status=0|checks=3|failures=0`。
构建后一次采集仅代表该次实际 ELF，推荐采集超时 90 秒。

[实际加权公式与数据](../docs/rtos_performance_report.md#f446-heap-weighted) ·
[编译、复跑和证据校验](../docs/logs/2026-11-13.md#heap-weighted)

<a id="f446-heap-equal-protection"></a>
#### 等保护和堆状态消融

用 `portable_fixture`、优先级 4、`-O2` 和 BASEPRI 构建
[f446_heap_compare.c](fw/Src/f446_heap_compare.c)，入口为 `f446_heap_compare_run`。
传入 `-FixtureWrap _sbrk`。默认使用等保护，`HCB_PROTECTED=0` 用于空锁对照，
`HCB_LIBC_FIRST=1` 交换回放顺序，`HCB_FAILURE_ONLY=1` 独立运行失败轨迹。
多条定义以逗号分隔。默认应有六组成功轨迹，失败镜像应有两组，均检查
`HCB|DONE|status=0` 及 `failures=0`。等保护镜像还须有 `HCB|MASK|checked=1|valid=1`。

[f446_heap_state_probe.c](fw/Src/f446_heap_state_probe.c) 的入口为
`f446_heap_state_probe_run`，无需链接包装，结束行为
`HSP|DONE|status=0|checks=5|failures=0`。逐项消融用
[prepare_heap_ablation.py](prepare_heap_ablation.py) 从当前生产文件生成构建输入，
空闲链表使用 [候选实现](fw/variants/rtos_heap_free_list.c)。两者均在独立工程副本
中替换堆实现。空闲链表与消融成功和失败对照可用 `HCB_RTOS_ONLY=1`。

[实测结果](../docs/rtos_performance_report.md#f446-heap-equal-protection) ·
[每日日志](../docs/logs/2026-11-13.md#heap-equal-protection)

<a id="f446-heap-industry"></a>
#### 不同分配器的业务回放

[f446_heap_industry.c](fw/Src/f446_heap_industry.c) 在 F446 的独立 32 KiB
试验区重放固定帧、常规遥测、积压遥测、会话换新和隔离孔恢复。
入口为 `f446_heap_industry_run`。本地批次脚本将后端适配器编译进隔离构建树。
解析器按同一次启动的 `HIB|START` 和 `HIB|DONE` 取数据，核对完成数量、
输入哈希及容量恢复。固定块池只跑固定帧。回压后端可触发不同次数的重试，
对比业务成本时以每项完成工作所需的全部计时调用为分子。

[F446 结果](../docs/rtos_performance_report.md#f446-heap-industry) ·
[历史批次说明](../docs/logs/2026-11-13.md#heap-industry) ·
[当前 RTOS 堆与 Zephyr 的复跑入口](heap_study/README.md)

<a id="f446-heap-timer-latency"></a>
#### 堆首次失败与真实 TIM5 响应

[f446_heap_irq_latency.c](fw/Src/f446_heap_irq_latency.c) 的入口为
`f446_heap_irq_latency_run`。测试镜像提供独立 32 KiB 的 `industry_heap_*` 后端。
相位扫描使用 TIM5 CCR1、P6 和 P4，结束行是
`HILIRQ|DONE|status=0|checks=12|failures=0`。定向布局把 128 个小孔隔离，
每次冷样本前撤销上一轮缓存。使用独立插桩镜像测量屏蔽内部窗口时，
只在测试后端局部锁进出调用 DWT 探针，并定义 `HEAP_IRQ_INSTRUMENTED=1`。

用原业务夹具加 `HEAP_IRQ_BACKGROUND=1` 和同一计时器代码，可统计完整业务
及其准备、收尾期间的 P6 延迟和跳过的比较槽。原始和插桩样本分别统计。
[配置与测量结果](../docs/logs/2026-11-13.md#heap-timer-latency) ·
[复跑入口](heap_study/README.md) · [UART 原始证据](evidence/2026-11-13/README.md)

<a id="f446-interaction-fixtures"></a>
### F446 跨功能与 IRQ 交错夹具

以下六个夹具均通过 `portable_fixture` 运行。每次只选择一个构建命令，立即烧录、采集并
核对本轮启动后的结束行，再构建下一项；不能连续构建后把最后的ELF当成全部用例。
源码中的软件置 pending 触发真实NVIC异常，不要求外接EXTI引脚信号。除栈钩子夹具使用
EXTI3外，其余使用EXTI2，运行前必须未被应用占用。

| 夹具源码 | 场景与验收 |
|---|---|
| [f446_timer_command_order.c](fw/Src/f446_timer_command_order.c) | 回调真实阻塞期间，IRQ先投递START/STOP并填满16槽命令队列，回调随后同步STOP/START；核对FIFO、周期自停/重启、满队列拒绝及屏蔽恢复。默认16轮：`F4TI\|DONE\|status=0\|checks=227\|failures=0\|rounds=16\|stop_calls=16\|start_calls=32`。开启DELETE扩展后为234项，并须有 `F4TI\|DELETE\|calls=1\|command_status=-1`，其余结束计数相同 |
| [f446_timer_irq_burst.c](fw/Src/f446_timer_irq_burst.c) | 32/96/128个活跃定时器，各8轮IRQ突发及一个真实阻塞发送者。每档须有irq=8、accepted=136、full=8、task_sends=8；核对发送者确已阻塞及后续命令已消费。结束为 `F4TB\|DONE\|status=0\|checks=33\|failures=0\|clock_hz=168000000` |
| [f446_reap_irq_pressure.c](fw/Src/f446_reap_irq_pressure.c) | 动态任务退出积压、回收释放窗口被IRQ唤醒的高优任务打断，高优任务再次分配大栈；核对创建/删除成功及堆最大块、活跃分配数恢复。结束为 `REAPIRQ\|DONE\|pass=3\|fail=0`；created数量受配置影响，不硬编码为固定36个 |
| [f446_cooperative_yield_irq.c](fw/Src/f446_cooperative_yield_irq.c) | 协作调度中显式yield已选好候选、PendSV尚未提交时，IRQ恢复更高优任务或挂起已选任务；核对真实交错窗口及首个实际执行任务。结束为 `COY\|DONE\|pass=2\|fail=0`，两项CASE均须window=1、irq=1且first=expected |
| [f446_stack_hook_irq.c](fw/Src/f446_stack_hook_irq.c) | Idle报告栈哨兵损坏时，IRQ唤醒监督任务，删除并同址重建另一待报告任务；核对不向新代次投递旧报告、重建后的哨兵完好及资源回收。结束为 `SGIR\|DONE\|pass=1\|fail=0`，RESULT须stale=0、irq=1、new_guard=1、clean=1 |
| [usb_rx_session_regression.c](fw/Src/usb_rx_session_regression.c) | CDC/HID多个读取者等待时，由真实IRQ注入RESET、断开、重配置并立即到新数据，以及检查连接后到登记等待前的会话切换；旧读取全部取消且不能偷取新会话数据。结束为 `URX\|DONE\|status=0\|failures=0\|setup=1`，四项CASE均pass=1，新数据阶段fresh=2、stolen=0 |

构建参数如下；入口、优先级、宏和链接包装应成组使用。下面定时器顺序命令包含DELETE
扩展；移除其 `-FixtureDefines` 参数即为默认227项。`F446_TIMER_ORDER_ROUNDS` 可配置为
1～256，修改后检查总数随轮数变化，不能继续套用16轮的结束计数。

```powershell
# 定时器跨IRQ/回调顺序，含DELETE后同步命令返回值。
pwsh -NoProfile -File hil/build_fw.ps1 -Variant portable_fixture -Optimization O2 `
    -FixtureSource hil/fw/Src/f446_timer_command_order.c `
    -FixtureEntry f446_timer_command_order_run -FixturePriority 4 `
    -FixtureDefines F446_TIMER_ORDER_TEST_DELETE=1
```

```powershell
# 定时器IRQ突发、等待发送者和屏蔽片段；测试钩子只在此镜像启用。
pwsh -NoProfile -File hil/build_fw.ps1 -Variant portable_fixture -Optimization O2 `
    -FixtureSource hil/fw/Src/f446_timer_irq_burst.c `
    -FixtureEntry f446_timer_irq_burst_run -FixturePriority 4 `
    -FixtureDefines RTOS_HIL_CRITICAL_TEST=1
```

```powershell
# 回收窗口的IRQ与再次动态创建。
pwsh -NoProfile -File hil/build_fw.ps1 -Variant portable_fixture -Optimization O2 `
    -FixtureSource hil/fw/Src/f446_reap_irq_pressure.c `
    -FixtureEntry f446_reap_irq_pressure_run -FixturePriority 9 -FixtureWrap rtos_heap_free
```

```powershell
# 协作yield候选与PendSV提交间的IRQ。
pwsh -NoProfile -File hil/build_fw.ps1 -Variant portable_fixture -Optimization O2 `
    -FixtureSource hil/fw/Src/f446_cooperative_yield_irq.c `
    -FixtureEntry f446_cooperative_yield_irq_run -FixturePriority 4 `
    -FixtureDefines 'RTOS_CONFIG_USE_PREEMPTION=0,RTOS_CONFIG_USE_TIME_SLICING=0' `
    -FixtureWrap rtos_port_context_switch
```

```powershell
# Idle栈报告与同址重建；需要现有配置的栈检查模式2、性能登记和抢占调度。
pwsh -NoProfile -File hil/build_fw.ps1 -Variant portable_fixture -Optimization O2 `
    -FixtureSource hil/fw/Src/f446_stack_hook_irq.c `
    -FixtureEntry f446_stack_hook_irq_run -FixturePriority 4 -FixtureWrap rtos_stack_overflow_hook
```

```powershell
# USB会话代次/等待登记竞态；初始化钩子须在启动调度前运行。
pwsh -NoProfile -File hil/build_fw.ps1 -Variant portable_fixture -Optimization O2 `
    -FixtureSource hil/fw/Src/usb_rx_session_regression.c -FixtureEntry usb_rx_session_run `
    -FixturePriority 4 `
    -FixtureDefines 'HIL_PORTABLE_BEFORE_START=usb_rx_session_prepare,RTOS_HIL_OBJECT_LIFETIME=1' `
    -FixtureWrap 'usbd_initialize,usbd_ep_start_read'
```

每次构建后，采集命令按实际夹具替换Tag、结束前缀和新批次路径。例如第一项的DELETE
扩展可这样执行；采集到DONE后仍须核对上表全部字段，而非仅确认串口有输出。

```powershell
pwsh -NoProfile -File hil/run_hil.ps1 -Elf Debug/f446_rtos.elf `
    -Tag timer_order_delete_O2 -OutDir hil/logs/new_interaction_batch -Port COM3 -Seconds 20 `
    -Terminator '(?m)^F4TI\|DONE\|'
```

顺序、回收及USB夹具还可用 `-Optimization O3 -CriticalMask primask -DisableAssertions`
复测另一配置；协作夹具可用 `-Optimization O0 -UseVectorWrappers` 检查未优化异常路径。
这些组合须分别重编译及烧录，不能由O2结果推定。栈钩子夹具会故意破坏静态任务的哨兵，
由测试专用链接包装接管报告并清理；不向真实业务栈写越界数据，也不等价于MPU隔离验证。

定时器burst的 `max_mask_cycles` 是DWT观察到的服务任务外层临界区最大片段，包含测试
钩子开销；计时区无printf。它受活跃链长度、编译配置、主频及实际抢占影响，代表本轮
样本最大值，不是全部负载的WCET承诺。命令批次不持有一个整批临界区，各条START及改
周期的有序插链仍为O(N)。普通控制路径无测试钩子时的时间须另行隔离测量。
`timer_dispatch_bench` 变体自动包装 `rtos_queue_recv`、`rtos_internal_queue_recv_until`
和 `rtos_queue_get_count`，同时兼容旧版空接收探针与当前有界前缀路径；验收为两种等待
模式各64样本、invalid=0及 `TDB|DONE|pass=6|fail=0`，时间字段单位为DWT周期。

USB会话夹具包装控制器初始化和端点读提交，在板上运行真实RTOS及IRQ并注入USB协议事件；
它不需要也不证明物理USB线枚举、主机驱动或真实总线数据传输。串口采集若在本轮
`System init done` / `F4HIL|READY` 之前混入上次镜像的输出，必须按启动边界区分并保留
原始日志，不能仅凭全文件FAIL关键字或删除旧行来判定本轮结果。

### 4.1 新增定向回归

```powershell
pwsh -NoProfile -File hil/build_fw.ps1 -Variant robustness_msp -Optimization O0 -UseVectorWrappers
pwsh -NoProfile -File hil/run_hil.ps1 -Elf Debug/f446_rtos.elf -Tag msp_wrapped `
    -Terminator 'MSP\|DONE' -Seconds 15 -OutDir hil/logs/new_batch
pwsh -NoProfile -File hil/build_fw.ps1 -Variant robustness -Cooperative -PerfHotpath
pwsh -NoProfile -File hil/run_hil.ps1 -Elf Debug/f446_rtos.elf -Tag targeted `
    -Terminator 'RB\|DONE' -Seconds 15 -OutDir hil/logs/new_batch
python -B hil/run_memory_host.py --opt O2
```

省略 `-UseVectorWrappers` 则使用 F446 默认向量直连。带该选项只改变 SVC/PendSV
向量路由，仍运行真实异常。旧 C 包装可导致目标板停在故障处理，超时和故障现场
应保留。修复版须有结束行、首任务 MSP 8 字节对齐、单次 PendSV 及 32 轮延时前后
的 MSP 相同。可选用 `-CriticalMask primask -DisableAssertions -Optimization O3`
验证状态型等待在另一屏蔽实现中也能拒绝非法阻塞。

宿主内存入口复用板上内存夹具，另用 GCC 的对齐插桩独立验证 64 位指针 ABI。
用 `--source-root` 指向提取的旧源码，可以复查同一用例的失败。USB 的定向 audit
使用板上 IRQ 和传输提交替身注入失败；默认业务压力另用生产夹具。
定向用例的实测范围见[每日记录](../docs/logs/2026-11-11.md#hil-full-validation)。

### 4.2 优化与故障诊断

```powershell
pwsh -NoProfile -File hil/build_fw.ps1 -Variant wake_bench -Optimization O3
pwsh -NoProfile -File hil/run_hil.ps1 -Elf Debug/f446_rtos.elf -Tag wake_o3 `
    -Terminator 'BM\|DONE' -Seconds 15 -OutDir hil/logs/new_batch
pwsh -NoProfile -File hil/build_fw.ps1 -Variant startup_clock -Optimization O2
pwsh -NoProfile -File hil/run_hil.ps1 -Elf Debug/f446_rtos.elf -Tag startup_o2 `
    -Terminator 'STARTUP\|DONE' -Seconds 15 -OutDir hil/logs/new_batch
pwsh -NoProfile -File hil/build_fw.ps1 -Variant fault_diagnostics -FaultContext fpu
pwsh -NoProfile -File hil/run_hil.ps1 -Elf Debug/f446_rtos.elf -Tag fault_fpu `
    -Terminator '(?m)^={54}\r?$' -Seconds 15 -OutDir hil/logs/new_batch
python -B hil/check_fault_probe.py hil/logs/new_batch/log_fault_fpu.txt
```

时钟夹具使用 100 Hz 是为了让 32 位输入能触及 24 位 LOAD 上界。故障夹具故意让
处理器进入 UsageFault；PRIMASK 变体会升级为 HardFault，解析器需加 `--primask`。
收齐结束行只是完成采集，解析器还核对异常来源、屏蔽值和栈用量公式。完成故障镜像
测试后需复位或重新下载，再运行正常应用。

### 4.3 串口遥测与 CNN 项目

应用由 USART2 ISR、解析任务、单一模型任务和发送任务组成。STM32 端由
[uart_pipeline.c](fw/Src/uart_pipeline.c) 实现，PC 端使用
[run_uart_pipeline.py](run_uart_pipeline.py)（Python 3.9+、pyserial）。
在指定波特率下用 ST-LINK 烧录并验证 ELF，待启动完成后再开主机测试。以下示例
使用 `Debug/f446_rtos.elf`，若实际下载了其他副本，`--elf` 应指向该副本。

```powershell
pwsh -NoProfile -File hil/build_fw.ps1 -Variant uart_pipeline -UartBaud 921600
New-Item -ItemType Directory -Path hil/logs -Force
gcc -shared -O2 -ffp-contract=off -fno-fast-math AI/output/int8/ai_app.c `
    AI/output/int8/ai_param.c AI/output/int8/conv.c -lm -o hil/logs/uart_golden.dll
# 用 STM32CubeProgrammer 经 SWD 下载、校验并复位 Debug/f446_rtos.elf。
python -B hil/run_uart_pipeline.py --port COM3 --baud 921600 --mode inference `
    --model hil/logs/uart_golden.dll --elf Debug/f446_rtos.elf --windows 1,4 --requests 36 `
    --verify-header-recovery --output-dir hil/logs/new_batch
```

每帧依次为同步字节 `A5 5A`、操作码、LE16 数据长度、LE32 请求 ID、负载及
LE16 CRC16-CCITT-FALSE。CRC 从操作码覆盖到负载末尾。操作 1 接受 0–64 字节，
返回求和与 FNV-1a。操作 2 接受 2044 字节，返回类别和四个 binary32 logits。
操作 3 查询状态。状态前 12 个 LE32 字依次为接收字节、接收 ISR 周期低位、ISR 峰值、
UART 错误、队列丢弃、CRC 拒绝、其他拒绝、遥测完成、推理完成、模型周期低位、
解析周期低位、发送失败，再附接收 ISR、模型、解析三个周期计数的高 32 位。

应用用四个输入槽为模型做背压。合法请求若拿不到槽，返回 `0xE0` 与错误码 2。
CRC 错误及结构不合法的头丢弃并计数，错误头的请求 ID 未受 CRC 校验，所以不回应。
主机正常测量时，推理窗口取 1 或 4，遥测窗口取 1 或 8。生产串口控制协议若要处理
合法长度范围内的任意破坏、重传或重复 ID，须另立相应帧和去重约定。

`-UartModelOptimization O3` 只控制本变体的模型编译，`-Optimization` 控制内核与
应用。测量保留原始二进制串口、逐请求延迟和状态差量。本项目使用真实 USART2 和
CNN 算子，输入为确定性向量，不能据此推断麦克风链路或真实语音识别准确率。

### 4.4 采集、租约监督与共享总线恢复

三个项目分别构建，不修改日常 `main.c`，不依赖外接 ADC 或总线设备。

| 变体 / 夹具 | 项目与验收 | 串口结束行 |
|---|---|---|
| `adc_capture` / [project_adc_capture.c](fw/Src/project_adc_capture.c) | TIM2 以 1 kHz 触发真实 ADC1 VREFINT，DMA2 完成中断通知采集任务，固定块经队列交给处理任务；核对 24 块、384 个样本、序号和归还数，另测三种控制块重叠 | `ADC\|DONE\|pass=2\|fail=0` |
| `timer_supervisor` / [project_timer_supervisor.c](fw/Src/project_timer_supervisor.c) | 板载 LD2 模拟受租约保护的输出；在服务任务接收入口放行高优任务，用实际调度和 tick 跨过截止期；另核对续约及周期回调自停 | `SUP\|DONE\|pass=23\|fail=0` |
| `bus_recovery` / [project_bus_recovery.c](fw/Src/project_bus_recovery.c) | 任务对模拟设备寄存器争用，检查递归释放、正常交接、待确认时撤销和同址重建；记录每个请求的返回值和实际所有权 | `BUSREC\|DONE\|pass=3\|fail=0` |

例如，在工程根目录运行一轮。若改用 O0 或 O3，给日志标签加上相应配置，避免覆盖。

```powershell
$cases = [ordered]@{ adc_capture = 'ADC'; timer_supervisor = 'SUP'; bus_recovery = 'BUSREC' }
foreach ($name in $cases.Keys) {
    pwsh -NoProfile -File hil/build_fw.ps1 -Variant $name -Optimization O2
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $name" }
    pwsh -NoProfile -File hil/run_hil.ps1 -Elf Debug/f446_rtos.elf -Tag "${name}_O2" `
        -OutDir hil/logs/project_run -Port COM3 -Seconds 10 -Terminator "$($cases[$name])\|DONE"
    if ($LASTEXITCODE -ne 0) { throw "HIL failed: $name" }
}
pwsh -NoProfile -File hil/restore_project.ps1 -Rebuild
# 若要让板上也回到日常应用，通过 SWD 下载并验证重建的 Debug/f446_rtos.elf。
```

定时器夹具的链接包装只参与本变体。它在精确位置放行高优任务，实际抢占和
GPIO 操作仍由 F446 执行。ADC 样本只检查量级、完整性与内存所有权，不做模拟精度校准。
总线夹具不发送真实 I²C、SPI 帧。原始内核的预期失败及复验结果见
[项目验证记录](../docs/logs/2026-11-11.md#project-robustness)。

## 5. 判读与恢复要求

热点基准以 `-Variant mutex_release_bench`、`tick_timeout_bench` 或
`pool_free_bench` 单独构建。分别下载基线和候选，保持相同夹具、优化级及屏蔽配置；
Tick 夹具只包装真实 SysTick 中的调度器调用，计时不含异常入口、PendSV 和任务恢复。
结束前核对各组样本量以及终止行的 `fail=0`。对照方法和当前读数见
[热点优化实测](../docs/rtos_performance_report.md#10-互斥释放与批量超时的隔离实测)。
信号量、定时器和性能快照的隔离口径及数据见
[后续热点复测](../docs/rtos_performance_report.md#11-就绪位图信号量定时器与性能快照)。

- `run_hil.ps1` 先打开串口，再烧录、复位并采集，保存原始串口、下载日志、ELF 副本及摘要。
  指定结束行缺失或出现失败断言会报错；需要负向对照时才使用 `-AllowFailures`，并核对
  预期失败原因及非零结果，不能把预期失败计作正常通过。
- 生产压力当前输出 `STRESS|HIGHIRQ_ACCOUNT`。同一次 TIM2 ISR 可以同时观察到自然
  更新和软件触发，也可能两者都没观察到；验收要求
  `entries + coincident == natural + forced_observed + spurious`，并另核对软件请求
  是否全部收到、线程屏蔽值和外设恢复。空入口计数不能独立证明有无自然更新丢失。
  历史冻结验收器使用旧字段，不作为新日志的完整验收入口。
- 自检需要真实中断握手；组合入口已配置 `ST\|ISR\|ARM` 后回发 `X`。手动采集时应显式
  配置 `-ReplyOn 'ST\|ISR\|ARM' -Reply X`，避免遗漏 USART IRQ 覆盖。
- `make` 不跟踪宏变化，不能跨变体混用旧目标。Windows 的 `make clean` 也可能因没有
  `rm` 而留下目标；应确认实际重编译命令、对象集合、源码身份及有效配置。
- 比较性能必须使用相同对象集合、频率、配置、编译优化和采样口径。`BM` 与场景周期值
  不能脱离主频换算；旧场景中的 `us_per_call` 标签不能直接解释为单次调用耗时。
- 结束后恢复日常规则、重建、重新下载并检查启动与任务进展。只恢复文件或只生成 ELF
  不等于板上已经恢复日常固件。
- 缺少 ELF、原始日志、下载记录或身份清单时，报告证据不足。离线解析、实际编译、
  寄存器观察和真实硬件执行分别报告，有限测试不能证明不存在任何 BUG。

## 6. 文件保留规则

长期维护 `fw/`、`templates/`、当前脚本和入口文档。运行产物写入独立批次，整理前先
决定要保留的完整证据，不改写旧日志或摘要来适配新源码。过期批次可以整批清退，
同时撤下相应的“可完整重验”声明和失效链接，不保留大量旧内核、旧工程或失败方案副本。

AI 示例和文档校验的输出可写入本机 `hil/logs/<批次>/`，生成方法见
[AI 校验说明](ai_docs/README.md)。叙述性结果及清理记录按日期维护，见
[工程文档导航](../docs/README.md#daily-logs)。
