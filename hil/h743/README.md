# STM32H743 实板验证

本目录用 H743 的启动代码、时钟和 USART1 构建隔离测试镜像。RTOS 默认取本工程 `RTOS/`，
`--rtos-root` 可以指定另一份源码做同板对照。链接目标是 Cortex-M7、FPv5-D16 hard-float。

## 板级输入

`build.py` 默认使用同级 `h743_microarch_lab`，也可用 `--bsp-root` 指向同布局的历史 BSP。
已归档的 H743 实板证据来自该历史配置。它使用 BSP 的 `bsp/startup.s`、`bsp/system_stm32h7xx.c`、
`bsp/clock.c`、`bsp/usart.c`、`src/syscalls.c`，以及 `include/` 和 `vendor/` 下的 CMSIS/HAL。

只有本仓库和公开 SDK 时，使用 `--cubeh7-root` 指向 STM32CubeH7 根目录。可从 v1.11.2
取得 CMSIS Core，并初始化两个驱动子模块。仓库内的 `cubeh7/` 提供 25 MHz HSE、LDO、
USART1 PA9/PA10 及 newlib 堆的 HIL 胶水。CubeH7 提供启动模板和驱动。
[ST 官方 STM32CubeH7 v1.11.2](https://github.com/STMicroelectronics/STM32CubeH7/tree/v1.11.2)

```powershell
git clone --depth 1 --branch v1.11.2 https://github.com/STMicroelectronics/STM32CubeH7.git ../STM32CubeH7
git -C ../STM32CubeH7 submodule update --init --depth 1 `
    Drivers/CMSIS/Device/ST/STM32H7xx Drivers/STM32H7xx_HAL_Driver
python -B hil/h743/build.py --cubeh7-root ../STM32CubeH7 `
    --out-dir hil/logs/h743_public_build --opt O2 --data-memory axi
```

公开 SDK 路径使用本地 HAL 1.11.1 按官方目录布局完成了构建、链接和向量检查。
实板比较使用归档所记录的历史 BSP、编译器和配置。

| 入口 | 验证内容 | 结束行 |
|---|---|---|
| `h743_bench_run` | I/D Cache 几何、调度器字段跨线数、AXI 集中/分散和预热/定向失效、DTCM 对照 | `CACHE\|DONE\|pass=7\|fail=0` |
| `h743_compat_run` | O0 向量包装、SVC/PendSV MSP、浮点上下文、Tick 和实际 NVIC 嵌套 | `H7C\|DONE\|pass=16\|fail=0` |
| `h743_dma_cache_run` | 32 次 DMA2 传输，真实 IRQ 解除 RTOS 阻塞，检查 AXI 数据与邻接缓存行 | `H7DMA\|DONE\|pass=6\|fail=0` |
| `production_stress_task` | 16 个任务、真实 TIM2/TIM6/TIM7、过载及恢复、独立消息和屏蔽对账 | `STRESS\|DONE\|status=0` |
| `h743_lifecycle_stress_run` | 混合 IPC 等待、关闭重建、阻塞任务删除、动态堆耗尽与恢复、TIM2/EXTI4 中断与队列账本 | `H7L\|DONE\|status=0` |
| `h743_boundary_stress_run` | 任务优先级与超时边界、真实 EXTI2 填满定时器命令队列、旧代次命令隔离 | `H7B\|DONE\|status=0\|` |
| `h743_resume_regression_run` | EXTI2 在 PendSV 前恢复较高优先级任务，核对候选和实际执行顺序 | `H7RESUME\|DONE\|pass=4\|fail=0` |
| `h743_reap_regression_run` | Idle 饥饿下动态任务自删，TCB 和栈的实际耗尽及失败回滚 | `H7REAP\|DONE\|pass=8\|fail=0` |
| `h743_idle_fifo_regression_run` | 三个最低优先级应用任务 FIFO、队头自删和单个应用任务跨 SysTick | `H7FIFO\|DONE\|status=0\|` |

通用信号量、任务登记、Tick、批量到期和 IPC 回绕夹具从 `hil/fw/Src/` 引用。后面三种 Tick
夹具使用 `--wrap rtos_sched_tick` 链接到真实 SysTick 调度入口。性能观测由板级 DWT 初始化提供。

## 执行

使用 GNU Arm GCC、STM32CubeProgrammer、Python 3.9+ 和 `pyserial`。测试板的 H743 Rev V
工作于 480 MHz，USART1 走 PA9/PA10，VCP 默认 COM4、921600 8N1。
通过 `STM32_Programmer_CLI -l` 核对探针与串口。使用独立输出目录，首轮烧录前做两次完整备份，
写入和恢复前核对 MCU UID 与芯片修订。测试镜像的链接上限是 Flash 首扇区 128 KiB，
恢复后读回全部 2 MiB 做字节比较。旧清单供证据查阅，重新运行需生成当前板的备份。

```powershell
$batch = 'hil/logs/h743_rerun_YYYYMMDD'
$probe = '<ST-LINK serial>'
$bsp = @()  # 公开 SDK 配置使用 @('--cubeh7-root', '../STM32CubeH7')
python -B hil/h743/run_board.py --batch $batch --probe $probe backup
if ($LASTEXITCODE -ne 0) { throw 'Board backup failed' }
python -B hil/h743/build.py @bsp --out-dir "$batch/images/cache" --opt O2 --data-memory axi
if ($LASTEXITCODE -ne 0) { throw 'H743 build failed' }
try {
    python -B hil/h743/run_board.py --batch $batch --probe $probe capture `
        --image "$batch/images/cache/firmware.bin" --entry h743_bench_run --tag cache `
        --terminator '^CACHE\|DONE\|pass=7\|fail=0$'
    if ($LASTEXITCODE -ne 0) { throw 'H743 cache fixture failed' }
} finally {
    python -B hil/h743/run_board.py --batch $batch --probe $probe restore
    if ($LASTEXITCODE -ne 0) { throw 'Original flash verification failed' }
}
```

`--data-memory axi|dtcm` 选择内核常驻数据和任务栈。MSP 始终在 DTCM，DMA 专用缓冲始终
在 AXI。`--icache on|off` 和 `--dcache on|off` 分别选择 L1 状态。`--vector wrapper`
验证无 C 栈帧的入口跳转，直接向量是默认配置。`--critical-mask primask --disable-assertions`
可测试另一套屏蔽配置。`--compiler` 和 `--cli` 用于指定本机工具链。

`--entry` 将通过／失败判定限定在指定测试入口的 `H743|READY` 之后，完整串口仍保存在
`uart.bin` 与 `uart.txt`，判定窗口另存 `current_uart.txt`。重复的同名 READY 会判为失败。
若连续烧录同名入口时 SWD 连接阶段重新启动了前一镜像，可交替烧录不同入口后重新采集。
`python -B hil/h743/test_run_board.py` 可在宿主复测写入前身份核对和串口判定。

<a id="h743-stress-reproduction"></a>
## 调度和生命周期压力复现

使用 `--entry-source hil/h743/<夹具>.c --entry-name <入口>` 选择表中入口。生命周期与恢复回归
的入口优先级为 4，其他三个定向夹具使用 9。恢复回归还需要 `--wrap rtos_port_context_switch`，
包装函数只为夹具制造可重复的 IRQ/PendSV 相位。关闭时间片使用
`--define RTOS_CONFIG_USE_TIME_SLICING=0`，合作式配置使用 `--define RTOS_CONFIG_USE_PREEMPTION=0`。

```powershell
python -B hil/h743/build.py @bsp --out-dir "$batch/images/lifecycle" --opt O2 `
    --entry-source hil/h743/h743_lifecycle_stress.c --entry-name h743_lifecycle_stress_run `
    --entry-priority 4 --define H743_LIFECYCLE_SECONDS=60U --define H743_LIFECYCLE_SEED=3735928559U

python -B hil/h743/build.py @bsp --out-dir "$batch/images/production" --opt O2 `
    --entry-source hil/fw/Src/production_stress.c --entry-name production_stress_task --entry-priority 4 `
    --extra-source hil/fw/Src/production_irq.c --extra-source hil/fw/Src/production_highirq.c `
    --define FIRMWARE_PRODUCTION_STRESS=1 --define PRODUCTION_STRESS_SECONDS=180U `
    --define PRODUCTION_STRESS_SEED=3735928559U --define PRODUCTION_STRESS_QUEUE_CAPACITY=8U
```

基线使用 `--rtos-root` 指向 `c11450f` 提交的 `RTOS/`。同一夹具和配置下，基线自删回归预期
出现 `created=40`、`idle_switches=0`，IRQ 恢复回归预期 `low_first=32`，关闭时间片的
边界回归预期在 `lowest_priority_forever` 失败。检查完整负对照后再运行修复版。
边界夹具可用 `--define H743_BOUNDARY_ROUNDS=128U` 扩展队列满载、发送超时和代次重建轮数。

<a id="h743-functional-reproduction"></a>
## 分模块边界复测

除下表另有说明外，入口优先级为 9。`hil/fw/Src/` 的多个夹具原入口都叫
`audit_regressions_task`。连续采集时可用 `--define audit_regressions_task=<本次入口>` 重命名，
并将同名入口传给构建的 `--entry-name` 和采集的 `--entry`。

| 夹具 | 关键构建参数 | 结束行 |
|---|---|---|
| `hil/fw/Src/boundary_test.c` | 入口 `boundary_test_task` | `BT\|SUMMARY\|pass=103 fail=0` |
| `hil/fw/Src/startup_clock_regression.c` | `--define H743_TEST_BEFORE_START=audit_pre_start --define RTOS_CONFIG_TICK_RATE_HZ=100U --opt O0 --vector wrapper` | `STARTUP\|DONE\|pass=11\|fail=0` |
| `hil/fw/Src/robustness_runner.c` | `--usb-osal --define RTOS_CONFIG_PERF_HOTPATH_STATS=1`，并以 `--extra-source` 加入 `robustness_core.c`、`robustness_memory.c`、`robustness_services.c` | `RB\|DONE` |
| `hil/fw/Src/adversarial_mutex.c`、`adversarial_mutex_boundary.c`、`adversarial_registry.c` | 分别构建，可混合 O2/BASEPRI 和 O3/PRIMASK | `AM\|DONE`、`AMB\|DONE`、`AR\|DONE` |
| `hil/fw/Src/adversarial_wait.c` | `--extra-source hil/fw/Src/production_highirq.c --define FIRMWARE_ADVERSARIAL_WAIT=1 --define FIRMWARE_AUDIT=1` | `AW\|DONE` |
| `hil/fw/Src/adversarial_lifetime.c` | `--define FIRMWARE_ADVERSARIAL_LIFETIME=1 --define RTOS_HIL_OBJECT_LIFETIME=1` | `OF\|DONE` |
| `hil/h743/h743_priority_noop_regression.c` | 入口 `h743_priority_noop_regression_run`，优先级 4，`--define RTOS_CONFIG_USE_TIME_SLICING=0` | `H7PRIO\|DONE\|status=0\|ready=1\|running=1` |
| `hil/h743/h743_service_edges.c` | 入口 `h743_service_edges_run`，优先级 4，`--define RTOS_CONFIG_PERF_HOTPATH_STATS=1` | `H7S\|DONE\|status=0\|checks=12\|failures=0` |
| `hil/h743/h743_heap_poll_regression.c` | 入口 `h743_heap_poll_regression_run`，优先级 4，`--opt O3 --lto` | `H7MP\|DONE\|pass=4\|fail=0` |

`--usb-osal` 链接实际 RTOS USB 对象池与 IPC 适配，不启用 DWC2 外设。`--lto` 让测试与堆
代码进行跨编译单元优化，含裸汇编的生产 `port.c` 单独编译。启动测试的非法时钟检查在
READY 之前完成，验收时同时查阅完整 `uart.txt`。USB 复合设备协议和资源耗尽见
[宿主测试入口](../README.md)。本轮数据见[分模块边界结果](../../docs/rtos_testing_report.md#h743-functional-boundaries)。

板测范围和 A/B 周期见 [H743 实测](../../docs/logs/2026-11-11.md#h743-board) 与
[性能报告](../../docs/rtos_performance_report.md#h743-cache-performance)。最新边界结果见
[H743 健壮性回归](../../docs/rtos_testing_report.md#h743-robustness)。
