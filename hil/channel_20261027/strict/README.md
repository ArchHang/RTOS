# 通道集成后的严格编译矩阵

`run_strict.py` 直接编译当前工程的 17 个自研 RTOS C 翻译单元，排除第三方实现。
第三方与 BSP 头文件按 HIL 构建布局提供。该脚本不修改生产源文件、不链接、不烧录，
也不访问串口。

从工程根目录运行：

```powershell
python hil/channel_20261027/strict/run_strict.py
```

默认使用工程 HIL 的 ARM GCC 11.3 工具目录。`--tools` 可指定其他包含
`arm-none-eabi-gcc` 和 `arm-none-eabi-size` 的目录；`--jobs 1..8` 控制独立编译进程数。
`--out-dir` 必须是 strict 目录下尚不存在的目录，不能覆盖旧证据。
本机 32 位 Windows 工具若在受限环境报系统 DLL 重定位，需要在获准的本地执行环境运行；
这与 C 编译器返回的语法或警告错误不同。

## 矩阵与判据

| 项目 | 数量 | 条件 |
| --- | ---: | --- |
| Cortex-M4F 自研单元 | 408 | 17 单元 × channel 开/关 × BASEPRI/PRIMASK × 断言开/关 × O0/O2/O3 |
| Cortex-M7 代码生成 | 17 | 17 单元，channel=1、BASEPRI=1、断言=1、O2、FPv5 |
| 设计文档 C 示例 | 1 | 从 `docs/rtos_channel_design.md` 提取实际示例，C11 严格语法编译 |
| 通道公共头 | 4 | C11 / C++17 × channel 开/关，检查全部九个 API 的声明与使用 |
| 合计 | 430 | 编译或语法检查；无运行测试 |

所有命令启用 `-Wall -Wextra -Werror`；实际 C 模块使用工程的 `gnu11`。
逐项保存退出码和诊断，非零退出、警告/错误诊断或缺失对象的 stack-usage 输出均判失败。
脚本收集全部项目，不以早期失败掩盖后续结果；输入文件前后摘要不同则整批不能通过。

Cortex-M7 项仍定义 STM32F446xx 并包含 F4 HAL/CMSIS 设备头。这仅验证 M7 编译目标能生成
这些翻译单元的代码，不能证明 H7 BSP、启动代码、链接布局、MPU、DMA 或缓存一致性正确。
C11 业务示例不被声称为 C++17 示例，C++17 检查仅针对公共头的可调用性。

## 证据与清理

每次运行创建独立 evidence 时间目录，包含：

- `compiler.txt`：实际编译器版本。
- `commands.json`：实际编译命令数组。
- `compile.log`、`results.json`：每项的诊断、退出码、耗时及结果。
- `sources.json`、`sources_after.json`：生产 C、相关头、脚本和文档等输入的身份记录。
- `stack_usage.su`：逐项标记的原始 GCC `.su` 内容汇总。
- `sizes.tsv`：每个实际对象的 text/data/BSS 尺寸；`results.json` 另保存 size 命令及输出。
- `probes/`：当次从文档提取的示例及公共头语法探针。
- `summary.json`：完整批次是否通过及各子矩阵数量。

对象仅在私有 `.build/<时间>/` 中短暂生成；记录身份、尺寸和 `.su` 后逐项删除，不留下二进制大件。
未完成或只有部分日志的目录不能当作完整 PASS。对象尺寸与编译成功均不代表链接或硬件运行正确，
运行证据应与宿主模型及独立的板上 HIL 记录一起阅读。
