# hil/ — 真机测试台（Hardware-in-the-Loop）

本目录是**全部真机测试的唯一入口**。设计原则：**测试夹具的副本集中在这里，测试逻辑不散落在内核源码目录**；
所有需要改工程构建文件的操作都由脚本完成并可一键还原。

> 配套文档：[测试报告](../docs/rtos_testing_report.md)（功能/边界结果与缺陷清单）、
> [历史性能报告](../docs/rtos_performance_report.md)（性能数据与复现纪律）、
> [底层优化报告](../docs/rtos_lowlevel_optimization_20261012.md)（当前优化实现）、
> [综合压力报告](../docs/rtos_production_stress_20261013.md)（16 任务、双 IRQ 及原始证据）。
> 后续补充：[BASEPRI 修复与回归报告](../docs/rtos_basepri_fix_20261025.md)（精确抢占、阻塞返回、三层 IRQ）。
> 复杂并发：[对抗场景与健壮性报告](../docs/rtos_adversarial_20261026.md)（传递继承、对象销毁窗口、循环依赖、递归溢出及配置矩阵）。
> 当前源码布局：[源码导览](../RTOS/README.md)。重组后的独立复测入口位于 `refactor_20261027/`；历史批次保留当时的源码路径。

---

## 1. 一分钟上手

```powershell
# 默认 audit / port / adversarial / bench / boundary / selftest / scenario
# port 包含 74 个镜像的屏蔽、阻塞返回和优先级边界，不能仅用正常交接替代
powershell -ExecutionPolicy Bypass -File hil\repro_all.ps1

# 只跑部分
powershell -ExecutionPolicy Bypass -File hil\repro_all.ps1 -Only selftest,boundary

# 只跑精确抢占、不同编译配置和负向 IRQ 边界
powershell -ExecutionPolicy Bypass -File hil\repro_all.ps1 -Only port

# 60 个复杂并发镜像：4 套场景的 48 组合，加容量边界的 12 组合
powershell -ExecutionPolicy Bypass -File hil\repro_all.ps1 -Only adversarial

# 单独构建某个变体（不烧录）
powershell -ExecutionPolicy Bypass -File hil\build_fw.ps1 -Variant bench

# 恢复当前目录布局的日常构建模板（并重建日常固件）
powershell -ExecutionPolicy Bypass -File hil\restore_project.ps1 -Rebuild
```

### 运行前提与仓库发布范围

上述命令用于 Windows PowerShell 和 STM32CubeIDE 工具链。当前 `build_fw.ps1` 与
`restore_project.ps1` 从自身位置定位工程，构建模板使用相对源码路径；历史脚本仍可能使用
固定工程路径 `E:\STM32_Project\CubeIDE\f446_rtos`。工具仍配置为 CubeIDE 1.13.2，串口默认为 COM3。
其他电脑需要先配置工程、编译器、make、CubeProgrammer 和串口路径，再执行新批次；
路径配置发生变化时须保存新的源码身份与证据，不能将新构建认作原始实测镜像。

Git 仓库保留测试源码、脚本、原始文本日志、源码 manifest 和解析结果。
工程 `Debug/`、本地 ELF、编译器 `.su` 栈记录、`baseline_RTOS/` 等回退快照及 `backup_precleanup/` 不随 Git 发布。
`refactor_20261027/evidence/ide/generated/debug/` 与 `release/` 仅精准放行 IDE 生成的文本规则和对象清单，ELF/map 仍忽略。
因此，clone 后可以审阅提交的文本证据，但不能原样执行完整原始身份复核：
历史 `validate_evidence.ps1` 和新批次 `verify_evidence.py` 还依赖保存的 ELF、本机源码身份、栈记录及日常构建目录；
缺少这些输入时应视为证据不可用，不能绕过哈希检查或据此生成新的通过结论。

