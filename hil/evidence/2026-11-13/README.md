# F446 堆与 TIM5 硬件实测证据

采集于 2026-11-13。目标板为 NUCLEO-F446RE，CPU 168 MHz，TIM5 84 MHz，构建为 `-O2`，临界区使用 BASEPRI，串口波特率 115200。每个测试镜像刷入两次，共保留 50 份原始 UART 捕获，其中 16 份来自最终采用的 RTOS 堆。覆盖同一业务轨迹、首次碎片申请失败、计时插桩、真实 TIM5 中断和边界账本。

| 日志组 | 测试内容 |
| --- | --- |
| `irq`、`irq_direct` | TIM5 CCR1 硬件比较相位扫描，分别用 P4、P6 检查屏蔽行为。带 `_direct` 的镜像额外使用 DWT 测量临界区 |
| `periodic`、`periodic_direct` | 业务轨迹运行期间由 TIM5 产生 P6 中断，分别记录无插桩响应及带插桩的屏蔽区间 |
| `industry` | 无后台 TIM5 测量插桩的 32 KiB 五类业务轨迹 |
| `failure` | 128 个空闲孔下的首次和重复申请失败、最大连续容量查询与恢复 |
| `weighted` | 三类业务轨迹的操作次数和周期账本 |
| `boundary` | 申请边界、对齐、最小拆分、耗尽、异常释放、屏蔽下失败与容量恢复 |
| `mixed`、`stress` | 采用前一个 `layout` 版本的混合测试和每次 20 秒的四源并发压力回归 |

`accepted` 为最终堆，`current` 为当时的对照堆快照，`zephyr` 为启用运行时统计的 Zephyr v4.2.0 `sys_heap`，通过适配层使用等价的 BASEPRI 保护和 8 字节对齐。失败与加权数据还保留了用来说明取舍的候选快照。每组使用的源码和测试镜像摘要见 [manifest.json](manifest.json)。源码快照和测试镜像均在打包时与本地归档核对。该目录保留可公开的串口原始字节与身份摘要。完整 ELF、烧录器记录及用户固件备份均保存在本地 HIL 归档。

离线核验使用 Python 标准库，从仓库根目录运行。

```text
python -B hil/evidence/2026-11-13/verify.py --check-source
```

脚本验证原始串口文件摘要，按唯一的 `START` 与 `DONE` 划定有效区间，并检查场景、样本数量、部分统计、输入轨迹标识、操作账本和容量恢复。部分 Zephyr 周期采集在 `START` 之前含有上一轮尾部，原始字节照常保留。可去掉 `--check-source` 独立核验串口归档，保留该参数则同时核对当前检出版本的 RTOS 堆与测试夹具源码，允许换行符转换。镜像摘要用于关联现场记录。离线脚本使用记录中的 ELF 摘要，现场测量与烧录须按复跑步骤执行。参见[硬件复跑入口](../../heap_study/README.md)、[每日日志](../../../docs/logs/2026-11-13.md#heap-timer-latency)和[性能报告](../../../docs/rtos_performance_report.md)。
