# 测试入口与文件管理

本目录保留当前测试夹具、构建模板、执行脚本和独立验收器。按任务选择入口，
不要把旧批次目录名当成当前源码版本。API 文档与示例检查见
[ai_docs/README.md](ai_docs/README.md)，架构说明见
[rtos_design.md](../docs/rtos_design.md)。

2026-10-28 按用户要求清退历史测试工程副本、旧批次原始结果和中间产物；本次整理
没有连接板卡、编译固件、烧录或采集串口。旧结果不作为本次测试结论。

## 1. 目录与职责

```text
hil/
├── README.md                         本入口
├── build_fw.ps1 / build_layout.ps1    变体构建及日常构建布局
├── run_hil.ps1                       SWD 下载、串口采集及结束条件检查
├── repro_all.ps1                     常用回归组合入口
├── run_scenarios.ps1 / repeat_run.ps1 场景及重复运行辅助入口
├── restore_project.ps1               恢复日常构建规则，可选择重建
├── fw/                              权威测试源码，必须保留
├── templates/                       权威 make 模板，必须保留
├── refactor_20261027/                当前布局的矩阵、编译与验收脚本
├── basepri_20261025/                 保留原字节的基础日志验收器
├── adversarial_20261025/             保留原字节的复杂并发日志验收器
└── ai_docs/                         API 契约检查、示例编译及最新资料证据
```

`basepri_20261025/verify_evidence.py` 和
`adversarial_20261025/verify_evidence.py` 仍被当前验收器按固定摘要导入，
因此保留原路径、原字节；这两个目录不再保存旧工程、旧运行脚本或旧结果。
当前矩阵入口集中在 [refactor_20261027](refactor_20261027/README.md)。

运行时会重新生成 `logs/`、`fwsrc/` 和 `Debug/` 等目录。`fw/` 是测试源码，
`fwsrc/` 是复制后参与编译的中间文件，二者不能混淆。

## 2. 选择入口

| 需求 | 入口 | 是否需要板卡 |
|---|---|---|
| 核对 API 文档、索引与本地链接 | `ai_docs/check_contracts.py` | 否 |
| 编译独立 C 示例和支持实现 | `ai_docs/compile_examples.py` | 否，需 ARM GCC |
| 核对源码重组的结构与历史基线 | `refactor_20261027/verify_layout.py` | 否，需保留 Git 基线 |
| 16 个 RTOS C 单元的配置编译矩阵 | `refactor_20261027/compile_checks.ps1` | 否，需 ARM GCC |
| 常用功能、端口和复杂并发组合 | `repro_all.ps1` | 是 |
| 复杂等待、继承、销毁窗口和容量矩阵 | `refactor_20261027/run_matrix.ps1` | 是 |
| BASEPRI、阻塞返回、IRQ 优先级边界 | `refactor_20261027/run_port_matrix.ps1` | 是 |
| 功能、生产负载、负向对照和长时间运行 | `refactor_20261027/run_regressions.ps1` | 是 |
| 单个变体构建 | `build_fw.ps1` | 否，需 ARM GCC 与 make |
| 单个镜像下载与串口采集 | `run_hil.ps1` | 是 |
| 恢复日常构建规则 | `restore_project.ps1` | 否；重建需工具链，重新下载需板卡 |

上述构建及实板脚本未在本轮整理中执行；本轮只运行资料与语法等静态检查。旧批次原始输入已经清理，历史严格验收器默认运行时
会报告缺失输入，不能凭保留的脚本、叙述报告或摘要重新宣称历史批次通过。
新测试应使用新输出目录，并保留完整的源码身份、构建、固件、下载和原始日志。

## 3. 执行环境与示例命令

从工程根目录运行 Windows PowerShell。脚本当前配置为 NUCLEO-F446RE、ST-LINK SWD、
COM3、115200 / 8N1，工具路径指向本机 STM32CubeIDE 1.13.2。
`build_fw.ps1`、`restore_project.ps1` 从自身路径定位工程；部分辅助脚本仍含
`E:\STM32_Project\CubeIDE\f446_rtos` 固定路径。换电脑前先检查编译器、make、
CubeProgrammer、项目路径和串口配置，不能只修改命令中的串口参数。

以下是以后执行真实回归的命令示例，本轮没有运行这些命令：

```powershell
# 常用全部组合：audit、port、adversarial、bench、boundary、selftest、scenario。
powershell -NoProfile -ExecutionPolicy Bypass -File hil/repro_all.ps1 -Tag rerun_YYYYMMDD

# 仅功能与契约边界。
powershell -NoProfile -ExecutionPolicy Bypass -File hil/repro_all.ps1 `
    -Only selftest,boundary -Tag rerun_YYYYMMDD

# 单独构建、下载并采集生产压力变体。
powershell -NoProfile -ExecutionPolicy Bypass -File hil/build_fw.ps1 `
    -Variant production_stress -StressSeed 3735928559 `
    -StressSeconds 900 -StressQueueCapacity 8
powershell -NoProfile -ExecutionPolicy Bypass -File hil/run_hil.ps1 `
    -Elf Debug/f446_rtos.elf -Tag production_01 -Seconds 950 `
    -Terminator 'STRESS\|DONE\|' -OutDir hil/logs/rerun_YYYYMMDD