清单记录的是本机实测时的原始文件字节。当前 Git 使用 `core.autocrlf=true`，部分源码会在提交或
检出时规范换行，因此不能保证 clone 后源码摘要与原实测字节自动一致；不要为匹配新检出文件而
重写历史清单。原始 HIL txt/json 等通过 `.gitattributes` 的 `-text` 保持字节；本批旧夹具 `.recorded`、
IDE XML/生成规则快照，以及按固定摘要加载的离线验收器也保留原字节，避免发布改变证据或分析器身份。
更换路径、换行或配置后应运行新批次并生成新证据，完整流程和缺陷总览见
[2026-10-26 修复汇总](../docs/rtos_fix_summary_20261026.md)。

`repro_all.ps1` 包含传统套件及端口、复杂并发矩阵。第一轮性能优化、底层优化和综合压力分别使用下表中
`perf_20261012/`、`lowlevel_20261012/`、`production_20261012/` 的独立入口；
历史阶段名称不会自动回退实现，复测前须按对应报告确认当前源码和配置。

---

## 2. 脚本清单

| 脚本 | 用途 |
|---|---|
| `repro_all.ps1` | **传统套件入口**：audit / bench / boundary / selftest / scenario，自动构建+烧录+抓串口并汇总 |
| `build_fw.ps1` | **变体化构建**：靠 `-DFIRMWARE_*` 宏切换固件形态，**不改源码**；自动处理变体切换导致的强制重编译 |
| `build_layout.ps1` | 当前 15 个构建目录的共享清单；从权威模板建立完整 Debug 树，防止构建与恢复的目录清单漂移 |
| `run_hil.ps1` | **烧录 + 抓串口**：烧录与监听在同一次调用内原子完成（避免丢失开机输出）；支持终止行与自动应答 |
| `run_scenarios.ps1` | 批量跑场景 S1–S5 |
| `repeat_run.ps1` | 同一固件连续烧录 N 次，用于**确定性验证**（判断指标是否可复现） |
| `restore_project.ps1` | 恢复全部日常 make 规则、源码目录清单和链接对象；清除 HIL 副本及变体产物，可选重建 |
| `refactor_20261027/verify_layout.py` | 与重组前提交比较函数 token、全局声明、公开头、主函数和第三方源码，以及新链接清单；只读，无硬件 |
| `refactor_20261027/compile_checks.ps1` | 当前全部 16 个自研 C 单元 × 12 配置严格编译，加 M7 端口编译检查 |
| `refactor_20261027/run_matrix.ps1`、`run_port_matrix.ps1` | 当前布局的复杂 IPC 及端口/中断回归；构建、模板和固件输入统一快照 |
| `refactor_20261027/run_regressions.ps1` | 功能、生产压力及负向校验；`-SoakSeconds` 默认为 120，可配置至 900 |
| `refactor_20261027/verify_results.py` | 独立核对本轮原始日志、烧录、ELF、源码身份及日常恢复状态；离线分析器不参与固件构建，其身份另记于验收输出 |
| `perf_20261012/run_measurement.ps1`、`run_regression.ps1` | 第一轮性能优化的独立测量及回归；阶段身份与结果见[报告](../docs/rtos_optimization_20261012.md) |
| `lowlevel_20261012/run_measurement.ps1`、`run_regression.ps1`、`run_lowlevel_checks.ps1` | 底层优化测量、完整回归及上下文／FPU 检查；见[报告](../docs/rtos_lowlevel_optimization_20261012.md) |
| `production_20261012/run_production.ps1` | 综合压力批次：smoke / matrix / soak / negative；保留源码身份、构建、ELF、反汇编、真实下载和串口证据 |
| `production_20261012/restore_daily.ps1` | 本轮结束后检查内核身份、恢复日常固件并上板验证；拒绝覆盖已有恢复证据 |
| `production_20261012/validate_evidence.ps1` | 独立读取本轮证据，核对参数、消息与令牌守恒、覆盖、固件身份及日常恢复；不连接硬件 |
| `basepri_20261025/run_matrix.ps1` | 74 个镜像：精确中断交错、阻塞返回、优先级契约、关闭断言的非法嵌套；默认写新时间戳目录 |
| `basepri_20261025/run_regressions.ps1` | `functional` 原回归；`production` 多容量/种子和已知损坏对照；`soak` 900 秒三层 IRQ 业务负载 |
| `basepri_20261025/verify_evidence.py` | 标准库离线重算日志、不变量、ELF/下载身份和源码保留；只有严格完整通过才生成汇总 |

