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
        --image "$batch/images/cache/firmware.bin" --tag cache `
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

`python -B hil/h743/test_run_board.py` 可在宿主复测写入前身份核对，不连接板卡。

板测范围和 A/B 周期见 [H743 实测](../../docs/logs/2026-11-11.md#h743-board) 与
[性能报告](../../docs/rtos_performance_report.md#h743-cache-performance)。
