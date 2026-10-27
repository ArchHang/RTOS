# CubeIDE 隔离构建验证（2026-10-27）

从冻结工程复制 186 个文件，使用独立 `-data` 工作空间导入副本，再由本机 `stm32cubeidec.exe` 重新生成并构建 Debug、Release。未使用原工程的 Debug、HIL 模板或编译结果。

| 配置 | 优化 | 独立编译单元 | 生成规则目录 | 编译/链接结果 |
|---|---|---:|---:|---|
| Debug | O3 | 57 | 15 | exit 0，0 errors，3 warnings |
| Release | Os | 57 | 15 | exit 0，0 errors，3 warnings |

两配置均包含 16 个自研 RTOS C 单元、6 个 CherryUSB C 单元和完整应用、HAL、AI、启动文件。源码与对象清单逐项核对，未引用旧端口目录或原工程的绝对构建路径。编译器 include 包含公共 `Core/Inc` 与私有 `Core/Private`。

实测 IDE 选择 GNU Tools for STM32 14.3.rel1 / GCC 14.3.1，版本原始输出见 `compiler_version.txt`。每个配置的 3 条告警为日常 main.c 的两个未使用声明及该工具链发出的 RWX LOAD 段告警；未出现 RTOS 编译告警。未修改任何源文件、工程配置或链接脚本来消除这些告警。

`*_headless.log` 保留完整原始构建输出，`*_command.json` 记录真实命令，`summary.json` 记录检查结果及 ELF 身份，`generated/` 保存 IDE 生成的规则与本地 ELF/map。ELF/map 不随 Git 发布。复制前后核对 186 个文件，原件与副本均未变化。临时工程与独立工作空间在证据归档后删除，见 `cleanup.json`。

此项证明 IDE 配置能重新生成、编译和链接两个配置。它不代表两份 IDE 镜像经过上板运行；上板结果由本轮独立 HIL 证据提供，且 HIL 使用 GCC 11.3。