## 3. 固件变体（`-Variant`）

| 变体 | 编译进固件的测试代码 | 对应的判读结束行 |
|---|---|---|
| `daily` | 无（日常演示固件：应用任务 + HardFault 测试任务 + USB） | — |
| `audit` | `fw/Src/audit_{regressions,ipc,memory_timer,usb_port}.c`，2026-10-12 专项回归 | `AT\|DONE` |
| `basepri` | `fw/Src/basepri_regressions.c`，600 交错、百万 pair、mask/计数与调度进展 | `BP\|DONE\|status=…` |
| `basepri_blocking` | `fw/Src/basepri_blocking.c`，无生产者的真实阻塞返回及数据哨兵 | `BLOCK\|DONE\|status=…` |
| `basepri_guard` | `fw/Src/basepri_guard.c`，真实 ISR 优先级契约与非法嵌套；仅此变体包装断言接收器 | `BP\|DONE\|status=…` |
| `basepri_nesting` | `fw/Src/basepri_nesting.c`，关闭断言时非法计数不回绕且完整保留调用方状态 | `NEST\|DONE\|status=…` |
| `selftest` | `Core/Src/selftest.c` | `ST\|SUMMARY\|pass=… fail=… info=… total=…\|END` |
| `bench` | `Core/Src/benchmark.c` | `BM\|DONE` |
| `perf` | `fw/Src/perf_probes.c`，叠加性能及互斥锁检查 | `BM\|DONE` |
| `lowlevel` | `fw/Src/{perf_probes,lowlevel_checks,lowlevel_fpu_probe}.c`，上下文及真实 FPU 寄存器保存检查 | `BM\|DONE` |
| `flash_diag` | `fw/Src/flash_diag.c`，同一镜像 Flash 缓存诊断 | `BM\|DONE` |
| `boundary` | `fw/Src/boundary_test.c` | `BT\|SUMMARY\|…` |
| `scenario` | `fw/Src/{kern_port_rtos,test_framework,test_scenarios}.c` | `SC\|DONE` |
| `scenario_usb` | 同上，但保留 USB 应用任务（复现历史采集环境） | `SC\|DONE` |
| `diag` | 追加 `fw/Src/diag_scenarios.c`：诊断场景 D1/D4–D10（用 `-Scenario <n>` 选择，见 §4.1） | `SC\|DONE` |
| `production_stress` | `fw/Src/production_{stress,irq,highirq}.c`：16 个不同优先级业务任务、共享 IPC，TIM6/TIM7 系统调用中断与 TIM2(0) 硬实时干扰 | `STRESS\|DONE\|status=…` |

> 隔离测试变体关闭日常应用和故障演示任务，仍有测试任务、idle 和启用的软件定时器任务。
> `scenario_usb` 保留 USB 应用任务，`daily` 运行演示任务，`production_stress` 则主动混合业务任务及 IRQ。
> 任务竞争和统计口径应按具体变体及对应报告解释。

---

## 4. 测试夹具源码位置

**权威副本在 `fw/`，`Core/Src/` 下两份是历史位置（仍参与变体构建，未移动以免打断工程）。**

