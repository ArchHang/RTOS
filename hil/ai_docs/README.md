# AI 文档和示例验证

本目录提供接口覆盖、设计手册摘录和独立 C 示例的校验工具。F446 整机验证使用
[HIL 入口](../README.md)。规范与阅读入口见
[AGENTS.md](../../AGENTS.md)、[AI指南](../../docs/ai/README.md)。

## 1. 公共API资料检查

在工程根目录使用 Python 3.9 或更新版本。每次执行选用新的本地批次目录。

```powershell
$batch = 'hil/logs/docs_20261111_01'
python -B hil/ai_docs/check_contracts.py --out-dir "$batch/contracts"
```

索引需要更新时，向命令加 `--write-index`，并为该次执行指定新的输出目录。
脚本独立扫描16个公共头的全部条件分支，核对函数声明、头内联、函数式宏及公开extern
变量，并对照三份人工审核元数据、逐符号锚点和文档原型。源扫描不以元数据名单为
过滤器，因此遗漏项目会导致失败。桥接补充单列，不宣称覆盖第三方全部API。

同时检查工程自有Markdown的本地链接、旧设计手册锚点，以及源码和权威测试输入在
扫描前后是否一致。报告列出相对于首次审核Git基线的新增、删除和变化。仅修改文档时，
可用`--require-unchanged-source`附加历史基线字节一致的检查。退役路径仍须不存在。
生成的`hil/logs/`与`hil/fwsrc/`不参与权威源码及资料扫描。
3种内存内篡改控制要求缺记录、错误原型、缺锚点均被拒绝；它们不修改原文件，也不是
RTOS运行测试。自然语言语义不能由token匹配证明，仍依赖逐项源码核对和交叉审查。

输出目录的 `summary.json` 记录当前 HEAD、源码和文档身份、覆盖、错误、
控制结果与 Python 版本。脚本拒绝覆盖已有目录。

## 2. 实际ARM编译和部分链接

`compile_examples.py` 需要 ARM GCC 的完整路径，使用同目录的 ARM nm。
以下示例从已配置的 PATH 获取编译器。若使用 CubeIDE 内置工具链，也可直接指定其路径。

```powershell
$compiler = (Get-Command arm-none-eabi-gcc -ErrorAction Stop).Source
python -B hil/ai_docs/compile_examples.py --compiler $compiler --output "$batch/compile_f446"
```

编译目标为Cortex-M4、Thumb、FPv4-SP-D16、hard-float、GNU C11，使用F446 BSP包含路径。
可选 `--h743-cmsis <STM32H743 工程的 Drivers/CMSIS>` 使用实际 H743 芯片头、
Cortex-M7、FPv5-D16 和 `-mno-unaligned-access` 重跑同一矩阵。工具检查目标 NVIC
优先级位数，记录外部 CMSIS 头的身份。该路径验证 C 对象和可重定位链接，不能代替
H743 的启动向量、链接布局、Cache、外设或实板验证。
全部正向项启用 `-Wall -Wextra -Werror`，七个示例另启用
`-Wconversion -Wshadow -Wmissing-prototypes`。脚本直接调用编译器。

| 范围 | 内容 |
|---|---|
| 基线组合 | BASEPRI/PRIMASK × ASSERT开/关 × O0/O2/O3，共12项 |
| 配置变体 | USB关闭、perf关闭、USB/perf/自动timer初始化关闭、热点统计开启、静态分配宏0、100Hz tick、USB device关闭，共7项 |
| 每项编译 | 当前7份真实示例C文件及16份RTOS支持实现，分别计数；显式文件清单拒绝遗漏/意外新增 |
| 每项部分链接 | 23个对象用`-r -nostdlib`组合；保留所有对象，检查重复定义，拒绝未满足的`rtos_*`/`ai_example_*`符号 |
| 头文件正向 | 基线16个公共头逐一独立包含编译；另1项USB关闭后显式包含类型依赖的正例 |
| 指定编译拒绝 | 定时器参数个数错误；现有USB关闭头缺少类型依赖的负例。须匹配指定诊断，工具启动失败不能算拒绝成功 |

大部分生产配置没有`#ifndef`，变体通过工作目录内的16个公共头副本覆盖指定配置，
不修改原头、不靠`-D`假装覆盖。每项保存编译器`-dM -E`实际输出并核对目标宏，记录
副本摘要与精确覆盖值；BASEPRI/ASSERT使用原头支持的命令行覆盖。配置宏0不一定
改变内核实现路径，例如USE_TIMERS仅控制自动初始化，USE_STATIC_ALLOCATION不切换
当前task_create的分配路径；以契约为准。

每批保存工具版本/摘要、命令、stdout/stderr、输入身份、有效宏、外部未解析符号和
汇总。生成对象/头副本仅位于该批独立`.objects`子目录，用完后删除，不复制到日常
Debug或对象清单。批次存续期间保留失败尝试，不覆盖或拼成通过；用户授权清理时在
每日日志说明删除范围和复核限制。

## 3. 结果与归档

部分链接检查示例与内核对象的符号关系，记录尚未解析的 HAL、C 运行库及
CherryUSB 等外部依赖。启动向量、最终内存布局和板上时序由项目构建及对应 HIL 用例验证。

当次验证的配置、结果及本地路径见[工程交付报告](../../docs/logs/2026-11-11.md#delivery-report)。
2026-10-28 和 2026-10-29 的汇总保留在[每日日志](../../docs/README.md#daily-logs)。
旧版提交 `446d724091bacd7db4fc619e9742e56642f61df4` 含当时的
`hil/ai_docs/evidence/` 归档。

## 4. 设计手册内嵌样例同步

手册使用`example-function`/`example-file`标记绑定到实际示例源。只读检查：

```powershell
python -B hil/ai_docs/check_handbook_examples.py --output "$batch/handbook_examples.json"
```

明确需要更新正文摘录时才加`--sync`，它只替换标记的C块，不修改示例或生产代码。
函数摘录的完整编译单元/静态依赖由链接给出，未标记的机制摘要/结构/汇编不声称参与编译。
此检查比较文本，不代替真实ARM编译，也不能证明样例运行时业务正确。

2026-10-29 的手册重写和示例验证见[当日交付记录](../../docs/logs/2026-10-29.md#delivery-report)。