# 恢复规则并重建日常固件；该命令本身不会将日常固件重新烧录。
powershell -NoProfile -ExecutionPolicy Bypass -File hil/restore_project.ps1 -Rebuild
powershell -NoProfile -ExecutionPolicy Bypass -File hil/run_hil.ps1 `
    -Elf Debug/f446_rtos.elf -Tag daily_01 -Seconds 15 `
    -OutDir hil/logs/rerun_YYYYMMDD
```

每次运行使用新的批次目录和 Tag。部分底层辅助脚本允许覆盖同名文件，不能依赖所有
入口都会阻止覆盖；执行者应先确认目标目录未使用。需要可严格复核的矩阵批次时，
按 [矩阵方法](refactor_20261027/README.md)保存全部输入与输出。

## 4. 夹具与固件变体

`build_fw.ps1` 通过预处理宏和额外编译单元选择变体，不要求手工替换生产源码。
它将 `fw/Src/` 中所需夹具复制到 `fwsrc/`，头文件使用 `fw/Inc/`。
`Core/Src/selftest.c`、`Core/Src/benchmark.c` 仍是现有工程中的测试入口。

| 变体 | 主要覆盖 | 结束标记 |
|---|---|---|
| `daily` | 日常应用、故障演示和 USB | 按应用输出判读 |
| `selftest` | 功能回归；真实 USART 中断需上位机回发 `X` | `ST\|SUMMARY` |
| `boundary` | API 参数、上下文及边界契约 | `BT\|SUMMARY` |
| `audit` | IPC、内存、定时器、USB 包装及端口回归 | `AT\|DONE` |
| `bench` / `perf` | 微基准及性能观察 | `BM\|DONE` |
| `lowlevel` / `flash_diag` | 上下文/FPU 保存、Flash 缓存诊断 | `BM\|DONE` |
| `basepri` / `basepri_blocking` | 精确屏蔽交错、真实等待返回值 | `BP\|DONE` / `BLOCK\|DONE` |
| `basepri_guard` / `basepri_nesting` | IRQ 契约、断言及非法嵌套 | `BP\|DONE` / `NEST\|DONE` |
| `adversarial_mutex` | 锁依赖链、优先级继承与撤销 | `AM\|DONE` |
| `adversarial_wait` | 多等待者、通知、事件、超时与 ISR 交错 | `AW\|DONE` |
| `adversarial_lifetime` | 对象检查窗口内的合法中断销毁 | `OF\|DONE` |
| `adversarial_mutex_boundary` | 递归溢出、持锁者删除、有限循环依赖 | `AMB\|DONE` |
| `adversarial_registry` | 最大互斥图容量及多锁删除 | `AR\|DONE` |
| `production_stress` | 16 个业务 worker、共享 IPC、三层 IRQ | `STRESS\|DONE` |
| `scenario` / `scenario_usb` / `diag` | 场景、保留 USB 的场景及夹具诊断 | `SC\|DONE` |

具体宏、预期检查数及完整结束格式以当前构建脚本、夹具和验收器为准。
USB 控制请求替身、寄存器观察和生产函数调用不能证明物理 USB 总线互操作；
销毁窗口测试使用仍可访问的对象存储，不能证明任意旧指针或提前释放内存安全。
有限循环依赖通过真实超时解除，不表示内核实现了死锁检测。

## 5. 判读与恢复要求

- `run_hil.ps1` 先打开串口，再烧录、复位并采集，保存原始串口、下载日志、ELF 副本及摘要。
  指定结束行缺失或出现失败断言会报错；需要负向对照时才使用 `-AllowFailures`，并核对
  预期失败原因及非零结果，不能把预期失败计作正常通过。
- 自检需要真实中断握手；组合入口已配置 `ST\|ISR\|ARM` 后回发 `X`。手动采集时应显式
  配置 `-ReplyOn 'ST\|ISR\|ARM' -Reply X`，避免遗漏 USART IRQ 覆盖。
- `make` 不跟踪宏变化，不能跨变体混用旧目标。Windows 的 `make clean` 也可能因没有
  `rm` 而留下目标；应确认实际重编译命令、对象集合、源码身份及有效配置。
- 比较性能必须使用相同对象集合、频率、配置、编译优化和采样口径。`BM` 与场景周期值
  不能脱离主频换算；旧场景中的 `us_per_call` 标签不能直接解释为单次调用耗时。
- 结束后恢复日常规则、重建、重新下载并检查启动与任务进展。只恢复文件或只生成 ELF
  不等于板上已经恢复日常固件。
- 缺少 ELF、原始日志、下载记录或身份清单时，报告证据不足。离线解析、实际编译、
  寄存器观察和真实硬件执行分别报告，有限测试不能证明不存在任何 BUG。

## 6. 文件保留规则

长期维护 `fw/`、`templates/`、当前脚本和入口文档。运行产物写入独立批次，整理前先
决定要保留的完整证据，不改写旧日志或摘要来适配新源码。过期批次可以整批清退，
同时撤下相应的“可完整重验”声明和失效链接，不保留大量旧内核、旧工程或失败方案副本。

AI 示例编译与文档检查保留最新完整证据，详情见
[AI 校验说明](ai_docs/README.md)。叙述性进展集中记录在
[每日记录](../docs/logs/2026-10-28.md)，长期设计与调试说明原位维护。
