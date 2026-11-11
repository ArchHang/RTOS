# 2026-11-11 RTOS 实测归档

首批归档保存 115 份 F446/H743 串口采集和 6 份宿主模型日志。后续追加的调度、分模块和
跨功能验证按下方独立章节索引，分别使用各自验收器。原样保留正常结果、两项 USB
旧版复现、损坏包负对照以及 H743 首次启动故障。`manifest.json` 记录原始字节、采集
结束条件和内核源码身份。板测原始 ELF/BIN、SWD 记录与用户原固件备份留在本机
`hil/logs/`。仓库中的 H743 恢复记录只公开 2 MiB 读回比较的结果。

| 路径 | 内容 |
|---|---|
| `raw/f446-soak/` | 900/180 秒压力、不同容量、旧版 USB 复现、修复复验、IPC 回绕及真实 ADC/DMA |
| `raw/f446-architecture/` | O0/O2/O3 性能对照、合入压力、上下文及 IPC 回归 |
| `raw/h743/` | 480 MHz 性能、I/D Cache、AXI/DTCM、DMA2、移植、两轮压力及初次失败 |
| `raw/host/` | 堆和固定池随机模型、等待链排序和位图模型 |

## 离线核验

从仓库根目录执行以下命令。第一条验证采集字节、预期失败、压力账本、缓存/端口配置和
关键周期值。第二条还核对两个提交中各 64 个 RTOS 文件。浅克隆需要先获取相关提交。

```powershell
python -B hil/evidence/2026-11-11/verify.py
python -B hil/evidence/2026-11-11/verify.py --check-sources
```

本机仍持有原始 Flash 备份时，可传 `--h743-backup-dir hil/logs/h743_perf_20261111_1803`
核对两份测试前读取和恢复后读回。此选项仅读取文件。历史备份清单缺少现行写入工具要求的
MCU UID，应保留作证据。重新烧录前对当前板卡创建新的备份。

## 重新构建和上板

- 性能基线取 `cee655b`，即两项 USB 修复已合入、架构优化未合入。`e267a03` 是本轮
  性能内核。完整提交值与精确源码摘要见 [manifest.json](manifest.json)。使用当前夹具和
  两份内核分别构建，保持优化级、屏蔽方式、断言、缓存和链接区一致。
- F446 使用 [HIL 入口](../../README.md) 的 `build_fw.ps1` 和 `run_hil.ps1`。可在单独
  工作树中以 `git restore --source cee655b -- RTOS` 替换测试内核。两边使用
  `sem_wake_bench`、`task_registry_bench`、`systick_empty_bench` 和 `tick_timeout_bench`
  夹具。当前版可再执行 `run_regressions.ps1 -Suite soak -SoakSeconds 900`。
- USB 原版复现可在另一独立工作树用 `git restore --source e83ad70 -- RTOS/USB`，再应用
  [CDC 旧版测试钩子](cdc-baseline-test-hook.patch)。运行 `usb_osal_priority_regression`
  和 `usb_cdc_cursor_regression -CdcRxCapacity 768`，原版分别预期 2 项和 1 项失败。
  修复版用同一变体应全部通过。CDC 使用实际收包回调和端点提交替身。
- H743 使用 [构建和烧录说明](../../h743/README.md)。可用 `--rtos-root` 指向基线的
  `RTOS/`。实际采集采用历史 BSP，公开 STM32CubeH7 路线另行完成了编译、链接及向量
  检查。采集时记录 `H743|BOOT`、`H743|LAYOUT` 和 `H743|READY`，以核对时钟、缓存和
  内存放置。Flash 恢复后读回整片验证。
- [宿主模型复跑](replay/README.md) 包含堆、固定池和有序等待链的夹具、编译开关与逐种子校验。

实时 IRQ 相位、编译器、地址布局和采样区间会影响周期及消息数。复跑时使用夹具的完整性、
超时、屏蔽和结束条件验收。离线核验仅针对归档记录和源码身份，不替代板测。

