# AI 文档和示例验证

本目录仅校验开发资料、接口覆盖和独立C示例，不烧录、不打开串口、不调用调试探针，
不修改生产配置、旧HIL脚本、模板、main.c或日常固件。规范与阅读入口见
[AGENTS.md](../../AGENTS.md)、[AI指南](../../docs/ai/README.md)。

## 1. 公共API资料检查

在工程根目录，用Python3.9或更新版本运行：

```powershell
python -B hil/ai_docs/check_contracts.py --write-index
python -B hil/ai_docs/check_contracts.py
```

第一个命令明确重新生成`docs/ai/api_index.json`，第二个只检查当前索引是否一致。
脚本独立扫描16个公共头的全部条件分支，核对函数声明、头内联、函数式宏及公开extern
变量，并对照三份人工审核元数据、逐符号锚点和文档原型。源扫描不以元数据名单为
过滤器，因此遗漏项目会导致失败。桥接补充单列，不宣称覆盖第三方全部API。

同时检查工程自有Markdown（根入口、docs、HIL、源码导览）的本地链接、旧设计手册锚点，
以及生产源码、权威测试夹具、模板和保留脚本与基线Git blob是否相同。已明确退役的旧HIL
脚本单列，要求退役路径确实不存在；不是把现行生产或测试代码变化排除为正常。
3种内存内篡改控制要求缺记录、错误原型、缺锚点均被拒绝；它们不修改原文件，也不是
RTOS运行测试。自然语言语义不能由token匹配证明，仍依赖逐项源码核对和交叉审查。

输出到新的`evidence/docs_时间戳/summary.json`，已有目录不覆盖。JSON记录当前HEAD、
源及文档身份、覆盖、错误、控制结果与Python版本。未实现或未执行的检查不写成PASS。

## 2. 实际ARM编译和部分链接

`compile_examples.py`要求明确提供真实ARM GCC路径，使用同目录的ARM nm。以下是本机
已使用的命令；其他机器应先找到自身编译器，替换该实参，不套用本机工具路径。

```powershell
E:/ProgramData/Anaconda3/python.exe -B hil/ai_docs/compile_examples.py --compiler E:/ST/STM32CubeIDE_1.13.2/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.11.3.rel1.win32_1.1.100.202309141235/tools/bin/arm-none-eabi-gcc.exe
```

编译目标为Cortex-M4、Thumb、FPv4-SP-D16、hard-float、GNU C11，使用F446 BSP包含路径。
全部正向项启用`-Wall -Wextra -Werror`；六个示例另启用`-Wconversion -Wshadow
-Wmissing-prototypes`。这是独立编译，不经项目make，因而不触发旧make的清理问题。

| 范围 | 内容 |
|---|---|
| 基线组合 | BASEPRI/PRIMASK × ASSERT开/关 × O0/O2/O3，共12项 |
| 配置变体 | USB关闭、perf关闭、USB/perf/自动timer初始化关闭、热点统计开启、静态分配宏0、100Hz tick、USB device关闭，共7项 |
| 每项编译 | 6份真实示例C文件及16份当前RTOS支持实现，分别计数 |
| 每项部分链接 | 22个对象用`-r -nostdlib`组合；保留所有对象，检查重复定义，拒绝未满足的`rtos_*`/`ai_example_*`符号 |
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

## 3. 当前确认结果及限制

[本次完整编译](evidence/compile_20261028_121211_010144/summary.json)已完成19项配置：
114个示例对象、304个生产支持对象、19次部分链接、17项头文件正向检查，另2项指定
编译拒绝符合预期。编译器为GNU Tools for STM32 GCC11.3.1，输入前后稳定。
正常编译均无warning。六示例定义23个独立演示函数，未增加RTOS公共符号。

初次两批在检查有效宏时因验证工具处理空宏/CRLF行的问题中断；实际GCC已输出正确
宏值，尚未开始示例对象编译。随后修正解析器，用实际输出及行尾控制核对后重跑。
两次失败事实保留于当日日志；其原始文件及重复文档检查输出已按本轮清理要求删除。
这些属于工具失败，不能归为内核错误或计入正向编译覆盖。

**部分链接不是完整固件链接。** 结果允许并列出HAL、CMSIS环境、C运行库和CherryUSB
协议栈等外部未解析符号；没有启动向量、最终内存布局、可运行入口或硬件初始化。
本次不能由编译/部分链接声称示例时序、IRQ合法性、USB收发或业务压力已验证。
没有进行板卡运行，现有日常固件和main.c保持不变。

接口完整性与文档最终检查查[当日记录](../../docs/logs/2026-10-28.md)，不要把旧HIL通过数
直接算作这批资料的验证。该检查程序的输出也不能证明所有自然语言描述或未知BUG已排除。