| 夹具 | 权威副本 | 同时存在于 | 由哪个变体构建 |
|---|---|---|---|
| `selftest.c`（442 用例功能回归） | — | `Core/Src/selftest.c` | `selftest` |
| `benchmark.c`（BM0–BM7 微基准） | — | `Core/Src/benchmark.c` | `bench` |
| `boundary_test.{c,h}`（103 断言契约边界） | `fw/Src/`、`fw/Inc/` | — | `boundary` |
| `test_scenarios.{c,h}`（S1–S5 场景） | `fw/Src/`、`fw/Inc/` | — | `scenario` / `scenario_usb` / `diag` |
| `test_framework.{c,h}`（统计与上报） | `fw/Src/`、`fw/Inc/` | — | 同上 |
| `kern_port.h` + `kern_port_rtos.c`（内核适配层） | `fw/Src/`、`fw/Inc/` | — | 同上 |
| `diag_scenarios.{c,h}`（D1/D4–D10 诊断场景） | `fw/Src/`、`fw/Inc/` | — | `diag` |
| `production_{stress,irq,highirq}.{c,h}`（综合压力与三层 IRQ） | `fw/Src/`、`fw/Inc/` | — | `production_stress` |
| `basepri_{regressions,blocking,guard,nesting}.c`（精确交错与屏蔽／返回契约） | `fw/Src/` | `basepri_regressions.h` 在 `fw/Inc` | 四种 `basepri*` 变体 |

`build_fw.ps1` 会把 `fw/Src/*.c` 复制到构建目录 `fwsrc/` 编译（让 Makefile 生成的文件保持干净可还原），
头文件搜索路径直接指向 `fw/Inc`。

### 4.1 诊断场景清单（`-Variant diag -Scenario <n>`）

诊断场景**不是产品测试**，而是审计工具：每一个回答"报告里的结论是否真的被这份夹具测到了"。
其中被否证的场景，其编号对应的旧数字**不得再引用**：

| 编号 | 场景 | 结论状态 |
|---|---|---|
| 11 / 16 / 17 / 18 | D1 S1 争用取证 / D4 陈旧 `queue_bitmap` / D5 周期上界 / D6 栈帧 PC 的 Thumb 位 | ✅ 结论已采信（D1 的结论是**否定性的**：S1 夹具从未制造争用，故 S1 全部数据作废） |
| 19 / 20 | D7 `delay()` 成本随延时链长度 / D8 `sem_take` 阻塞成本随等待者数 | ⚠️ 可运行，**未采集完整数据** |
| 21 | D9 最坏关中断窗口（WCET） | ❌ **口径未收敛，数字不可引用**（见 `docs/rtos_testing_report.md` §10.3） |
| 22 | D10 `rtos_sched_suspend` 期间的阻塞 API | ✅ 结论已采信（判据 `verdict=REACHED_END`；同一契约的回归用例见 `selftest` 的 FIX19） |

### FreeRTOS 对照侧

场景套件设计为**同一份 `test_scenarios.c` 编进两套内核**，靠宏选路：
`-DKERN_IS_CUSTOM_RTOS` → `kern_port_rtos.c`；不定义 → `kern_port_freertos.c`。
历史 FreeRTOS 侧的完整库（含 `kern_port_freertos.c`）与构建/回滚脚本位于本地
`.trae/scenario_test/`，原入口为 `suite.ps1 -Project freertos -Scenarios 1,2,3,4,5`。
该对照工程和日志属于外部历史资料，不随本仓库发布；当前仓库入口用于自研 RTOS 一侧，
不能将其描述为 clone 后即可复现全部历史 FreeRTOS 对照。

---

## 5. 目录结构

```
hil/
├── build_fw.ps1 / run_hil.ps1 / repro_all.ps1 / run_scenarios.ps1
├── repeat_run.ps1 / restore_project.ps1
├── fw/                      ★ 权威测试夹具源码
│   ├── Src/                 （变体构建时复制到 Debug 下的 fwsrc/）
│   └── Inc/
├── fwsrc/                   构建中间产物（可弃，restore 会删除）
├── templates/               pristine 的 subdir.mk 模板 + 上次变体的副本
├── logs/
│   ├── current/             历史文档整合基线（保留原目录名）
│   └── archive/             历史轮次日志
├── audit_20261012/          后续专项审计与修复证据
├── perf_20261012/           第一阶段性能优化证据
├── lowlevel_20261012/       底层优化与上下文检查证据
├── production_20261012/     后续综合压力证据（实际完成于 2026-10-13）
├── basepri_20261025/        临界区修复对照、74 镜像、三层 IRQ 与独立验证脚本
├── archive_docs/            已合并的历史文档（见其 README.md）
├── archive_tests/           已作废的测试资产（见 §6）
└── backup_precleanup/       源码清理前回滚点（仅本地归档，不入库）
```

