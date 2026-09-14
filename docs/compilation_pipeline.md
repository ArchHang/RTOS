# 从源代码到可执行文件——编译原理完整技术手册

> **适用平台**：GCC + ARM Cortex-M 嵌入式系统  
> **语言聚焦**：C 语言（C99/C11），部分示例涵盖 C++ 的关联特性  
> **文档目标**：从第一性原理出发，逐阶段剖析 gcc 工具链将 `.c` 文本转化为 `.elf/.bin` 机器码的全部内部操作

---

## 目录

- [第一章：预处理——文本世界的宏宇宙](#第一章预处理文本世界的宏宇宙)
  - [1.1 词法阶段：物理源字符到预处理 Token](#11-词法阶段物理源字符到预处理-token)
  - [1.2 三字符组替换与行拼接](#12-三字符组替换与行拼接)
  - [1.3 注释替换](#13-注释替换)
  - [1.4 `#include` 的递归展开机制](#14-include-的递归展开机制)
  - [1.5 宏定义与宏展开](#15-宏定义与宏展开)
  - [1.6 条件编译](#16-条件编译)
  - [1.7 `#pragma` 与 `_Pragma` 的编译器通道](#17-pragma-与-_pragma-的编译器通道)
  - [1.8 行标记与诊断指令](#18-行标记与诊断指令)
  - [1.9 预处理器的内部状态机](#19-预处理器的内部状态机)
  - [1.10 预处理输出与衔接编译阶段](#110-预处理输出与衔接编译阶段)
- [第二章：编译——从语义树到中间代码](#第二章编译从语义树到中间代码)
  - [2.1 词法分析](#21-词法分析)
  - [2.2 语法分析](#22-语法分析)
  - [2.3 语义分析](#23-语义分析)
  - [2.4 抽象语法树的构建与遍历](#24-抽象语法树的构建与遍历)
  - [2.5 中间表示生成](#25-中间表示生成)
  - [2.6 GCC GIMPLE 三地址码](#26-gcc-gimple-三地址码)
  - [2.7 编译优化](#27-编译优化)
  - [2.8 SSA 形式与全局优化](#28-ssa-形式与全局优化)
  - [2.9 寄存器分配与指令选择](#29-寄存器分配与指令选择)
  - [2.10 目标代码生成](#210-目标代码生成)
  - [2.11 编译阶段与汇编阶段的接口](#211-编译阶段与汇编阶段的接口)
- [第三章：汇编——从助记符到机器码](#第三章汇编从助记符到机器码)
  - [3.1 ARM Thumb-2 指令集架构概要](#31-arm-thumb-2-指令集架构概要)
  - [3.2 汇编器的两遍扫描](#32-汇编器的两遍扫描)
  - [3.3 指令编码](#33-指令编码)
  - [3.4 伪指令与宏](#34-伪指令与宏)
  - [3.5 字面量池管理](#35-字面量池管理)
  - [3.6 符号表与重定位条目](#36-符号表与重定位条目)
  - [3.7 ELF 目标文件格式](#37-elf-目标文件格式)
  - [3.8 调试信息的嵌入](#38-调试信息的嵌入)
  - [3.9 汇编器输出与链接阶段的接口](#39-汇编器输出与链接阶段的接口)
- [第四章：链接——将零件组装成完整程序](#第四章链接将零件组装成完整程序)
  - [4.1 链接器的总体工作流](#41-链接器的总体工作流)
  - [4.2 段合并](#42-段合并)
  - [4.3 符号解析](#43-符号解析)
  - [4.4 地址分配与链接脚本](#44-地址分配与链接脚本)
  - [4.5 重定位修复](#45-重定位修复)
  - [4.6 ARM Thumb-2 重定位类型](#46-arm-thumb-2-重定位类型)
  - [4.7 弱符号与强符号](#47-弱符号与强符号)
  - [4.8 库文件的处理](#48-库文件的处理)
  - [4.9 链接时优化](#49-链接时优化)
  - [4.10 最终输出与启动流程](#410-最终输出与启动流程)
- [附录：命令速查与扩展阅读](#附录命令速查与扩展阅读)

---

# 第一章：预处理——文本世界的宏宇宙

预处理是整个编译流水线的第一站。它的输入是 C 语言源文件（`.c`）和头文件（`.h`），输出是一个经过完整宏展开的、自包含的 C 语言文本文件（`.i`）。预处理器（GCC 中为 `cpp` 程序，即 C Preprocessor）本质上是一个**文本处理引擎**，它不关心 C 语言的语法语义，只根据预处理指令操作字符流。

C 标准（ISO/IEC 9899）定义了预处理的 8 个翻译阶段（Translation Phases，共 8 个阶段，预处理占据 Phase 4）。以下按照这些阶段的实际执行顺序逐一讲解。

---

## 1.1 词法阶段：物理源字符到预处理 Token

### 1.1.1 字符集映射（Translation Phase 1）

C 标准要求编译器首先将源文件的**物理字符**映射为源字符集。在现代环境中，UTF-8 已成为事实标准，但标准本身还规定了三字符组（trigraph）等历史遗存。

核心操作：

1. 若源文件包含三字符组序列（以 `??` 开头），按标准表映射为对应的单字符。例如 `??=` 映射为 `#`。**GCC 默认禁用三字符组替换**（由 `-trigraphs` 启用），这是对代码可读性的务实妥协。

2. 任何依赖于实现的换行符表示统一映射为 ASCII Line Feed（`\n`）。Windows (CR+LF)、旧 Mac (CR)、Unix (LF) 均归一化为单一 `\n`。

3. 非标准的行尾反斜杠（`\`）处理进入下一阶段。

### 1.1.2 行拼接（Translation Phase 2）

这是一个容易被忽视但极为重要的阶段。在 C 语言中，以反斜杠 `\` 字符紧跟换行符结尾的逻辑行会被合并为一行：

```c
// 物理上 3 行，逻辑上 1 行
#define LONG_MACRO(a, b, c) \
    do {                     \
        foo(a);              \
        bar(b, c);           \
    } while (0)
```

处理步骤：
1. 扫描每一个 "反斜杠 + 换行" 序列；
2. 删除这两个字符；
3. 将后续行的内容接到当前行末尾；
4. 重复直到无更多拼接。

**关键陷阱**：反斜杠后必须是**纯粹的换行符**，若反斜杠后有任何空白字符（如 Windows 的 CR `\r`），行拼接会失败。这是嵌入式开发中在 Windows 和 Linux 之间迁移代码时经常遇到的麻烦——GCC 在 Windows 上默认正确识别 CR+LF，但其他工具可能不识别，导致 `\` 行拼接异常。

### 1.1.3 预处理 Token 的分类（Translation Phase 3）

预处理器将预处理后的字符流切分为**预处理 Token**。预处理 Token 的种类如下：

| Token 类型 | 示例 | 说明 |
|-----------|------|------|
| header-name | `<stdio.h>` 或 `"mylib.h"` | `#include` 的目标头文件名 |
| identifier | `main`, `uint32_t`, `__GNUC__` | 标识符 |
| pp-number | `42`, `0xFF`, `3.14`, `1e-9` | 预处理数字（后续由编译器精确解析） |
| character-constant | `'A'`, `'\n'`, `L'中'` | 字符常量 |
| string-literal | `"hello"`, `L"你好"`, `u8"utf8"` | 字符串字面量 |
| punctuator | `{`, `}`, `+`, `->`, `##` | 标点符号和运算符 |
| 非空白字符组合 | `#` 单独出现 | 非上述分类的其他字符 |

**pp-number 的宽松定义**：预处理数字的语法定义极其宽松——一个数字/句点开头，后跟任意字母数字下划线序列。这意味着 `0xDEADBEEF`、`1.2.3`、`42program` 都是合法的预处理数字（虽然后两者在编译阶段会报错）。

**空白的重要性**：预处理 Token 之间必须有明确的边界。以下两个声明虽然对人类来说"看起来一样"，但对预处理器而言完全不同：

```c
// 声明 1: int 和 main 是两个独立的 identifier Token
int main(void);

// 声明 2: #define 展开后 "intmain" 是一个 identifier
#define TYPE int
TYPE main(void);  // → intmain(void); ← 不是 int main(void)!
```

预处理器不会在 `TYPE` 展开后自动插入空格。如果需要分隔符，必须使用 `##` 或显式留空。

---

## 1.2 预处理指令的执行（Translation Phase 4）

Phase 4 是预处理器的**核心阶段**。在这一阶段，预处理器逐行读取代码，执行预处理指令并展开宏。此阶段结束后，文件中不应再存在任何预处理指令。

### 1.2.1 预处理指令的格式

一条合法的预处理指令以 `#` 开头（`#` 必须是该行的第一个非空白字符），后跟指令名和参数：

```
# 指令名 参数1 参数2 ...
```

预处理指令可以跨行（通过行拼接），也可以在 `#` 和指令名之间有空白（但不建议这样做）。除了少数例外（如 `#define` 可以有 `##` 和 `#`），指令的主体不受宏展开影响。

---

## 1.3 `#include` 的递归展开机制

### 1.3.1 两种包含形式的路径查找

```c
#include <rtos_config.h>    // 形式1: 尖括号
#include "my_library.h"     // 形式2: 双引号
```

路径查找的规则因 GCC 而略有不同，但通用规则为：

1. **尖括号包含**：按照 `-I` 选项指定的系统头文件搜索路径（system include path）依次查找。典型路径包括：
   - GCC 内置的系统头文件目录（如 `<GCC-install>/arm-none-eabi/include/`）
   - 通过 `-isystem` 指定的路径
   - 编译器内置搜索路径

2. **双引号包含**：先在**当前源文件所在的目录**中查找，若未找到再退回到尖括号的搜索路径查找。

```bash
# GCC 命令行指定额外搜索路径
arm-none-eabi-gcc -I./RTOS/Core/Inc -I./RTOS/Port/GCC/ARM_CM7 main.c
```

### 1.3.2 头文件守卫与 `#pragma once`

为防止同一个头文件被多次包含（导致重复定义错误），常见两种守卫方式：

```c
// 传统守卫（标准 C）
#ifndef RTOS_CONFIG_H
#define RTOS_CONFIG_H
// ... 头文件内容 ...
#endif /* RTOS_CONFIG_H */

// 或使用 #pragma once（非标准但广泛支持）
#pragma once
```

*传统守卫*是通过预处理的条件编译实现的：第一次包含头文件时 `RTOS_CONFIG_H` 未定义，宏体被正常展开；第二次包含时，`RTOS_CONFIG_H` 已定义，`#ifndef` 条件为假，整段内容跳过——只有 `#ifndef`、`#define`、`#endif` 这三行被处理，其余部分不参与后续预处理。

`#pragma once` 是编译器扩展——预处理器记录每个文件的唯一标识（通常是 inode + 设备号或文件路径），同一物理文件仅展开一次。它的性能优于传统守卫（省去宏定义查找+条件判断），但移植性较弱（部分老编译器或嵌入式编译器不支持）。

### 1.3.3 递归包含与传播

`#include` 可以是嵌套的：

```
main.c ──include──→ rtos_config.h
                    rtos_task.h ──include──→ rtos_types.h
                                             rtos_sched.h ──include──→ rtos_config.h (已守卫，跳过)
```

预处理器维护一个内部包含栈来跟踪嵌套深度和文件边界。当展开嵌套的 `#include` 时，处理完嵌套文件后恢复原文件的展开。**`#line` 指令**（后详）在此过程中被自动插入以维护文件/行号映射。

---

## 1.4 宏定义与宏展开

宏（Macro）是预处理阶段最复杂也最强大的特性。一条 `#define` 指令定义一个宏名与一个替换列表（replacement list）之间的关联。

### 1.4.1 对象式宏

```c
#define BUFFER_SIZE  256
#define PI           3.14159265359
#define greeting     "Hello, World!"
```

对象式宏是简单的文本替换——预处理器在源文本中遇到宏名时，用替换列表替换之。注意替换列表中**不进行任何求值**——`PI` 被替换为 `3.14159265359`（一组 pp-number 和标点 Token），实际的浮点数解析在编译阶段发生。

```c
// 源代码
int arr[BUFFER_SIZE];    // int arr[256];

// 多重替换
#define A 10
#define B A
#define C B + 1
int x = C;   // → 展开: B + 1 → A + 1 → 10 + 1
// 最终: int x = 10 + 1;
// 编译器在语义阶段将其常量折叠为 int x = 11;
```

### 1.4.2 函数式宏

函数式宏在宏名后紧跟一组圆括号，括号中是逗号分隔的参数列表：

```c
#define MAX(a, b)  ((a) > (b) ? (a) : (b))

int x = MAX(3 + 1, 5);   // → ((3 + 1) > (5) ? (3 + 1) : (5))
                          // → ((4) > (5) ? (4) : (5))
                          // → (5)
```

展开过程：
1. 预处理器在宏调用点用实际参数替换宏定义体中的形式参数；
2. 替换前，参数中的宏先被展开（除非参数在宏体中被 `#` 或 `##` 操作符处理）；
3. 整个替换完成后，替换文本再次被扫描进行宏展开。

**多重求值陷阱**：

```c
#define ABS(a)  ((a) < 0 ? -(a) : (a))

int x = ABS(x++);   // → ((x++) < 0 ? -(x++) : (x++))
                    // ↑ x 被递增了两次！结果完全不可预测
```

这是函数式宏最臭名昭著的问题。C99 引入了 `__typeof__`（GCC 扩展）和 `__auto_type` 作为缓解手段，内联函数（`static inline`）是更安全的替代方案。

### 1.4.3 `#` 字符串化操作符

在函数式宏的替换列表中，`#param` 将参数转换为字符串字面量：

```c
#define STRINGIFY(x)  #x
#define LOG(expr)     printf("%s = %d\n", #expr, (expr))

LOG(a + b);   // → printf("%s = %d\n", "a + b", (a + b));
```

字符串化操作将参数 Token 序列的双引号和反斜杠进行转义，并在两端添加双引号。

### 1.4.4 `##` Token 连接操作符

`##` 将相邻的两个预处理 Token 连接成一个：

```c
#define CONCAT(a, b)  a ## b
#define GPIO(mode, pin)  CONCAT(GPIO_PIN_, pin)  // 与下一宏配合

int val = GPIO(OUTPUT, 5);  // → CONCAT(GPIO_PIN_, 5) → GPIO_PIN_5
```

`##` 在参数替换**之前**被处理——`##` 的两侧参数先不展开，连接后再整体展开。这个顺序差异是很多宏 bug 的根源。

### 1.4.5 变参宏（C99）

```c
#define DEBUG(fmt, ...)  printf("[%s:%d] " fmt, __FILE__, __LINE__, __VA_ARGS__)

DEBUG("value = %d\n", x);  // → printf("[main.c:42] value = %d\n", x);
DEBUG("hello\n");           // → printf("[main.c:43] hello\n", );  ← C99 空 __VA_ARGS__ 导致逗号冗余!

// GCC 扩展: ##__VA_ARGS__ 处理空参数
#define DEBUG_GCC(fmt, ...)  printf("[%s:%d] " fmt, __FILE__, __LINE__, ##__VA_ARGS__)
```

C99 的 `__VA_ARGS__` 在参数为空时会留下一个冗余逗号，GCC 的 `##__VA_ARGS__` 扩展解决了这个问题。C++20/C23 的 `__VA_OPT__` 提供了标准化的替代方案：

```c
// C23
#define DEBUG_C23(fmt, ...)  printf("[%s:%d] " fmt, __FILE__, __LINE__ __VA_OPT__(,) __VA_ARGS__)
```

### 1.4.6 预定义宏

编译器预定义了大量宏，无需 `#define` 即可使用：

| 宏名称 | 值 | 说明 |
|--------|-----|------|
| `__FILE__` | `"main.c"` | 当前源文件名（字符串字面量） |
| `__LINE__` | `42` | 当前行号（整数常量） |
| `__DATE__` | `"Jan 01 2025"` | 编译日期 |
| `__TIME__` | `"12:00:00"` | 编译时间 |
| `__STDC__` | `1` | 是否遵循 ISO C 标准 |
| `__STDC_VERSION__` | `201112L` | 标准版本（C99: 199901, C11: 201112） |
| `__GNUC__` | `12` | GCC 主版本号 |
| `__ARM_ARCH` | `7` | ARM 架构版本号 |
| `__ARM_ARCH_7EM__` | `1` | Cortex-M4F/M7F 特征宏 |
| `__thumb__` | `1` | 当前编译为 Thumb 模式 |
| `__OPTIMIZE__` | `1` | 启用了优化（-O1 及以上） |

这些宏通常是嵌入式**条件编译**的核心基础：

```c
#if __STDC_VERSION__ < 201112L
    #error "This project requires C11 or later"
#endif

#if defined(__ARM_ARCH_7EM__) && !defined(__SOFTFP__)
    #define HAS_FPU  1
#else
    #define HAS_FPU  0
#endif
```

---

## 1.5 条件编译

条件编译允许根据宏定义或常量表达式的值决定是否编译某段代码。

### 1.5.1 基本条件指令

```c
#if 常量表达式
    // 若表达式求值为非零, 编译此块
#elif 常量表达式
    // 否则, 若此表达式为非零, 编译此块
#else
    // 否则编译此块
#endif
```

```c
#ifdef 宏名   // 等价于 #if defined(宏名)
#ifndef 宏名  // 等价于 #if !defined(宏名)
```

**重要限制**：`#if` 和 `#elif` 的表达式必须是常量表达式。表达式中的标识符若不是已定义的宏名，则被替换为 0（不是报错！）：

```c
#if UNDEFINED_MACRO
    printf("不会被执行\n");  // UNDEFINED_MACRO → 0, 条件为假
#endif
```

这经常导致错误——如果拼错了宏名，预处理器**静默地**将其替换为 0，不发出任何警告。`-Wundef` 编译选项可以捕捉此类问题。

### 1.5.2 `defined` 操作符

```c
#if defined(CONFIG_FPU) && !defined(CONFIG_SOFT_FLOAT)
    // FPU 已配置且未禁用软浮点
#endif
```

`defined` 操作符是预处理阶段的"标识符已定义?"查询——它不是函数，不产生运行时开销。`defined` 可以和逻辑运算符 `&&`、`||`、`!` 自由组合。

### 1.5.3 条件编译的嵌套与递归

条件编译块可以任意嵌套：

```c
#ifdef FEATURE_A
    #if FEATURE_A_VERSION >= 2
        // A 的 V2 实现
        #ifdef FEATURE_B
            // 还需要 B
        #else
            #warning "FEATURE_A_V2 requires FEATURE_B"
        #endif
    #else
        // A 的 V1 实现
    #endif
#else
    // 无 FEATURE_A 的后备代码
#endif
```

`#if` 块的嵌套深度不受标准限制（GCC 可支持超过 200 层），但在大型代码库中应避免深度嵌套，使用 `#elif` 链更可读。

---

## 1.6 `#pragma` 与 `_Pragma` 的编译器通道

### 1.6.1 `#pragma` 指令

`#pragma` 是编译器扩展的标准入口——任何以 `#pragma` 开头的指令若不被当前编译器识别，将被静默忽略（不报错，这是 C 标准要求的）。

常见 pragma 指令：

```c
#pragma once                        // 头文件仅包含一次
#pragma GCC optimize("O3")          // 局部覆盖优化级别
#pragma GCC diagnostic ignored "-Wunused" // 抑制警告
#pragma pack(push, 1)               // 结构体 1 字节对齐
#pragma message("Compiling module X")     // 编译时打印消息
```

本项目中 `#pragma` 的用法：

```c
// rtos_config.h
#pragma once  // 替代传统头文件守卫
```

### 1.6.2 `_Pragma` 操作符（C99）

`_Pragma` 是 `#pragma` 的函数式等价物，允许在宏内部使用：

```c
#define SUPPRESS_WARNING(w)  _Pragma(#w)

SUPPRESS_WARNING(GCC diagnostic ignored "-Wunused-variable");
// 等价于 #pragma GCC diagnostic ignored "-Wunused-variable"
```

字符串化 `#w` 将宏参数变为字符串字面量，再传给 `_Pragma` 处理。这解决了 `#pragma` 不能作为宏展开结果的问题。

---

## 1.7 行标记与诊断指令

### 1.7.1 `#line` 指令

预处理器在处理 `#include` 时，自动在输出中插入 `#line` 指令，使得编译器的错误信息能指向**原始的源文件位置**而非预处理后的中间文件：

```
# 1 "main.c"
# 1 "<built-in>" 1
# 1 "<command-line>" 1
# 1 "main.c"

int main(void) {
    bad_function();  // ← 编译器报告: main.c:5: error: 'bad_function' undeclared
}
```

手工插入 `#line` 可以修改行号和文件名：

```c
#line 100 "generated_code.c"
int x = bad;  // 错误报告: generated_code.c:100: error
```

### 1.7.2 `#error` 与 `#warning`

```c
#if RTOS_CONFIG_MAX_PRIORITIES > 32
    #error "MAX_PRIORITIES must be <= 32 (32-bit bitmap)"
#endif

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
    #warning "This code assumes little-endian byte order"
#endif
```

- `#error` 立即终止预处理，输出指定的错误消息；
- `#warning`（GCC 扩展）输出警告但不终止编译。

在本项目中的典型应用：编译期验证配置常量范围，若超限则立即停止编译，防止产生不安全的二进制文件。

---

## 1.8 预处理器的内部状态机

### 1.8.1 宏扩展的递归保护

预处理器在展开宏时维护一个"正在展开的宏"列表，防止无限递归：

```c
#define FOO  BAR
#define BAR  FOO
int x = FOO;  // → FOO 被展开为 BAR
              // → BAR 尝试展开为 FOO, 但 FOO 已在展开列表中
              // → BAR 保留为字面标识符 BAR (不再展开)
              // 最终: int x = BAR;   (未定义标识符, 编译阶段报错)
```

这个递归保护机制被称为"蓝漆规则"（Blue Paint Rule）：一旦一个宏被标记为"正在展开中"，在该轮展开完成前不会被再次尝试展开。

### 1.8.2 宏展开的两次扫描

宏展开分两步：
1. 第一次扫描参数列表——实参先被扫描，其中的宏被展开；
2. 第二次扫描替换列表——将展开后的实参插入替换列表，再整体扫描展开。

```c
#define STR(s)  #s
#define XSTR(s) STR(s)
#define VAL     42

STR(VAL)   // Step 1: 参数 s=VAL, s 前有 # → 不展开 VAL
           // Step 2: #VAL → "VAL"
           // 结果: "VAL"

XSTR(VAL)  // Step 1: 参数 s=VAL, s 前无 #/## → 展开 VAL → 42
           // Step 2: STR(42) → #42 → "42"
           // 结果: "42"
```

这个 `XSTR(STR)` 的"额外间接层"模式是 C 预处理中最重要的设计模式之一——任何需要对宏**展开后的值**进行字符串化的场景，都需要这样加一层间接。

---

## 1.9 预处理输出与衔接编译阶段

经过上述所有步骤后，预处理器的输出具有以下特征：

1. **无预处理指令残留**——所有 `#define`、`#include`、`#if` 等指令已被执行完毕。
2. **无注释**——所有注释替换为单个空格。
3. **宏名已全部展开**——只保留最终展开结果中的标识符。
4. **包含 `#line` 标记**——保持与原始源文件的行号对应关系。

你将预处理结果保存为 `.i` 文件：

```bash
arm-none-eabi-gcc -E -o main.i main.c -I./RTOS/Core/Inc
```

这个 `.i` 文件有数百到数千行，每一行都是可直接交给编译器的纯 C 语言文本。**预处理和编译之间没有复杂的接口协议**——预处理器的输出就是一个合法的 C 语言翻译单元，编译器可以直接读取它。

---

# 第二章：编译——从语义树到中间代码

编译阶段是整条流水线中**最复杂、技术含量最高**的环节。它将预处理后的 C 语言翻译单元转化为平台的汇编语言（`.s` 文件）。在 GCC 内部，由 `cc1` 程序（C 编译器前端）执行此阶段。

编译本身可以进一步划分为多个子阶段：词法分析、语法分析、语义分析、中间表示生成、中间代码优化、目标代码生成。以下逐一详述。

---

## 2.1 词法分析

### 2.1.1 任务定义

词法分析器（Lexer/Scanner）将字符流切分为有意义的**Token（词法单元）**。Token 是编译器的"词"——它是语法分析器能识别的最小语义单位。

### 2.1.2 Token 的种类

C 语言的 Token 分为以下几大类：

**1. 关键字（Keywords）**

关键字是 C 语言保留的标识符，不能作为用户定义的变量名/函数名。C11 标准定义了 44 个关键字：

```
auto, break, case, char, const, continue, default, do, double,
else, enum, extern, float, for, goto, if, inline, int, long,
register, restrict, return, short, signed, sizeof, static, struct,
switch, typedef, union, unsigned, void, volatile, while,
_Alignas, _Alignof, _Atomic, _Bool, _Complex, _Generic,
_Imaginary, _Noreturn, _Static_assert, _Thread_local
```

其中以下划线大写字母开头的（如 `_Bool`、`_Static_assert`）是 C11 新增关键字。为避免与现有代码的变量名冲突，C11 标准委员会采用了这种命名策略。

**2. 标识符（Identifiers）**

用户定义的名称：变量名、函数名、类型名、标签名。标识符的规则：以字母或下划线开头，后跟字母、数字或下划线。

**3. 字面量（Literals）**

- 整数常量：`42`（十进制）、`0x2A`（十六进制）、`052`（八进制）、`0b101010`（GCC 扩展二进制）
- 浮点常量：`3.14`、`1.5e-3`、`6.02E23`
- 字符常量：`'A'`、`'\n'`、`'\x41'`
- 字符串字面量：`"hello"`、`"multi\nline"`

**4. 运算符和标点符号**

`+`、`-`、`*`、`/`、`%`、`++`、`--`、`==`、`!=`、`<`、`>`、`<=`、`>=`、`&&`、`||`、`!`、`&`、`|`、`^`、`~`、`<<`、`>>`、`=`、`+=`、`-=`、`*=`、`/=`、`%=`、`&=`、`|=`、`^=`、`<<=`、`>>=`、`?`、`:`、`,`、`;`、`(`、`)`、`[`、`]`、`{`、`}`、`.`、`->`、`...`

### 2.1.3 词法分析的实现原理

现代编译器中，词法分析通常通过**有限自动机（Finite Automaton）**实现。对于 C 语言而言，一个确定有限自动机（DFA）可以在 O(n) 时间内完成整个翻译单元的 Token 化。

以整数常量的识别为例，DFA 状态转换如下：

```
          [0]  ──[digit]──→ [整数十进制]
          [0]  ──[0]─────→ [前缀0]
       [前缀0] ──[x/X]───→ [前缀0x]
       [前缀0] ──[0-7]───→ [整数八进制]
      [前缀0x] ──[0-9a-f]─→ [整数十六进制]
       [整数*] ──[digit]──→ [整数*]
       [整数*] ──[other]──→ ACCEPT
```

GCC 使用手工编写的递归下降词法分析器（而非自动生成的 lex/yacc），能更好地处理错误恢复和 C 语言的角例。

### 2.1.4 复合 Token 的歧义消解

C 语言的词法分析采用"最大匹配"（Maximal Munch）原则：只要当前字符序列能构成更长的 Token，就一直读取，直到不能再继续为止。

```c
int a = x+++y;   // Token 化: int a = x ++ + y ;  (最大匹配: x + ++ + y)
                 // 而不是: int a = x + ++ y ; (不符合最大匹配)

a+++++b;         // Token 化: a ++ ++ + b
                 // 错误: a++ 的结果是右值, 不能再次 ++
```

编译器在词法阶段不会判断 `a+++++b` 是否正确——它只负责切分 Token，语法分析阶段才报错。

---

## 2.2 语法分析

### 2.2.1 任务定义

语法分析器（Parser）接收词法分析器输出的 Token 流，根据语言的**形式文法**检查 Token 序列是否符合语法规则，并构建**抽象语法树（AST）**。

### 2.2.2 上下文无关文法与 BNF

C 语言的语法由**上下文无关文法（CFG）** 定义（略微超出，因为有 typedef 名称的上下文依赖）。GCC 内部使用 BNF（Backus-Naur Form）风格的文法描述。

简化版的 C 赋值语句文法：

```
assignment_expr:
    unary_expr assignment_operator assignment_expr
    | conditional_expr

unary_expr:
    postfix_expr
    | '++' unary_expr
    | '--' unary_expr
    | unary_operator cast_expr

primary_expr:
    IDENTIFIER
    | CONSTANT
    | STRING_LITERAL
    | '(' expression ')'
```

### 2.2.3 语法分析算法

GCC 4.x 后采用**手工递归下降解析器**，这比传统的 yacc/bison 自动生成方案提供了更好的错误恢复和诊断信息。

**递归下降**的核心思路：为每个语法规则写一个解析函数，该函数调用子规则的解析函数：

```c
// 伪代码: 解析赋值表达式
ASTNode* parse_assignment_expr(void) {
    ASTNode* left = parse_unary_expr();

    if (current_token is assignment_operator) {
        Token op = consume_token();
        ASTNode* right = parse_assignment_expr();  // 右递归（C 标准要求）
        return make_binary_node(op, left, right);
    }

    return left;
}
```

### 2.2.4 运算符优先级与结合性

C 语言的运算符优先级通过文法层次隐式表达。以下是关键优先级的文法层次（由低到高）：

```
层级1 (最低): expression = assignment_expr {',' assignment_expr}
层级2:        assignment_expr
层级3:        conditional_expr ('?' ':')
层级4:        logical_or_expr ('||')
层级5:        logical_and_expr ('&&')
层级6:        inclusive_or_expr ('|')
层级7:        exclusive_or_expr ('^')
层级8:        and_expr ('&')
层级9:        equality_expr ('==' '!=')
层级10:       relational_expr ('<' '>' '<=' '>=')
层级11:       shift_expr ('<<' '>>')
层级12:       additive_expr ('+' '-')
层级13:       multiplicative_expr ('*' '/' '%')
层级14:       cast_expr ('(type)')
层级15:       unary_expr ('++' '--' '&' '*' '+' '-' '!' '~')
层级16 (最高): postfix_expr ('[]' '()' '.' '->' '++' '--')
```

这是为什么 `a + b * c` 等价于 `a + (b * c)` 而非 `(a + b) * c`——乘法在更高的层级（更紧的绑定）。

---

## 2.3 语义分析

### 2.3.1 任务定义

语义分析器检查语法正确的代码是否具有**合法语义**。它处理：

- 类型检查
- 类型转换（隐式/显式）
- 作用域解析
- 符号表构建与管理
- 常量表达式求值
- 控制流检查

### 2.3.2 符号表

符号表是每个作用域的"字典"——记录该作用域内所有已声明的标识符及其属性。

```c
// 全局作用域符号表
// ┌─────────────┬────────┬───────┬───────────┐
// │ 标识符       │ 种类    │ 类型   │ 属性       │
// ├─────────────┼────────┼───────┼───────────┤
// │ main        │ 函数    │ fn     │ 地址: TBD │
// │ g_counter   │ 变量    │ int    │ .bss, 4B  │
// │ task_create │ 函数    │ fn     │ 地址: TBD │
// └─────────────┴────────┴───────┴───────────┘
```

GCC 使用基于哈希表的符号表，每个作用域（全局、函数、块）对应一个独立的符号表实例。内层作用域的符号表包含指向外层符号表的指针（实现作用域链）。

### 2.3.3 类型检查

```c
int a = 42;
float b = 3.14;
int c = a + b;    // a (int) + b (float) → b 隐式转换为 int?
                  // 实际: C 的算术转换规则 → a 转为 float → float + float
                  // → 结果 float → 赋值给 int → 截断小数部分
```

C 语言的类型系统相对宽松（弱类型），允许大量隐式类型转换。编译器在语义分析阶段插入转换操作——这些操作在后续的 IR 生成阶段体现为显式转换指令。

**Usual Arithmetic Conversions（常规算术转换）**：

```
若任一操作数为 long double → 另一操作数转为 long double
否则, 若任一操作数为 double → 另一操作数转为 double
否则, 若任一操作数为 float → 另一操作数转为 float
否则, 执行 integer promotion:
  若任一操作数为 unsigned long → 另一操作数转为 unsigned long
  否则, 若任一操作数为 long → 另一操作数转为 long
  否则, ...
```

### 2.3.4 作用域规则

C 语言有五种作用域：

| 作用域类型 | 范围 | 示例 |
|-----------|------|------|
| 文件作用域 | 整个翻译单元 | 全局变量、函数定义 |
| 函数作用域 | 整个函数体 | 标签（label） |
| 块作用域 | 一对 `{}` 之内 | 局部变量、函数参数 |
| 函数原型作用域 | 函数声明参数列表内 | `void f(int x, int y)` 中的 x,y |
| 文件作用域+static | 当前翻译单元内部 | `static int count;` |

**名称遮蔽（Name Shadowing）**：

```c
int count = 10;          // 外层变量

void func(void) {
    int count = 20;      // 遮蔽外层 count
    printf("%d", count); // 打印 20（内层可见）
}
```

符号表在处理名称遮蔽时采用覆盖策略：内层符号表加入同名条目，遮蔽外层的同名符号。当离开内层作用域时，符号表回退到外层状态。

---

## 2.4 抽象语法树的构建与遍历

### 2.4.1 AST 的结构

以一个简单的赋值语句为例：

```c
arr[i] = x + y * 2;
```

对应的 AST 可能表示为：

```
                '='
               /   \
           '[]'    '+'
          /   \   /   \
       arr    i  x    '*'
                     /   \
                    y     2
```

GCC 的 AST 节点是 `tree` 类型的联合体，包含操作码、类型信息、子节点指针和源码位置信息。

### 2.4.2 AST 的中间表示转换

AST 构建完成后，GCC 进入 **GENERIC → GIMPLE** 的降级过程：

1. **GENERIC**：语言无关的 AST 表示（所有 GCC 前端共享同一格式）
2. **GIMPLE**：简化的三地址码形式（所有后续优化在其上执行）

这一步是编译前端的最终输出——GENERIC → GIMPLE 降级完成后，代码的控制流和数据流被显式展平，为优化器提供了统一的入口。

---

## 2.5 中间表示生成

### 2.5.1 为什么需要中间表示

中间表示（IR）是连接"语言相关的前端"和"目标相关的后端"的桥梁。GCC 支持 C、C++、Fortran、Ada、Go 等多种语言前端，和 ARM、x86、RISC-V、MIPS 等多种后端——所有前后端组合通过统一的 IR （GIMPLE 和 RTL）解耦。

```
   C 前端 ─┐
 C++ 前端 ─┤
Fortran ──┼──→ GIMPLE (语言无关 IR) ──→ RTL (接近硬件的 IR) ──→ ARM 后端
  Ada ────┤                                                       x86 后端
   Go ────┘                                                       ...
```

### 2.5.2 GIMPLE 的特征

GIMPLE 是 GCC 的三地址码 IR，每条指令最多包含三个操作数：

```
t1 = y * 2        // 临时变量 = 操作数 op 操作数
t2 = x + t1       // 每次最多一个操作
arr[i] = t2       // 赋值
```

GIMPLE 的约束：
- 每个 GIMPLE 语句都是一个控制流节点
- 最多两个输入操作数、一个输出操作数
- 函数调用是一个操作数（可以有多个参数）
- 条件分支和无条件跳转是显式语句

---

## 2.6 GCC GIMPLE 三地址码

### 2.6.1 一个完整示例

```c
// C 源
int max(int a, int b) {
    if (a > b)
        return a;
    else
        return b;
}
```

降级为 GIMPLE：

```
max (int a, int b)
{
  <bb 2> :
  if (a_2(D) > b_3(D))
    goto <bb 3>;
  else
    goto <bb 4>;

  <bb 3> :
  <retval> = a_2(D);
  goto <bb 5>;

  <bb 4> :
  <retval> = b_3(D);
  goto <bb 5>;

  <bb 5> :
  return <retval>;
}
```

观察：
1. 变量名添加了后缀 `_2`、`_3`——这是 SSA 版本的编号（静态单赋值形式）。
2. `(D)` 后缀表示"默认定义"——函数入口参数。
3. `bb 2/3/4/5` 是**基本块**编号——基本块是一段顺序执行的代码序列，只有一条入口和一条出口。
4. 条件语句 `if (a > b)` 被展开为条件跳转 `if ... goto`。

### 2.6.2 基本块与控制流图

**基本块**是编译器优化的基本单位，具有以下性质：
- 只有第一个语句可以是从外部跳转的目标
- 只有最后一个语句可以发生跳转
- 内部所有语句按顺序执行，无分支

多个基本块通过跳转边组成**控制流图（CFG）**。几乎所有的全局优化（死代码消除、循环不变式外提、常量传播）都基于 CFG 执行。

---

## 2.7 编译优化

GCC 在 `-O1`、`-O2`、`-O3` 和 `-Os` 级别下执行数百个优化 Pass。以下挑选嵌入式开发中最具影响力的优化类型，逐一深入讲解。

### 2.7.1 常量折叠与常量传播

**常量折叠（Constant Folding）**：编译期计算常量表达式的值。

```c
int x = 5 + 3 * 4;    // → int x = 17;
int y = sizeof(int) * CHAR_BIT;  // → int y = 32;
```

**常量传播（Constant Propagation）**：将常量变量的值传播到所有使用点。

```c
int a = 10;
int b = a + 5;    // → int b = 15;  (a 的值被传播)
int c = b * 2;    // → int c = 30;  (b 的值也被识别为常量)
```

这两项优化结合可以将大段代码缩减为常数——对于 Flash 空间紧凑的 MCU，这意味着节省大量不需要的运行时计算。

### 2.7.2 死代码消除

```c
if (0) {
    dangerous_call();  // 这段代码永远不会执行 → 整体删除
}

int x = compute_value();
return 0;              // x 的值不再被使用 → compute_value() 的调用可以删除
```

死代码消除基于**活跃变量分析**：一个变量在定义后，从该定义点到任何使用点之间存在可达路径，则该定义是"活跃"的；若定义后无任何可达的使用点，则该定义是死的，可以安全删除。

### 2.7.3 公共子表达式消除

```c
// 优化前
a = x * y + z;
b = x * y - w;

// 优化后 (CSE)
t = x * y;
a = t + z;
b = t - w;
```

`x * y` 被识别为公共子表达式，仅计算一次。CSE 基于可用表达式分析——这是一个数据流分析问题，检查在 Control Flow Graph 上的每个点哪些表达式已经被计算且计算以来的操作数未被修改。

### 2.7.4 函数内联

```c
static inline uint32_t queue_next_index(uint32_t idx, uint32_t cap) {
    return (idx + 1U) >= cap ? 0U : (idx + 1U);
}

uint32_t next = queue_next_index(current, MAX_QUEUE_ITEMS);
// 内联后: uint32_t next = (current + 1U) >= 16 ? 0U : (current + 1U);
// 如果 cap=16, 编译器可以进一步优化条件表达式为特定序列
```

GCC 在内联决策中权衡多个因素：
- 函数体大小（过大函数不内联，以免代码膨胀）
- 调用次数（仅调用一次的函数强烈倾向于内联）
- `inline` 关键字（提示但不强制）
- `-O3` 模式下更激进的内联

在 Cortex-M 上，一次函数调用的开销约 6 cycles（`BL` + `BX LR` + 栈空间），而内联后可能只需 2 cycles。对于热路径上的小型函数（如 `queue_next_index`、`find_highest_priority`），内联的价值非常显著。

### 2.7.5 循环优化

**循环展开（Loop Unrolling）**：

```c
// 优化前
for (int i = 0; i < 100; i++) {
    arr[i] = 0;
}

// 优化后 (4 倍展开)
for (int i = 0; i < 100; i += 4) {
    arr[i]   = 0;
    arr[i+1] = 0;
    arr[i+2] = 0;
    arr[i+3] = 0;
}
// 循环条件检查从 100 次降为 25 次
// Cortex-M 上每次条件判断+分支 ~3 cycles → 节省 ~225 cycles
```

**循环不变量外提（Loop Invariant Code Motion）**：

```c
// 优化前
for (int i = 0; i < n; i++) {
    arr[i] = base + offset;  // base + offset 在循环中不变
}

// 优化后
int t = base + offset;
for (int i = 0; i < n; i++) {
    arr[i] = t;
}
```

**强度削弱（Strength Reduction）**：

```c
// 优化前
for (int i = 0; i < n; i++) {
    arr[i * 4] = 0;  // 乘法是"强"运算
}

// 优化后
int *p = arr;
for (int i = 0; i < n; i++) {
    *p = 0;
    p += 4;  // 加法替代乘法
}
```

### 2.7.6 尾调用优化

当函数的最后一条语句是对另一个函数的调用时，调用者的栈帧不再需要——可以直接复用：

```c
int bar(int x) {
    return foo(x + 1);  // 尾调用: 无需保存 bar 的栈帧
}

// 优化后 (等价于):
int bar(int x) {
    // 直接跳转到 foo, 不复用 bar 的栈帧
    // 等价于 goto foo (不是真正的调用)
}
```

在嵌入式系统中，尾调用优化可以显著减少递归函数的栈深度。但在 RTOS 中应谨慎——尾调用可能使调用栈回溯变得困难。

---

## 2.8 SSA 形式与全局优化

### 2.8.1 静态单赋值形式

SSA（Static Single Assignment）是 GCC 中绝大多数全局优化的基础设施。在 SSA 形式中，每个变量只被赋值一次（静态地）。当控制流合并时需要新的版本号，使用 **Φ 函数**（phi function）选择值：

```c
// 原始代码
if (cond)
    x = 10;
else
    x = 20;
y = x + 5;

// SSA 形式
if (cond)
    x1 = 10;
else
    x2 = 20;
x3 = Φ(x1, x2);   // 根据控制流来源选择 x1 或 x2
y1 = x3 + 5;
```

SSA 形式简化了所有数据流分析——只需查找变量名（如在 SSA 中每个变量仅定义一次）即可完成 use-def 链。

### 2.8.2 全局值编号

全局值编号（GVN）在 SSA 形式的基础上识别语义上等价的表达式：

```c
a = x + y;
b = x + y;  // → b = a;
```

GVN 不关心表达式是否字面相同——它通过散列+比较确定两个表达式在所有可能输入下产生相同结果。

### 2.8.3 条件常量传播

结合常量传播和死代码消除，能识别仅在特定条件下为常量的值：

```c
if (flag) {
    a = 1;
} else {
    a = 2;
}
if (a == 1) {
    // → 只有在 flag 为 true 时才到达此处
    // 编译器可以在此分支内将 flag 替换为 true
    do_something();
}
```

---

## 2.9 寄存器分配与指令选择

### 2.9.1 从 SSA 到 RTL

GIMPLE（树型 IR + SSA）在高级优化完成后降级为 **RTL（Register Transfer Language）**——一种接近指令集架构的线性 IR。

RTL 显式表达寄存器、内存和立即数之间的数据流：

```
(insn 5 4 6 (set (reg:SI 120)           // r120 = a + b
        (plus:SI (reg/v:SI 115 [a])
                 (reg/v:SI 116 [b]))) -1)
```

### 2.9.2 寄存器分配

**问题定义**：将 IR 中无限个虚拟寄存器映射到目标平台有限个物理寄存器上。若同时活跃的虚拟寄存器数超过物理寄存器数，需将部分值**溢出（spill）**到栈上。

现代编译器的寄存器分配策略基于**图着色算法**：
1. 构建**干涉图**（Interference Graph）——每个节点是一个虚拟寄存器，若两个虚拟寄存器同时活跃则在它们之间连边；
2. 用目标平台的物理寄存器数种颜色对图进行着色；
3. 同色节点使用同一个物理寄存器；
4. 若无法着色，选择某些节点溢出到栈上，简化图后重新着色。

在 Cortex-M4 上，通用寄存器 R0-R12 + LR 共 14 个可用（R13=SP、R15=PC 不可用于通用计算），若含 FPU 还有 S0-S31。分配器需要在寄存器压力和栈溢出之间找到最优平衡。

### 2.9.3 指令选择

指令选择将 RTL 操作映射为目标平台的具体指令。它基于**树模式匹配**——将 RTL 的表达式树与目标描述文件（`.md` 文件，Machine Description）中的指令模板进行匹配。

```lisp
;; ARM 目标描述片段 (arm.md)
(define_insn "*arm_addsi3"
  [(set (match_operand:SI 0 "register_operand" "=r,r")
        (plus:SI (match_operand:SI 1 "register_operand" "%r,r")
                 (match_operand:SI 2 "arm_add_operand" "rI,L")))]
  "TARGET_32BIT"
  "add%?\\t%0, %1, %2"
)
```

这告诉编译器：加法操作 `reg = reg + (reg | imm)` 可以匹配 ARM 的 `ADD` 指令（当操作数为寄存器或常量立即数时）。`%?` 表示根据上下文生成条件执行后缀（如 `ADDNE`）。

### 2.9.4 指令调度

在指令选择之后，大多数现代编译器执行**指令调度**——调整指令顺序以：
1. 减少流水线停顿（数据依赖导致的 RAW 冒险）
2. 利用双发射能力（Cortex-M7 可同时发射两条指令到不同执行单元）
3. 减少寄存器压力峰值

```asm
; 调度前 (LDR→ADD 之间有使用依赖, 产生气泡)
LDR  r0, [r1, #4]     ; cycle 1
ADD  r0, r0, #1       ; cycle 2 (等待 r0) → 气泡

; 调度后 (插入不依赖的操作)
LDR  r0, [r1, #4]     ; cycle 1
LDR  r2, [r3, #8]     ; cycle 2 (不依赖 r0, 可执行)
ADD  r0, r0, #1       ; cycle 3 (r0 就绪)
```

---

## 2.10 目标代码生成

编译阶段的最终步骤是**将优化后的 RTL 翻译为目标汇编语言**。

### 2.10.1 汇编输出

GCC 的后端遍历 RTL insn 链，为每条 insn 调用对应的输出模板，生成汇编文本：

```
; C:  if (bitmap == 0) return MAX; else return __builtin_clz(bitmap);

; RTL:
; (set (reg:SI 120)
;      (if_then_else:SI (eq (reg/v:SI 115) (const_int 0))
;                       (const_int 32)
;                       (clz:SI (reg/v:SI 115))))

; 输出的 ARM Thumb-2 汇编
    CBZ  r0, .L1       ; 若 r0==0, 跳转到 .L1
    CLZ  r0, r0        ; CLZ 硬件指令
    BX   lr            ; 返回
.L1:
    MOV  r0, #32       ; 返回 32 (MAX_PRIORITIES)
    BX   lr
```

### 2.10.2 条件执行

ARM 指令集广泛使用条件执行——大多数指令可以在前面附加条件码，避免短跳转：

```c
// C:  if (x > 0) a = 1; else a = 2;

// 无优化 (含跳转):
    CMP  r0, #0
    BLE  .L2
    MOV  r1, #1       ; a = 1
    B    .L3
.L2:
    MOV  r1, #2       ; a = 2
.L3:

// 优化后 (条件执行):
    CMP  r0, #0
    ITE  GT            ; If-Then-Else: GT
    MOVGT r1, #1       ;    then: a = 1
    MOVLE r1, #2       ;    else: a = 2
```

条件执行消除了分支跳转（以及随之而来的流水线刷新），在 Cortex-M4 上节省 ~3 cycles，在 M7 上也节省了分支预测器资源。

---

## 2.11 编译阶段与汇编阶段的接口

编译阶段结束的标志是 `.s` 文件（汇编源文件）的生成。这个文件是纯文本，每一行是一个汇编指令、标签、伪指令或注释。汇编器可以完全独立于编译器运行——编译器输出的汇编与人类手写的汇编在格式上完全一致。

```
// 连接方式
arm-none-eabi-gcc -S -O2 main.c -o main.s      # 输出汇编文件
arm-none-eabi-as main.s -o main.o               # 手动汇编
arm-none-eabi-gcc -c -O2 main.c -o main.o       # 一步完成编译+汇编
```

---

# 第三章：汇编——从助记符到机器码

汇编阶段将人类可读的汇编语言转化为目标处理器可执行的二进制机器码，存于 ELF 目标文件（`.o` 文件）中。汇编器（GCC 中为 `as`，即 GNU Assembler）的工作相比于编译阶段来说相当机械化，但其精确性至关重要——每个 bit 的编码必须严格符合指令集规范。

---

## 3.1 ARM Thumb-2 指令集架构概要

### 3.1.1 Thumb-2 的混合编码

ARM Cortex-M 处理器仅执行 Thumb 模式指令（不支持 32-bit ARM 模式）。Thumb-2 是指令集的扩展版本，融合了 16-bit 和 32-bit 指令：

- **Thumb (16-bit)**：紧凑编码，大多数常用指令可用。寄存器范围限于 R0-R7（低寄存器），立即数范围窄。
- **Thumb-2 (32-bit)**：功能丰富——全寄存器范围（R0-R15）、更大立即数、更多运算种类、条件执行更灵活。

汇编器自动选择最短的编码。程序员通常无需关心 16-bit 还是 32-bit，但了解两者的边界对性能优化很重要。

```asm
; 这些是 16-bit 指令（各 2 字节）:
MOV  r0, r1          ; 0x4608
ADD  r0, r0, #1      ; 0x1C40

; 这些必须是 32-bit 指令（各 4 字节）:
MOV  r0, #0x100      ; 0xF04F  (MOVW 立即数超过 8-bit)
MOV  r8, r9          ; 0xEA4F  (高寄存器操作)
LDR  r0, =0x20000000 ; 0x4800 + 字面量池 (PC 相对加载)
```

### 3.1.2 统一汇编语法

GAS（GNU Assembler）的双模式语法可能令人困惑。ARM 原本有两种汇编语法：

- `.arm` 模式（32-bit ARM 指令，Cortex-M 不支持）
- `.thumb` 模式（传统 Thumb 语法）

从 GCC 4.x 开始，GAS 支持 `.syntax unified`——统一语法允许在 Thumb 代码中使用 ARM 风格的助记符格式：

```
.syntax unified
.cpu cortex-m4
.thumb

; 传统分割语法 (不推荐):
; ADD R0, R1       ← .text 32 模式, ADD 是 ARM 指令
; .thumb
; ADD R0, R1       ← 不同含义! 此时是 Thumb 指令

; 统一语法 (推荐):
ADD  r0, r1          ; 汇编器根据目标 CPU 选择 Thumb 编码
ADD.W r0, r1         ; 强制选择 32-bit Thumb-2 编码
ADD.N r0, r1         ; 强制选择 16-bit Thumb 编码
```

本项目所有汇编文件均使用 `.syntax unified`（详见 [port.c](file:///e:/STM32_Project/CubeIDE/f446_rtos/RTOS/Port/GCC/ARM_CM7/port.c) 中的内联汇编），确保指令行为在不同模式下一致。

---

## 3.2 汇编器的两遍扫描

汇编器采用两遍扫描策略解决**前向引用**问题——标签可能在定义之前就被引用。

### 第一遍

1. 确定每个段的大小
2. 收集所有符号及其偏移量
3. 计算每条指令的长度（16-bit 还是 32-bit），确定后续符号偏移
4. 暂不生成机器码（编码需要知道标签的最终地址）

### 第二遍

1. 使用第一遍收集的符号地址完成指令编码
2. 插入字面量池
3. 生成重定位条目
4. 输出目标文件

**两遍扫描的必要性示例**：

```asm
    B    forward_label     ; 第一遍: 记录"forward_label"是未定义符号
                           ; 第二遍: 已知 forward_label 在 12 字节后, 编码跳转偏移
    MOV  r0, #1
    MOV  r1, #2
forward_label:             ; 第一遍: 记录此标签在段的偏移
    ADD  r0, r0, r1
```

---

## 3.3 指令编码

### 3.3.1 Thumb-2 编码规则

每条 Thumb-2 指令的 16-bit 或 32-bit 编码有严格固定的位域布局。以下以几个典型指令为例：

**16-bit `MOV Rd, Rm`** （低寄存器 R0-R7）：

```
bits: 15-14  13-12  11-10  9-8   7-6   5-4   3-2  1-0
      01      00     0      0     1     0     Rd    Rm

; MOV r0, r1 → 0100 0010 0000 1000 → 0x4608
```

**16-bit `ADD Rd, Rn, #imm3`** （3-bit 立即数）：

```
bits: 15-14  13-12  11-10  9-8   7-6   5-4   3-2  1-0
      00      0      1      imm3  Rn    Rd

; ADD r0, r0, #1 → 0001 1100 0100 0000 → 0x1C40
```

**32-bit Thumb-2 BL 指令**：

BL 是 ARM 汇编中最复杂的编码之一。它有 21-bit 有符号偏移，拆分为两半：

```
上半字 (H): 1111 0 S J1 J2
下半字 (L): 1 0 1 1 J1 J2 imm10
最终跳转偏移 = SignExtend(S:J1:J2:imm6:imm11:0)  (23-bit)

; J1 = NOT(offset[22] XOR S)
; J2 = NOT(offset[21] XOR S)
```

这种拆分为两个非连续性字段的编码设计是为了兼容早期的 Thumb 指令集，同时也保证了编码密度和跳转范围（±16MB）的平衡。

### 3.3.2 立即数的编码约束

ARM 的立即数编码采用 8-bit 常数 + 偶数位旋转的格式：

```asm
MOV  r0, #0xFF       ; ✓ 8-bit 以内
MOV  r0, #0x100      ; ✗ 超过 8-bit → 需要用 MOVW (32-bit Thumb-2)
MOV  r0, #0xFF000000 ; ✓ 旋转右移 24 位 (= 8-bit 常数 0xFF 旋转)
```

汇编器会自动选择最合适的指令序列——若立即数不能直接编码，会用 `MOVW + MOVT` 组合或 `LDR =const`（PC 相对加载）。程序员在大多数情况下不需要关心编码细节，但在需要精确控制代码大小时（如在 2KB bootloader 中），理解立即数约束至关重要。

---

## 3.4 伪指令与宏

### 3.4.1 常用伪指令

伪指令（Directive）不是 CPU 指令，而是告诉汇编器**做什么**的元指令：

```asm
.syntax unified              ; 使用统一汇编语法
.cpu cortex-m4               ; 指定目标 CPU
.fpu fpv4-sp-d16             ; 指定 FPU 类型
.thumb                       ; 生成 Thumb 指令

.global Reset_Handler        ; 导出符号, 供链接器使用
.weak   Default_Handler      ; 弱符号, 可被覆盖
.type   Reset_Handler, %function  ; 声明符号类型

.section .isr_vector, "a"    ; 将后续内容放入 .isr_vector 段
.align  2                    ; 按 4 字节对齐

.word   _estack              ; 放置一个 32-bit 值 (栈顶地址)
.space  256                  ; 保留 256 字节空间

.thumb_func                  ; 声明下一个符号是 Thumb 函数 (位 0 的 1 标记)
```

### 3.4.2 汇编宏

汇编器支持基于文本替换的宏（与 C 宏类似）：

```asm
.macro PUSH_CONTEXT base_reg
    STMDB \base_reg!, {r4-r11}
    VSTMDBEQ \base_reg!, {s16-s31}
.endm

; 使用: PUSH_CONTEXT r0  → 展开为 STMDB r0!, {r4-r11} + VSTMDBEQ ...
```

在本项目中，汇编宏可以用来封装重复的上下文保存模式，减少代码冗余，但不建议过度使用——内联汇编中的宏容易与 C 宏产生混淆。

---

## 3.5 字面量池管理

### 3.5.1 什么是字面量池

当 ARM 指令无法直接用立即数编码一个 32-bit 常数时，编译器/汇编器将常数存放在代码附近的**字面量池**中，用 PC 相对加载指令读取：

```asm
; 源汇编:
    LDR  r0, =0x20000000   ; 加载 SRAM 基地址

; 汇编器自动展开:
    LDR  r0, [pc, #8]      ; PC 相对加载 (偏移在指令的 8-bit imm 中)
    BX   lr
    .ltorg                  ; 插入字面量池
    .word 0x20000000        ; 常数值在此
```

`LDR r0, [pc, #8]` 的偏移是字对齐的（乘以 4），即实际偏移 32 字节（8 × 4）。PC 当前值为 `LDR` 指令地址 + 4。计算过程：`0x08000100 + 4 + 32 = 0x08000124`，如果 `.word 0x20000000` 恰好在地址 0x08000124 处，加载成功。

### 3.5.2 `.ltorg` 的手动控制

在需要精确控制字面量池位置（如异常返回指令之后不能有数据）的场景中，必须手动插入 `.ltorg`：

```asm
" cpsie i                     \n"  ; 开启中断
" dsb                         \n"  ; 数据同步屏障
" isb                         \n"  ; 指令同步屏障
" svc 0                       \n"  ; 触发 SVC 异常
" .ltorg                      \n"  ; 在此处插入字面量池 (SVC 之后不会执行)
```

本项目在 PendSV handler 和启动代码中多处使用 `.ltorg` 避免字面量池污染执行路径。如果 `.ltorg` 被错误地放置在 `BX lr`（异常返回）之后，CPU 可能将字面量池的常数数据作为指令执行——几乎必然触发 HardFault。

---

## 3.6 符号表与重定位条目

### 3.6.1 符号的分类

目标文件中的符号有三种状态：

| 状态 | 说明 | 示例 |
|------|------|------|
| 已定义 | 符号的地址/值在本文件中确定 | `Reset_Handler:`、全局变量定义 |
| 未定义 | 引用外部符号，地址未知 | `extern int g_counter;`、`BL printf` |
| 绝对 | 符号的值是常数，不依赖地址 | 通过 `.equ` 定义的常量 |

每个符号在 ELF 的 `.symtab` 段中占据一条记录：

```
符号表条目结构:
  - st_name:  符号名 (字符串表索引)
  - st_value: 符号值 (地址或常量)
  - st_size:  符号占用的字节数
  - st_info:  绑定属性 (STB_LOCAL/STB_GLOBAL/STB_WEAK) + 类型 (STT_FUNC/STT_OBJECT/...)
  - st_shndx: 所属段的索引 (SHN_UNDEF 表示未定义)
```

### 3.6.2 重定位条目

当指令或数据引用了地址未知的符号时，汇编器生成重定位条目而非尝试填入最终地址。每个需修补的位置对应一条重定位条目：

```
重定位条目结构:
  - r_offset:   需要修补的位置在段内的偏移
  - r_info:     (符号表索引 << 8) | 重定位类型
  - r_addend:   加数 (REL 类型) 或隐含在指令中 (RELA 类型)
```

ARM 的常用重定位类型（见后文 4.6 节）在汇编完成后被写入 `.rel.text`、`.rel.data` 等重定位段中。

---

## 3.7 ELF 目标文件格式

### 3.7.1 ELF 整体结构

ELF（Executable and Linkable Format）是 Unix/Linux 及嵌入式系统的标准目标文件格式。一个 ELF 目标文件结构如下：

```
┌─────────────────────────┐
│ ELF Header               │  ← 魔数(0x7F 'E' 'L' 'F')、32/64位、字节序、目标架构
├─────────────────────────┤
│ Section Header Table     │  ← 每个段的名称、类型、地址、大小
├─────────────────────────┤
│ .text                    │  ← 代码段 (机器指令)
├─────────────────────────┤
│ .data                    │  ← 已初始化数据段
├─────────────────────────┤
│ .bss                     │  ← 未初始化数据段 (文件中的大小, 不占空间)
├─────────────────────────┤
│ .rodata                  │  ← 只读数据段 (常量、字符串字面量)
├─────────────────────────┤
│ .symtab                  │  ← 符号表
├─────────────────────────┤
│ .rel.text                │  ← .text 的重定位条目
├─────────────────────────┤
│ .rel.data                │  ← .data 的重定位条目
├─────────────────────────┤
│ .debug_*                 │  ← DWARF 调试信息
├─────────────────────────┤
│ .comment                 │  ← 编译器版本标识
├─────────────────────────┤
│ .shstrtab                │  ← 段名称字符串表
├─────────────────────────┤
│ .strtab                  │  ← 符号名称字符串表
└─────────────────────────┘
```

### 3.7.2 ELF 头解析

```bash
arm-none-eabi-readelf -h rtos_sched.o
```

输出示例：

```
ELF Header:
  Magic:   7f 45 4c 46 01 01 01 00 00 00 00 00 00 00 00 00
  Class:                             ELF32
  Data:                              2's complement, little endian
  Version:                           1 (current)
  OS/ABI:                            UNIX - System V
  Type:                              REL (Relocatable file)
  Machine:                           ARM
  Flags:                             0x5000400, Version5 EABI, hard-float ABI
```

关键信息：
- `Class: ELF32` → 32-bit 地址
- `Machine: ARM` → ARM 目标平台
- `Type: REL` → 可重定位文件（待链接）
- `Flags: hard-float ABI` → 使用硬件浮点调用约定

### 3.7.3 段表解析

```bash
arm-none-eabi-readelf -S rtos_sched.o
```

输出展示每个段的大小和偏移：

```
Section Headers:
  [Nr] Name              Type            Addr     Off    Size   ES Flg Lk Inf Al
  [ 0]                   NULL            00000000 000000 000000 00      0   0  0
  [ 1] .text             PROGBITS        00000000 000034 0003c8 00  AX  0   0  4
  [ 2] .data             PROGBITS        00000000 0003fc 000000 00  WA  0   0  1
  [ 3] .bss              NOBITS          00000000 0003fc 000028 00  WA  0   0  4
  [ 4] .rodata           PROGBITS        00000000 0003fc 000068 00   A  0   0  4
  [ 5] .comment          PROGBITS        00000000 000464 00004f 01  MS  0   0  1
  [ 6] .ARM.attributes   ARM_ATTRIBUTES  00000000 0004b3 000030 00      0   0  1
  [ 7] .symtab           SYMTAB          00000000 0004e4 0002e0 10      8  38  4
  [ 8] .strtab           STRTAB          00000000 0007c4 0001ae 00      0   0  1
```

观察要点：
- `.text` 段大小为 `0x3C8` (968 字节)，标志 `AX`（可分配 + 可执行）
- `.bss` 段类型为 `NOBITS`——在文件中不占空间（仅 `Size` 有效），运行时在 SRAM 中分配
- `.rodata` 段包含只读常量（如字符串），标志 `A`（可分配）

---

## 3.8 调试信息的嵌入

### 3.8.1 DWARF 调试格式

GCC 通过 `-g` 选项在 ELF 中嵌入 **DWARF（Debug With Arbitrary Record Format）** 调试信息。DWARF 是一个与平台和语言无关的调试数据格式，在嵌入式开发中由 GDB/IDE 使用。

### 3.8.2 调试信息的组成

DWARF 在 ELF 中分解为多个段：

| 段名称 | 内容 |
|--------|------|
| `.debug_info` | 所有调试条目（变量、类型、函数、作用域） |
| `.debug_abbrev` | 调试条目的缩写格式 |
| `.debug_line` | 指令地址到源文件行号的映射 |
| `.debug_frame` | 调用帧信息（CFI），用于栈回溯 |
| `.debug_aranges` | 地址范围到编译单元的快速查找表 |
| `.debug_str` | 所有字符串（文件名、标识符名） |

### 3.8.3 调试信息对代码的影响

**关键事实**：调试信息仅存在于 ELF 文件中。当通过 `arm-none-eabi-objcopy -O binary` 生成 `.bin` 烧录文件时，`.debug_*` 段全部被丢弃，**不会占用 Flash 空间**，也不消耗任何运行时的 CPU 周期。这意味着你可以放心使用 `-g -O2` 组合——获得全优化的性能 + 完整的源码级调试能力。

```bash
# 生成带调试信息的 ELF：
arm-none-eabi-gcc -g -O2 -o firmware.elf main.c ...

# 提取纯机器码 (调试信息自动丢弃)：
arm-none-eabi-objcopy -O binary firmware.elf firmware.bin

# 文件大小对比：
# firmware.elf:  ~450KB (含调试信息)
# firmware.bin:  ~32KB  (纯机器码)
```

---

## 3.9 汇编器输出与链接阶段的接口

汇编阶段的最终产物是**可重定位的 ELF 目标文件**（`.o` 文件）。这个文件传递给链接器时，链接器从中提取以下核心信息：

1. **代码和数据**：`.text`、`.data`、`.rodata` 段的内容
2. **符号表**：哪些符号是本文件定义的，哪些是引用的外部符号
3. **重定位表**：哪些指令/数据位置需要在链接时修补
4. **段大小信息**（用于内存布局计算）

汇编器不需要关心链接器如何合并段或分配最终地址——它只负责将本翻译单元的汇编代码精确翻译为机器码，并将所有未解析的地址留空给链接器处理。

---

# 第四章：链接——将零件组装成完整程序

链接是编译流水线的最后一道工序。操作系统平台的程序链接通常在运行时由动态加载器完成，但在裸机嵌入式系统中，链接完全在编译时执行（静态链接）。链接器（GNU ld）接收所有目标文件（`.o`）和库文件（`.a`），生成一个可直接烧录到 Flash 的完整可执行文件。

---

## 4.1 链接器的总体工作流

链接器的处理步骤可概括为六个操作：

1. **读取所有输入文件**——解析每个 `.o` 和 `.a` 文件的 ELF 结构
2. **合并段**——根据链接脚本将同名输入段按序拼接
3. **收集所有符号**——建立全局符号表
4. **分配地址**——为每个段分配最终的虚拟地址（VMA）和加载地址（LMA）
5. **解析符号**——为每个引用找到其唯一定义
6. **修补重定位**——根据最终地址重新计算并填入偏移量

### 4.1.1 两步链接（对嵌入式而言非主流）

传统 GNU ld 支持**两步链接**（Linker Relaxation 通常在链接后阶段进行），但在静态链接的嵌入式场景中通常走单步流程：

```
输入: rtos_sched.o rtos_task.o rtos_queue.o port.o startup.o -lc -lm -lnosys
       ↓
    GNU ld
       ↓
输出: firmware.elf
       ↓
  objcopy -O binary
       ↓
    firmware.bin
```

---

## 4.2 段合并

### 4.2.1 链接脚本的"指挥权"

链接脚本是链接器的"建筑蓝图"。它为链接器精确指定每个输入段应该放在输出文件的哪个位置：

```ld
SECTIONS {
    .text : {
        *(.isr_vector)         /* 中断向量表在前 */
        *(.text)               /* 所有文件的 .text 段 */
        *(.text.*)             /* 编译器生成的子段 */
        *(.rodata)             /* 只读数据紧接代码 */
        *(.rodata.*)
    } > FLASH

    .data : {
        __data_start__ = .;    /* 记录 .data 的起始地址 */
        *(.data)
        *(.data.*)
        __data_end__ = .;      /* 记录 .data 的结束地址 */
    } > SRAM1 AT> FLASH        /* VMA=SRAM, LMA=Flash */

    .bss (NOLOAD) : {
        __bss_start__ = .;
        *(.bss)
        *(.bss.*)
        __bss_end__ = .;
    } > SRAM1
}
```

### 4.2.2 VMA vs LMA

嵌入式系统最令人困惑的概念之一：**虚拟内存地址（VMA）** 和 **加载内存地址（LMA）** 可能是不同的。

- **VMA**（Virtual Memory Address）：段在**运行时**所处的地址。`.data` 段的 VMA 在 SRAM 中（因为变量需要运行时修改）。
- **LMA**（Load Memory Address）：段在**文件/Flash 中**存放的地址。`.data` 段的 LMA 在 Flash 中（因为掉电后 Flash 不丢失数据）。

```
Flash (LMA for .data):                   SRAM (VMA for .data):
┌────────────────────┐                  ┌────────────────────┐
│ .text (代码)        │                  │                    │
│ .rodata (常量)      │                  │                    │
├────────────────────┤   启动时复制      ├────────────────────┤
│ .data 初始值的副本   │ ──────────────→  │ .data (变量)       │
├────────────────────┤   memcpy()       ├────────────────────┤
│ ...                │                  │ .bss (清零后的空间) │
│                    │                  │ 堆 ← 往上增长      │
│                    │                  │ 栈 ← 往下增长      │
└────────────────────┘                  └────────────────────┘
```

启动代码（`Reset_Handler`）负责从 LMA 到 VMA 的复制：

```c
// startup.c 中的标准模式
extern uint32_t __data_start__;
extern uint32_t __data_end__;
extern uint32_t __data_load__;  // LMA 起始 (在 Flash)

void copy_data_section(void) {
    uint32_t *src  = &__data_load__;
    uint32_t *dst  = &__data_start__;
    uint32_t *end  = &__data_end__;
    while (dst < end) {
        *dst++ = *src++;
    }
}

void zero_bss_section(void) {
    uint32_t *dst  = &__bss_start__;
    uint32_t *end  = &__bss_end__;
    while (dst < end) {
        *dst++ = 0;
    }
}
```

---

## 4.3 符号解析

### 4.3.1 全局符号的解析流程

1. 链接器从所有输入文件的符号表中收集所有符号——所有 `STB_GLOBAL` 和 `STB_WEAK` 符号；
2. 对每一个未解析的引用（`SHN_UNDEF`），在所有输入文件中搜索匹配的定义；
3. 若找到唯一定义 → 引用被解析；
4. 若找到多个强定义 → **多重定义错误**；
5. 若未找到定义 → **未定义引用错误**。

### 4.3.2 符号绑定的三种类型

| 绑定类型 | ELF 常量 | 说明 |
|---------|---------|------|
| LOCAL | `STB_LOCAL` | 仅本文件内可见（`static` 变量/函数） |
| GLOBAL | `STB_GLOBAL` | 所有文件可见，优先被解析 |
| WEAK | `STB_WEAK` | 可被同名的 GLOBAL 符号覆盖 |

**弱符号的实际应用**——本项目的中断向量表：

```asm
// startup.s 中的默认中断处理程序
.weak  SysTick_Handler
.thumb_func
SysTick_Handler:
    B  .                // 死循环——用户必须覆盖

// 用户代码中 (可选的覆盖):
void SysTick_Handler(void) {
    rtos_port_sys_tick_handler();  // 用户自定义的 SysTick 处理
}
```

链接器遇到 `SysTick_Handler` 时：
- 用户代码中的定义为 `STB_GLOBAL` → 优先选用
- startup.s 中的定义为 `STB_WEAK` → 仅在无 STB_GLOBAL 定义时使用

---

## 4.4 地址分配与链接脚本

### 4.4.1 MEMORY 命令

```ld
MEMORY {
    FLASH  (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
    SRAM1  (rwx) : ORIGIN = 0x20000000, LENGTH = 112K
    SRAM2  (rwx) : ORIGIN = 0x2001C000, LENGTH = 16K
    CCMRAM (rw)  : ORIGIN = 0x10000000, LENGTH = 64K
}
```

`(rx)` 等标志表示该内存区域的可访问属性——`r`=读, `w`=写, `x`=执行。虽然属性仅影响链接器检查（不影响硬件），但不建议在 Flash 区域放置需要写入的 `.data` 段。

### 4.4.2 地址分配算法

链接器为每个输出段分配地址时，维护一个"位置计数器"（`.`）：

```
初始: . = 0x08000000 (FLASH 起始)
.isr_vector: . = 0x08000000 → .text 段: . = 0x08000100 → ...
SRAM 段从 0x20000000 起: . = ORIGIN(SRAM1)
```

链接脚本中的 `ALIGN(4)` 确保位置计数器始终按 4 字节对齐——这对 ARM 的字访问（LDR/STR 需要 4 字节对齐）至关重要。

### 4.4.3 数据段、BSS 段和堆栈的布局

典型的 Cortex-M 内存布局：

```
Flash:                         SRAM:
┌──────────────────┐          ┌───────────────────────┐
│ .isr_vector      │          │ .data          ← ┐    │
│ .text            │          ├───────────────── ┤    │
│ .rodata          │          │ .bss               │    │
│ .data (LMA备份)  │ ─copy──→ │ .heap          ← 静态 │
│ ...              │          │   ↑               │    │
│                  │          │   │   (动态增长)   │    │
│                  │          │   ↓               │    │
│                  │          │ .stack            │    │
└──────────────────┘          └───────────────────┴────┘
```

---

## 4.5 重定位修复

重定位修复是链接过程中最关键的操作——将编译器/汇编器留下的"占位"地址替换为符号的最终地址。

### 4.5.1 重定位修复的公式

通用修复公式：

```
修补值 = 符号值 + 加数 - 计算基址
```

具体计算依赖于**重定位类型**（见下节）和所引用的指令编码。

### 4.5.2 重定位修复示例（BL 指令）

在 `.o` 文件中：

```
偏移  机器码 (修补前)      汇编
0x00  F7FF FFFE              BL 0            ← 目标未知, 占位 0
0x04  ...
```

符号 `rtos_sched_tick` 在链接后被分配到地址 `0x0800142C`。当前 BL 指令位于 `0x08001000`。当前 PC = `0x08001000 + 4 = 0x08001004`。

目标偏移 = `0x0800142C - 0x08001004 = 0x428`（+ 2 字节对齐半字偏移 `0x214`）。

ARM 的 BL 指令编码公式（经过 J1/J2 编码）：

```
S = sign_bit(offset) → 0 (正偏移)
imm10 = offset[11:2] → 0x10A
imm11 = offset[22:12] → 0x000 → offset 在这范围内

编码:
  H = 1111 0 0 1 0 1 0 imm11[10:6] → 0xF000
  L = 1 0 1 0 1 0 1 1 imm11[5:0] imm10 → 具体的 16-bit 值

输出 (修补后):
0x08001000: F004 F214     BL 0x0800142C
```

这个修补值由链接器自动计算——程序员无需（通常也不应该）手工编写重定位修复逻辑。

---

## 4.6 ARM Thumb-2 重定位类型

ARM ELF ABI 定义了数十种重定位类型。以下列出本项目中最常见的几种：

| 类型 | 值 | 公式 | 应用 |
|------|----|------|------|
| `R_ARM_ABS32` | 2 | `S + A` | `.data` 中的函数指针 |
| `R_ARM_THM_CALL` | 10 | `S + A - P` | Thumb BL 指令 |
| `R_ARM_THM_MOVW_ABS_NC` | 47 | `S + A` (低 16-bit) | MOVW 指令 |
| `R_ARM_THM_MOVT_ABS` | 48 | `S + A` (高 16-bit) | MOVT 指令 |
| `R_ARM_THM_PC22` | 10 | 与 THM_CALL 相同 | 兼容旧版 |

**符号说明**：

- `S`：符号的最终地址
- `A`：加数（addend，指令中已有的偏移）
- `P`：修补位置的地址（通常为 PC 值）

**示例**：`.data` 段中的函数指针初始化：

```c
// func_ptr.c
void (*callback)(void) = my_handler;  // 全局函数指针, 初始值需重定位

// 在 func_ptr.o 中 (重定位前):
// .data 段偏移 0x00: .word 0x00000000  (加数 = 0, 占位)
// 重定位条目: R_ARM_ABS32 → S=my_handler, A=0
// 修补值: S + A - 0 = my_handler 的最终地址

// 链接后:
// .data 段偏移 0x00: .word 0x08001345  (my_handler 的 Flash 地址)
```

---

## 4.7 弱符号与强符号

### 4.7.1 强符号 vs 弱符号

C 语言的符号默认是**强符号（Strong / STB_GLOBAL）**。弱符号（Weak / STB_WEAK）必须在声明时显式标注。

链接器的选择规则：

| 场景 | 选择结果 |
|------|---------|
| 1 个强定义 | 使用强定义 |
| 多个强定义 | **链接错误** |
| 1 个弱定义 | 使用弱定义 |
| 多个弱定义 | 使用第一个弱定义 |
| 0 个定义 | **未定义引用链接错误** |

### 4.7.2 本项目的弱符号模式

```c
// rtos_sched.c 末尾
__attribute__((weak)) void rtos_assert_fail(const char *cond,
                                             const char *file, int line) {
    (void)cond; (void)file; (void)line;
    while (1) { }
}

// 用户代码可以覆盖:
void rtos_assert_fail(const char *cond, const char *file, int line) {
    // 自定义: 记录错误日志到 EEPROM
    log_assert(cond, file, line);
    while (1) { }
}
```

链接器看到两个 `rtos_assert_fail`：
- 用户代码: `STB_GLOBAL`（强符号）
- RTOS 默认: `STB_WEAK`（弱符号）
- 结果: 选择用户版本

---

## 4.8 库文件的处理

### 4.8.1 静态库（`.a` 文件）

静态库是一组 `.o` 文件的归档（ar archive）。链接器仅从库中**提取**实际引用了其符号的目标文件：

```
libc.a:
  ┌─ printf.o
  ├─ malloc.o
  ├─ memcpy.o
  └─ strlen.o  (如果 main.c 用了 strlen, 则仅此文件被链接)

# 链接命令中库文件在目标文件之后:
arm-none-eabi-gcc main.o rtos_sched.o ... -lc -lm
```

**关键规则**：库文件必须出现在引用它的目标文件**之后**。若 `main.o` 引用了 `printf`，`-lc` 必须在 `main.o` 之后——否则链接器在处理 `libc.a` 时还不知道 `printf` 被需要，将不会提取 `printf.o`。

### 4.8.2 链接器垃圾回收

启用 `--gc-sections` 后（`-ffunction-sections -fdata-sections` + `-Wl,--gc-sections`），链接器会将每个函数和每个全局变量放在独立的段中，并在链接时删除所有未被引用的段：

```makefile
CFLAGS  += -ffunction-sections -fdata-sections
LDFLAGS += -Wl,--gc-sections
```

这使得未使用的库函数（即使在同一文件中）被完全排除。例如若某个 `.o` 文件包含 10 个函数但只用了 1 个，未用到的 9 个不会出现在最终二进制中。这项技术在嵌入式开发中几乎是**必须启用**的——Flash 空间极其宝贵。

---

## 4.9 链接时优化（LTO）

### 4.9.1 LTO 的原理

标准编译是 "每个 `.c` 单独编译为 `.o`" → 链接。LTO 改变了这一流程：

```
# 标准编译:
main.c → (cc1) → main.s → (as) → main.o
                                       ↘
                                         (ld) → firmware.elf
rtos.c → (cc1) → rtos.s → (as) → rtos.o ↗

# LTO 编译:
main.c → (cc1 + GIMPLE 写入) → main.o (含 IR)
rtos.c → (cc1 + GIMPLE 写入) → rtos.o (含 IR)
                                       ↘
                                         (ld + LTO 插件) → 读取所有 IR
                                                            → 跨文件优化
                                                            → 生成最终机器码
                                                            → firmware.elf
```

LTO 将中间表示（GIMPLE bytecode）嵌入到目标文件中，链接器调用 LTO 插件读取所有 IR 后，执行跨模块的优化。

### 4.9.2 LTO 的收益示例

```c
// file1.c
static int helper(int x) { return x * 2 + 1; }
int api1(int a)     { return helper(a); }

// file2.c
extern int api1(int);
int api2(void)      { return api1(10); }

// 无 LTO: api2 → BL api1 → BL helper → 两次函数调用
// 有 LTO: api2 → 10*2+1 = 21 → 可直接计算为常量
```

### 4.9.3 启用 LTO

```makefile
CFLAGS  += -flto -fuse-linker-plugin
LDFLAGS += -flto -fuse-linker-plugin   # 链接器也需要 LTO 插件
```

**注意**：LTO 大幅增加编译时间（2-3 倍），且某些旧版 GCC 的 LTO 在 ARM 平台上有已知 bug。建议仅在稳定版本中使用。

---

## 4.10 最终输出与启动流程

### 4.10.1 firmware.elf 的内容

链接器生成的 ELF 文件包含了最终确定地址的完整程序：

```bash
arm-none-eabi-readelf -l firmware.elf
```

输出展示**程序头**（Program Headers）——定义了哪些段需要加载到内存：

```
Program Headers:
  Type           Offset   VirtAddr   PhysAddr   FileSiz  MemSiz   Flg Align
  LOAD           0x001000 0x08000000 0x08000000 0x01abc  0x01abc  RWE 0x10000
  LOAD           0x003000 0x20000000 0x08001abc  0x00200  0x00300  RW  0x10000
```

`LOAD` 类型的段是 `objcopy -O binary` 提取的内容。第一条 LOAD 段对应 Flash 中的代码和常量，第二条 LOAD 段对应 SRAM 中的数据区域。

### 4.10.2 .bin 文件的生成

```bash
arm-none-eabi-objcopy -O binary firmware.elf firmware.bin
```

`objcopy` 读取 ELF 文件中的所有 `LOAD` 段，按照 VirtAddr 排序后将 FileSiz 指定的数据范围拼接为连续的二进制块。这就是直接烧录到 MCU Flash 的文件。

### 4.10.3 从复位到 main() 的完整启动序列

```
1. 上电/复位 → CPU 取 SP 和 PC
   ├─ SP = *(uint32_t *)0x08000000  → 设为栈顶地址
   └─ PC = *(uint32_t *)0x08000004  → Reset_Handler 地址

2. Reset_Handler (汇编部分):
   ├─ 初始化 .data 段 (memcpy LMA→VMA)
   └─ 清零 .bss 段 (memset VMA 为 0)

3. Reset_Handler (C 部分):
   ├─ SystemInit() — 配置 PLL、Flash 等待周期、外设时钟
   ├─ __libc_init_array() — 执行 C++ 全局构造函数
   └─ main()

4. main():
   ├─ rtos_init() — 初始化调度器、堆、空闲任务
   ├─ rtos_task_create() — 创建用户任务
   └─ rtos_sched_start()
       └─ rtos_port_start_first_task()
           ├─ 设置 PSP (进程栈指针)
           ├─ 恢复第一个任务的栈帧 (R4-R11, LR, 可选 FPU 上下文)
           └─ BX LR — 跳转到第一个用户任务
```

至此，编译流水线的最终产物——一段 ARM Thumb-2 机器码——开始在 MCU 上执行，源代码中的每一行逻辑最终转化为了具体的寄存器和内存操作。

---

# 附录：命令速查与扩展阅读

## A.1 各阶段的手动调用命令

```bash
# 1. 预处理 (输出 .i 文件):
arm-none-eabi-gcc -E -o main.i main.c -I./RTOS/Core/Inc

# 2. 编译 (输出 .s 汇编文件):
arm-none-eabi-gcc -S -O2 -o main.s main.c -I./RTOS/Core/Inc

# 3. 汇编 (输出 .o 目标文件):
arm-none-eabi-as -o main.o main.s

# 4. 链接 (输出 .elf 可执行文件):
arm-none-eabi-gcc -o firmware.elf main.o rtos_sched.o ... -TSTM32F446.ld

# 5. 提取 .bin:
arm-none-eabi-objcopy -O binary firmware.elf firmware.bin

# 6. 生成反汇编 (.lst):
arm-none-eabi-objdump -d -S firmware.elf > firmware.lst

# 7. 查看段信息:
arm-none-eabi-readelf -S firmware.elf

# 8. 查看符号表:
arm-none-eabi-nm --size-sort firmware.elf | tail -20

# 9. 查看内存占用:
arm-none-eabi-size firmware.elf
```

## A.2 查看内存占用

```bash
arm-none-eabi-size firmware.elf
```

输出示例：

```
   text    data     bss     dec     hex filename
  13240     256    2048   15544    3CB8 firmware.elf
```

- `text`：Flash 占用（代码 + 常量 + 中断向量表）
- `data`：SRAM 中已初始化的变量（初始值也存在 Flash 中）
- `bss`：SRAM 中未初始化的变量（不占 Flash）
- Flash 总占用 ≈ `text + data`
- SRAM 静态占用 ≈ `data + bss`

## A.3 关键术语表

| 术语 | 全称 | 解释 |
|------|------|------|
| AST | Abstract Syntax Tree | 抽象语法树——编译器对源代码的结构化表示 |
| BSS | Block Started by Symbol | 未初始化的全局变量段 |
| CFG | Control Flow Graph | 控制流图——基本块和跳转边的有向图 |
| CSE | Common Subexpression Elimination | 公共子表达式消除 |
| DFA | Deterministic Finite Automaton | 词法分析使用的确定性有限自动机 |
| DWARF | Debug With Arbitrary Record Format | 调试信息格式 |
| ELF | Executable and Linkable Format | 可执行与可链接格式 |
| GIMPLE | — | GCC 的三地址码中间表示 |
| IR | Intermediate Representation | 中间表示 |
| LMA | Load Memory Address | 加载内存地址（段在文件中存储的地址） |
| LTO | Link-Time Optimization | 链接时优化 |
| RTL | Register Transfer Language | GCC 的低级中间表示 |
| SSA | Static Single Assignment | 静态单赋值形式 |
| VMA | Virtual Memory Address | 虚拟内存地址（段在运行时所在的地址） |

## A.4 进一步阅读

1. **ISO/IEC 9899:2011 (C11 标准)** —— 第 5.1.1.2 节正式定义了 8 个翻译阶段。
2. **GCC Internals Manual** —— 详细介绍了 GIMPLE、RTL、优化 Pass 的完整架构。
3. **ARM Architecture Reference Manual (ARMv7-M)** —— 第 A7 章定义了 Thumb-2 指令的完整编码格式。
4. **ELF for the ARM Architecture (ARM IHI 0044E)** —— ARM 平台 ELF 格式与重定位类型的完整规范。
5. **System V Application Binary Interface (ARM 补充)** —— 定义了 ARM 平台的 ABI，包括函数调用约定和数据布局。
6. **Linkers and Loaders (John R. Levine)** —— 关于链接器和加载器的经典著作，涵盖从 PDP-11 到现代系统的完整链接技术。

---

*基准项目：STM32F446 RTOS | 工具链：arm-none-eabi-gcc 12 | 目标架构：ARMv7E-M (Cortex-M4F/M7F)*
