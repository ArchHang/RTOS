# 诊断 ON/OFF 实测比较

每格为三次等工作量测量的 cycles/完整交换；交换含 API、循环和实际调度/中断干扰。保留全部原始样本，不作为 WCET。

| 优化 | 路径 | OFF min / median / max | ON min / median / max | 中位数增加 | 增幅 |
| --- | --- | ---: | ---: | ---: | ---: |
| O2 | sem_nowait | 215.41 / 215.41 / 215.41 | 224.38 / 224.38 / 224.38 | +8.97 | +4.16% |
| O2 | queue_nowait | 414.88 / 414.88 / 414.88 | 413.85 / 413.86 / 414.18 | -1.02 | -0.25% |
| O2 | notify_nowait | 228.40 / 228.40 / 228.58 | 236.88 / 237.49 / 239.38 | +9.09 | +3.98% |
| O2 | sem_pingpong | 1420.60 / 1420.61 / 1420.86 | 2167.74 / 2167.98 / 2168.10 | +747.37 | +52.61% |
| O2 | queue_pingpong | 1806.09 / 1806.12 / 1806.25 | 2586.05 / 2586.35 / 2586.57 | +780.24 | +43.20% |
| O2 | notify_pingpong | 1364.79 / 1364.82 / 1365.12 | 2197.92 / 2198.17 / 2198.24 | +833.36 | +61.06% |
| O3 | sem_nowait | 217.41 / 217.41 / 217.41 | 226.41 / 226.41 / 226.59 | +9.00 | +4.14% |
| O3 | queue_nowait | 399.87 / 399.87 / 400.20 | 400.91 / 400.91 / 401.25 | +1.04 | +0.26% |
| O3 | notify_nowait | 226.42 / 226.42 / 226.59 | 237.78 / 238.15 / 238.41 | +11.73 | +5.18% |
| O3 | sem_pingpong | 1358.62 / 1358.67 / 1359.00 | 2165.73 / 2165.79 / 2166.06 | +807.13 | +59.41% |
| O3 | queue_pingpong | 1710.17 / 1710.40 / 1710.41 | 2526.23 / 2526.27 / 2526.27 | +815.87 | +47.70% |
| O3 | notify_pingpong | 1295.52 / 1295.86 / 1295.87 | 2112.69 / 2112.92 / 2113.04 | +817.06 | +63.05% |

| 优化 | 指标 | OFF bytes | ON bytes | 差值 bytes |
| --- | --- | ---: | ---: | ---: |
| O2 | bss_data_bytes | 41612 | 44208 | +2596 |
| O2 | allocated_nobits_bytes | 43040 | 45632 | +2592 |
| O2 | allocated_file_bytes | 36768 | 40184 | +3416 |
| O2 | writable_allocated_bytes | 43160 | 45756 | +2596 |
| O2 | diagnostic_defined_function_bytes | 0 | 392 | +392 |
| O2 | diagnostic_defined_object_bytes | 0 | 2596 | +2596 |
| O3 | bss_data_bytes | 41612 | 44208 | +2596 |
| O3 | allocated_nobits_bytes | 43040 | 45632 | +2592 |
| O3 | allocated_file_bytes | 39216 | 42660 | +3444 |
| O3 | writable_allocated_bytes | 43160 | 45756 | +2596 |
| O3 | diagnostic_defined_function_bytes | 0 | 376 | +376 |
| O3 | diagnostic_defined_object_bytes | 0 | 2596 | +2596 |

`.bss + .data` 为链接后静态数据，NOBITS 另含链接脚本预留区；两者不能相加。defined symbol 数量只归属诊断模块自身，不含其他内核函数新增调用点或所有对齐，不能当作完整 Flash 增量。

四个 ELF 的原始 `arm-none-eabi-size -A` 与 `nm -S` 输出保存在 tools/。完整样本、配置及 ELF 节/符号信息见同目录 report.json。
