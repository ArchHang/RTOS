# 测试入口与文件管理

本目录保留当前测试夹具、构建模板、执行脚本和独立验收器。按任务选择入口，
不要把旧批次目录名当成当前源码版本。API 文档与示例检查见
[ai_docs/README.md](ai_docs/README.md)，架构说明见
[rtos_design.md](../docs/rtos_design.md)。

测试结论和归档范围按日期记录。本次交付所用验证入口、覆盖和结果见
[工程交付报告](../docs/logs/2026-11-11.md#delivery-report)，长时间压力与加速回绕的
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
├── fw/                              权威测试源码，必须保留
├── templates/                       权威 make 模板，必须保留
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

历史验收器按当时的字段和原始批次工作。当前实板复测应选择表中相应入口，使用新的
输出目录，并保留源码身份、构建、固件、下载和原始日志。

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