### 日志命名约定

`log_<Tag>_<suite>.txt`，其中 `<Tag>` 是运行批次：
`repro_`（首次复现）→ `sc123_`/`sc45_`（场景分批）→ `diag_`（取证）→ `clean1_`/`final_`（清理前后）
→ `verify_` → `verylast_`（整合前基线）→ `organized_`（文档整合后基线）→ `fix19_`/`d10_`（历史第二轮审计修复与取证）。
这些 tags 属于历史文档整合批次；后续审计、优化与综合压力使用上述带日期目录及各自的源码身份，
不将 `logs/current/` 或 `fix19_` 的结果替代当前实现的验证。

---

## 6. `archive_tests/` 里是什么，为什么不要用

| 子目录 | 内容 | 为什么不删也不要用 |
|---|---|---|
| `stale_kernel_copies/` | 9 个**被改过的内核源码旧副本**（`rtos_task.c`/`rtos_sched.c`/`rtos_mutex.c`/`rtos_sem.c`/`rtos_types.h`/`rtos_internal.h`/`selftest.c`/`test_scenarios.c`），原在 `.trae/scenario_test/applied/` | **最危险的一类**：与当前内核已漂移（如 `rtos_task.c` 38 557 B vs 现行 42 758 B）。它们**从不参与任何构建**，只是当年"改一份副本去实验"留下的快照。误当源码读会得出完全错误的结论。 |
| `unverified_wip/` | `wip_bitmap/` 6 文件：等待链位图化的早期实现 | 该方案**已被采纳并实装进内核**（见 `docs/rtos_performance_report.md` §4.2，退化 +18.4% → 0%）。留作实现的溯源记录，不要复用。 |
| `superseded_scripts/` | 旧的 `run.ps1`、`wire.ps1` | `run.ps1` → 已由 `hil/run_hil.ps1` 取代；`wire.ps1` → 已由 `wire2.ps1` 取代。 |

---

## 7. 踩坑记录（工具链自身，每条都真实发生过）

| # | 现象 | 根因 | 处置 |
|---|---|---|---|
| 1 | 变体切换后跑的还是上一个场景 | **`make` 不跟踪宏变化** | 切换 `-DFIRMWARE_*` 时必须删 `main.o`；`build_fw.ps1` 已用构建戳自动处理 |
| 2 | 判定"警告是否消失"得到假结论 | **CubeIDE 的 `make clean` 只删一部分 `.o`**（实测残留 59 个） | 判断编译产物变化必须手动清空 `Debug/**/*.o` 后重建 |
| 3 | 跨版本性能差 11% | **参与链接的目标文件集合不同**（`objects.list` 增删） | 比较前必须保证两侧 `.o` 集合一致；`objects.list` 每次从模板完整重建，**不可做增量过滤**（第一次排除某项后就永久丢失） |
| 4 | 烧录后串口长时间无输出 | 固件在**监听窗口之前**就跑完了 | 烧录与监听必须在同一次调用内原子执行（`run_hil.ps1` 已如此） |
| 5 | ISR 矩阵用例恒 FAIL | 该用例需上位机回发字符触发 | `run_hil.ps1` 检测到 `ST\|ISR\|ARM` 后自动回发 `X`（实测等待 170~180 ms） |
| 6 | `hil\fw` 被当成中间产物删掉 | 它与 `fwsrc`（中间产物）名字相近 | `restore_project.ps1` **只删 `fwsrc`，不碰 `fw`** |

---

