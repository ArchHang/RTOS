# F446 堆业务轨迹和 TIM5 对照

在 PowerShell 7 中从工程根目录执行。构建脚本复用 `hil/build_fw.ps1` 的 GNU Tools for STM32 11.3 和 Make 默认路径。若 CubeIDE 安装在其他位置，传入 `-ToolchainDirectory`（含 `arm-none-eabi-gcc.exe` 的目录）和 `-MakeExecutable`（`make.exe` 的完整路径）。每个 `-Tag` 建立独立构建目录，重复执行时使用新标签。

```powershell
pwsh -NoProfile -File hil/heap_study/build.ps1 -Backend current -Phase industry -Tag current_industry
pwsh -NoProfile -File hil/heap_study/build.ps1 -Backend zephyr -Phase irq -Tag zephyr_irq
pwsh -NoProfile -File hil/heap_study/build.ps1 -Backend current -Phase irq_direct -Tag current_irq_direct
pwsh -NoProfile -File hil/heap_study/build.ps1 -Backend zephyr -Phase periodic -Tag zephyr_periodic
pwsh -NoProfile -File hil/heap_study/build.ps1 -Backend current -Phase periodic_direct -Tag current_periodic_direct
```

| `-Phase` | 负载 | 串口完成标志 |
|---|---|---|
| `industry` | 固定大小、遥测、突发、会话和碎片业务轨迹 | `HIB\|DONE` |
| `irq` | TIM5 单次比较中断，包含空闲、成功、失败、释放，以及屏蔽阈值两侧的优先级 | `HILIRQ\|DONE` |
| `irq_direct` | 与 `irq` 相同，并在堆本地临界区边界插桩 | `HILIRQ\|DONE` |
| `periodic` | 同一组业务轨迹运行期间触发周期 TIM5 中断 | `HIB\|DONE` |
| `periodic_direct` | 与 `periodic` 相同，并在堆本地临界区边界插桩 | `HIB\|DONE` |

每个阶段均支持两个后端，使用 `-O2`、BASEPRI、32 KiB 测试区和 8 字节载荷对齐。`current` 将生产堆源码以独立符号再编译一次，每条轨迹重新初始化专用测试区。`zephyr` 使用 Zephyr v4.2.0 `sys_heap`，通过 `sys_heap_aligned_alloc` 请求 8 字节对齐，并在分配和释放时使用同一套 RTOS 本地中断屏蔽原语。Zephyr 兼容配置开启运行统计和常规断言，保留三次候选探测，关闭监听器和 MSAN。单次和周期阶段的 `*_direct` 测量含探针开销，记录中断响应时使用对应的无插桩镜像。

Zephyr 构建自动从 v4.2.0 标签下载 [heap.c](https://raw.githubusercontent.com/zephyrproject-rtos/zephyr/v4.2.0/lib/heap/heap.c)、[heap.h](https://raw.githubusercontent.com/zephyrproject-rtos/zephyr/v4.2.0/lib/heap/heap.h) 和 [Apache-2.0 许可证](https://raw.githubusercontent.com/zephyrproject-rtos/zephyr/v4.2.0/LICENSE)。离线构建可传入 `-ZephyrSource`，该目录需直接包含这三个文件。两种路径均在编译前检查 `build.ps1` 固定的 SHA-256。

ELF、MAP、构建日志和源码身份记录位于 `hil/logs/heap_study/<tag>/` 下。选用现有 HIL 入口刷写并采集，`-Port` 使用板子实际串口。同时连接多块 ST-LINK 时，传入 `-Probe` 选择目标。`run_hil.ps1` 沿用工程既有的 CubeProgrammer CLI 默认路径。若 CubeIDE 安装在其他位置，可用本机 CLI 刷写并使用自己的串口采集器。

```powershell
pwsh -NoProfile -File hil/run_hil.ps1 `
    -Elf hil/logs/heap_study/zephyr_irq/project/Debug/f446_rtos.elf `
    -Tag zephyr_irq -Port COM3 -Seconds 60 `
    -Terminator '(?m)^HILIRQ\|DONE\|status=0\|checks=12\|failures=0\r?$' `
    -OutDir hil/logs/heap_study/zephyr_irq/capture
```

业务和周期中断阶段使用 `(?m)^HIB\|DONE\|status=0\|checks=5\|failures=0\r?$`。周期中断阶段还会为每条业务轨迹输出 `HILIRQ|BG`，其中 `lost` 是比较事件超期计数，`mask_n` 和 `mask_max` 来自插桩镜像。`source_identity.json` 记录所编译的测试源码、堆、适配层和 ELF 的校验值。

本次构建验证覆盖 `current/industry`、`zephyr/irq_direct`（离线源）和 `current/periodic_direct`。三个 ELF 均含两个独立的 32 KiB 堆，两个中断镜像的 TIM5 向量均指向夹具的中断处理函数。