[设计手册](../../../docs/rtos_design.md) · [测试报告](../../../docs/rtos_testing_report.md#release-validation) ·
[性能报告](../../../docs/rtos_performance_report.md#architecture-hotpaths) · [当日记录](../../../docs/logs/2026-11-11.md#h743-board)

<a id="h743-robustness-archive"></a>
## H743 调度与生命周期补充归档

[raw/h743-robustness](raw/h743-robustness/) 包含 16 份串口记录、编译配置和 Flash 恢复摘要，
其中三份是冻结版 `c11450f` 的预期失败。先后保留两次 180 秒真实三路中断压力、两次
生命周期长测和调度边界回归。除第一次压力基线外，串口记录从指定夹具的 READY 开始。
含烧录交接和无效采集的完整 UART、构建、SWD 和读回记录在本机
`hil/logs/h743_robust_20261111_2014/`。

```powershell
python -B hil/evidence/2026-11-11/verify_h743_robustness.py
```

离线复算三项缺陷的负对照、FIFO、定时器队列耗尽、三路 IRQ 与消息账本、IPC 生命周期
和堆恢复结果。实板配置与限制见 [测试报告](../../../docs/rtos_testing_report.md#h743-robustness)。

<a id="h743-functional-archive"></a>
## H743 分模块验证与 USB 宿主证据

[raw/h743-functional](raw/h743-functional/) 保存优先级 FIFO、性能换算、真实 TIM6 下
LTO 堆统计的修复前后结果，以及通用边界、互斥、IPC、启动、浮点现场和组合压力采集。
同目录也保存 USB 宿主负对照、三种优化级和只读配置的原始输出。各构建参数见 `runs.json`。
完整串口、镜像、恢复和反汇编留在本机 `hil/logs/`，路径见[当日记录](../../../docs/logs/2026-11-11.md#h743-functional-run)。

```powershell
python -B hil/evidence/2026-11-11/verify_h743_functional.py
```

[功能与边界矩阵](../../../docs/rtos_testing_report.md#h743-functional-boundaries) ·
[构建和烧录说明](../../h743/README.md#h743-functional-reproduction)

<a id="f446-retest-archive"></a>
## F446 对照复测归档

[raw/f446-retest](raw/f446-retest/) 保存 38 份板测串口文本，含七份旧版负对照，另有
USART2 推理交易和结果摘要。原始 Flash、完整 SWD 日志、ELF、构建记录、UART 二进制
流留在本机 `hil/logs/f446_retest_20261111_2126/`。

```powershell
python -B hil/evidence/2026-11-11/verify_f446_retest.py
```

[F446 测试矩阵](../../../docs/rtos_testing_report.md#f446-functional-retest) ·
[共享夹具构建方法](../../README.md#f446-portable-fixtures)

<a id="f446-interaction-archive"></a>
## F446 跨功能与 IRQ 窗口归档

[raw/f446-interactions](raw/f446-interactions/) 保存 32 份板测和 5 份 USB 宿主输出，包含
六份板上旧版预期失败、一份宿主 TX 旧版失败和一份主动损坏包负对照。`runs.json` 保存
原文摘要、构建参数、镜像摘要、各内核快照和整片恢复结论。基线到最终实测内核的补丁为
`baseline_to_tested_final.patch`，最终实测之后的任务/定时器头文件与源码注释补充不改变
执行逻辑。板卡标识和原固件备份只保存在本机批次。

```powershell
python -B hil/evidence/2026-11-11/verify_f446_interactions.py
```

验收器检查回收抢占、合作 yield、栈钩子、定时器命令先后及 DELETE、突发队列与阻塞
发送者、USB 会话、合入后的 IPC、180 秒业务压力和 60 秒生命周期。原始串口保留上一
镜像的开头余留，以本轮启动标记后的结果判定。早期无阻塞发送者的 burst 作为补充观测，
主要性能对照采用后续同夹具的 waiter burst；源码清单针对内核快照，不能还原每个历史
夹具编辑版本。USB TX 窗口仅有宿主执行证据，物理 USB 未验证。

完整 ELF、映射、构建日志、SWD 记录和两份测试前 Flash 备份位于本机
`hil/logs/f446_interaction_20261111_2145/`。

[实测矩阵与限制](../../../docs/rtos_testing_report.md#f446-interaction-hardening) ·
[定向夹具参数](../../README.md#f446-interaction-fixtures) ·
[当日记录](../../../docs/logs/2026-11-11.md#f446-interaction-run)