## 8. 判读输出

2026-10-12 专项复现与修复证据存放于 `audit_20261012/`，分析见 `docs/rtos_audit_20261012.md`。运行方法：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File hil\build_fw.ps1 -Variant audit
powershell -NoProfile -ExecutionPolicy Bypass -File hil\run_hil.ps1 -Elf Debug\f446_rtos.elf -Tag audit_repeat -Seconds 30 -Terminator 'AT\|DONE' -OutDir hil\audit_20261012
```

`audit` 的 PendSV 窗口钩子、USB 传输替身和寄存器影子仅在测试宏下编译；正常变体不包含它们。UART 为真实 COM3/115200，IRQ 为真实 TIM6 优先级 6；USB 异常包和控制请求通过夹具输入生产处理函数，因此这些用例不代表实际 USB 总线互操作测试。

`run_hil.ps1` 保存串口日志、烧录校验日志、ELF 副本及含 SHA-256 的 JSON。指定的结束行缺失或出现失败断言时返回错误；记录修复前预期失败需显式使用 `-AllowFailures`。该选项仍要求测试到达结束行。`restore_project.ps1` 同时还原正常链接规则和测试宏，保留 `fw/` 中的权威测试源码。

| 套件 | 数据行格式 | 单位 |
|---|---|---|
| selftest | `ST\|<模块>\|<用例>\|<PASS\|FAIL\|INFO>\|<详情>` | — |
| boundary | `BT\|<分类>\|<用例>\|<PASS\|FAIL>\|<实际>\|<期望>` | — |
| bench | `BM\|<id>\|<name>\|N=<n>\|avg=<>\|min=<>\|max=<>` | CPU 周期（× 5.952 = ns） |
| scenario | `SC\|<场景>\|<指标>\|N=<n>\|avg=<>\|min=<>\|max=<>\|bad=<>\|us_avg=<>` | CPU 周期 |

**避坑**: 场景的 `us_per_call` 字段是 `kern_cycles_to_us(1000000)`，即
**1e6 个周期对应的微秒数（= 5952 µs）**，不是"每次调用的微秒数"。等价地 5.952 ns/周期。

失败时 `selftest` 会打印 `ST|ASSERT|TRAP|cond=… file=… line=…` 后停机；
总线错误由工程自带 HardFault 诊断打印完整寄存器现场（见 `docs/hardfault_debug.md`）。

## 9. 综合业务压力测试

业务模型为采集、队列传输、处理、周期控制和维护。16 个 worker 使用优先级 5–20，
四个采集任务与两个硬件定时器 ISR 向同一队列发送消息，四个处理任务竞争接收。
另外包含两个共享信号量服务任务、ANY／ALL／timeout 事件任务和共享资源维护。
controller 检查每个角色进展、等待链、优先级继承、消息身份、资源守恒和有界关停。
详细判据及其适用范围见上述综合报告。

```powershell
# 从项目根目录执行。工作阶段为正常、高压、恢复，各占总时长约三分之一。
powershell -NoProfile -ExecutionPolicy Bypass -File hil\build_fw.ps1 `
    -Variant production_stress -StressSeed 3735928559 `
    -StressSeconds 900 -StressQueueCapacity 8
powershell -NoProfile -ExecutionPolicy Bypass -File hil\run_hil.ps1 `
    -Elf Debug\f446_rtos.elf -Tag production_repeat_01 -Seconds 950 `
    -Terminator 'STRESS\|DONE\|' -OutDir hil\production_repeat_01

# 测试结束后恢复并下载日常固件。
powershell -NoProfile -ExecutionPolicy Bypass -File hil\restore_project.ps1 -Rebuild
powershell -NoProfile -ExecutionPolicy Bypass -File hil\run_hil.ps1 `
    -Elf Debug\f446_rtos.elf -Tag daily_repeat_01 -Seconds 15 `
    -OutDir hil\production_repeat_01
