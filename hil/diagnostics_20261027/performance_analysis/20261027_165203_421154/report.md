# 诊断 ON/OFF 实测比较

每格为三次等工作量测量的 cycles/完整交换；交换含 API、循环和实际调度/中断干扰。保留全部原始样本，不作为 WCET。

| 优化 | 路径 | OFF min / median / max | ON min / median / max | 中位数增加 | 增幅 |
| --- | --- | ---: | ---: | ---: | ---: |
| O2 | sem_nowait | 215.41 / 215.41 / 215.41 | 221.39 / 221.39 / 221.39 | +5.98 | +2.78% |
| O2 | queue_nowait | 414.88 / 414.88 / 414.88 | 409.59 / 409.59 / 409.59 | -5.29 | -1.27% |
| O2 | notify_nowait | 228.40 / 228.40 / 228.58 | 236.43 / 236.43 / 236.65 | +8.03 | +3.52% |
| O2 | sem_pingpong | 1420.60 / 1420.61 / 1420.86 | 2308.12 / 2308.12 / 2308.12 | +887.51 | +62.47% |
| O2 | queue_pingpong | 1806.09 / 1806.12 / 1806.25 | 2639.39 / 2639.42 / 2639.42 | +833.30 | +46.14% |
| O2 | notify_pingpong | 1364.79 / 1364.82 / 1365.12 | 2332.00 / 2332.03 / 2332.05 | +967.21 | +70.87% |
| O3 | sem_nowait | 217.41 / 217.41 / 217.41 | 223.42 / 223.42 / 223.42 | +6.00 | +2.76% |
| O3 | queue_nowait | 399.87 / 399.87 / 400.20 | 394.66 / 394.67 / 394.67 | -5.21 | -1.30% |
| O3 | notify_nowait | 226.42 / 226.42 / 226.59 | 235.42 / 235.59 / 235.59 | +9.17 | +4.05% |
| O3 | sem_pingpong | 1358.62 / 1358.67 / 1359.00 | 2296.92 / 2296.93 / 2296.98 | +938.26 | +69.06% |
| O3 | queue_pingpong | 1710.17 / 1710.40 / 1710.41 | 2610.32 / 2610.32 / 2610.33 | +899.92 | +52.61% |
| O3 | notify_pingpong | 1295.52 / 1295.86 / 1295.87 | 2218.00 / 2218.04 / 2218.28 | +922.18 | +71.16% |

| 优化 | 指标 | OFF bytes | ON bytes | 差值 bytes |
| --- | --- | ---: | ---: | ---: |
| O2 | bss_data_bytes | 41612 | 44208 | +2596 |
| O2 | allocated_nobits_bytes | 43040 | 45632 | +2592 |
| O2 | allocated_file_bytes | 36768 | 38892 | +2124 |
| O2 | writable_allocated_bytes | 43160 | 45756 | +2596 |
| O2 | diagnostic_defined_function_bytes | 0 | 916 | +916 |
| O2 | diagnostic_defined_object_bytes | 0 | 2596 | +2596 |
| O3 | bss_data_bytes | 41612 | 44208 | +2596 |
| O3 | allocated_nobits_bytes | 43040 | 45632 | +2592 |
| O3 | allocated_file_bytes | 39216 | 41632 | +2416 |
| O3 | writable_allocated_bytes | 43160 | 45756 | +2596 |
| O3 | diagnostic_defined_function_bytes | 0 | 1164 | +1164 |
| O3 | diagnostic_defined_object_bytes | 0 | 2596 | +2596 |

`.bss + .data` 为链接后静态数据，NOBITS 另含链接脚本预留区；两者不能相加。defined symbol 数量只归属诊断模块自身，不含其他内核函数新增调用点或所有对齐，不能当作完整 Flash 增量。

四个 ELF 的原始 `arm-none-eabi-size -A` 与 `nm -S` 输出保存在 tools/。完整样本、配置及 ELF 节/符号信息见同目录 report.json。