```

参数限制：时长 6–900 秒，队列容量 1–64，seed 为 uint32。
本轮 matrix 使用容量 1／8／32 和不同种子，soak 为 900 秒。
默认关闭 USB、日常演示和故障任务，排除旧 TIM6 benchmark handler。
串口为真实 COM3／115200；该负载不代表物理 USB、传感器或 DMA 端到端测试。

正常运行必须到达 DONE status=0，SUMMARY fail=0、worker_errors=0，且达到报告中的覆盖下限。
队列满、发送超时和信号量 overflow 是显式背压计数，不能误记为成功发送或成功 give。
编译时加 `-StressInjectCorruption` 可执行单包 checksum 损坏负向对照；抓取时须显式加
`-AllowFailures`，并核对具体 INJECT、packet_integrity 和非零结束状态，不能将其算作正常 PASS。

`run_production.ps1` 是本轮固定板卡／目录的批次执行器，重复执行必须使用新的 `-Label`，
否则拒绝覆盖已有结果。本轮恢复脚本也只接受尚无恢复证据的状态。
`validate_evidence.ps1` 只复核本轮固定 tags，新增批次需相应提供独立复核，不能套用旧结果。

## 10. 复杂并发与销毁窗口

`adversarial_20261025/` 保存 10 月 25 日开始、26 日完成的批次。
新测试覆盖此前正常交接测试缺少的依赖传播和检查窗口，夹具不修改日常 `main.c`。

| 构建变体 | 主要判据 | 测试方式 |
|---|---|---|
| `adversarial_mutex` | 2／3／6 层锁链的继承与撤销，15 个场景 | 公共 API 建链；成功、超时、挂起、删除及动态改优先级 |
| `adversarial_wait` | 8 轮混合等待，73 个阶段、120 次合法 ISR 请求 | 多发送／接收者、信号量、ANY／ALL 事件、通知、超时与迟到操作；TIM2 优先级 0 背景中断 |
| `adversarial_lifetime` | 80 次对象检查窗口，销毁后不再改状态或输出 | 测试钩子在外部预检后挂起真实 TIM7 优先级 5 中断；静态存储不释放 |
| `adversarial_mutex_boundary` | 溢出拒绝、删除持锁者后的移交、有限循环依赖 | 仅递归计数边界显式注入 `UINT32_MAX`；其余使用公共 API |
| `adversarial_registry` | 8 把锁／9 个所有者移交、删除双锁所有者，共 259 项检查 | 公共 API 构造，观察图容量和两条等待链的连续移交 |

```powershell
# 新目录保存每个镜像的源码身份、编译命令、固件、SWD 校验、串口和结果索引。
# 同一目录拒绝覆盖；矩阵为 4×2×2×3 = 48 个镜像。
powershell -NoProfile -ExecutionPolicy Bypass -File hil\adversarial_20261025\run_matrix.ps1 `
    -OutDir hil\adversarial_repeat_01

# 补充注册表满容量／多锁删除的同一组 12 种配置。
powershell -NoProfile -ExecutionPolicy Bypass -File hil\adversarial_20261025\run_matrix.ps1 `
    -RegistryOnly -ExpectedRegistryChecks 259 -OutDir hil\adversarial_repeat_01\registry

# 8 个相关内核文件的 12 配置严格编译，加 CM7 端口仅编译检查。
powershell -NoProfile -ExecutionPolicy Bypass -File hil\adversarial_20261025\compile_checks.ps1 `
    -OutDir hil\adversarial_repeat_01\compile
```

销毁窗口钩子只在专用生命周期变体编译，生产固件不包含该钩子。
测试的是仍可访问的对象存储；调用者仍须管理存储寿命，不能在其它上下文仍持有指针时释放或复用内存。
有限循环测试以真实超时打破依赖，不能解释为内核提供死锁检测。
各场景都有有限捕获期限；缺少结束行、出现 FAIL 或检查数量不足均拒绝验收。
原始故障基线及本批独立复核入口见[报告](../docs/rtos_adversarial_20261026.md)。
