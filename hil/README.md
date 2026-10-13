# hil/ — 真机测试台（Hardware-in-the-Loop）

本目录是**全部真机测试的唯一入口**。设计原则：**测试夹具的副本集中在这里，测试逻辑不散落在内核源码目录**；
所有需要改工程构建文件的操作都由脚本完成并可一键还原。

> 配套文档：[测试报告](../docs/rtos_testing_report.md)（功能/边界结果与缺陷清单）、
> [历史性能报告](../docs/rtos_performance_report.md)（性能数据与复现纪律）、
> [底层优化报告](../docs/rtos_lowlevel_optimization_20261012.md)（当前优化实现）、
> [综合压力报告](../docs/rtos_production_stress_20261013.md)（16 任务、双 IRQ 及原始证据）。

---

## 1. 一分钟上手

```powershell
# 跑传统套件：audit / bench / boundary / selftest / scenario
# 自动切换固件变体 → 烧录 → 抓串口；不包含后续优化及综合压力批次
powershell -ExecutionPolicy Bypass -File hil\repro_all.ps1

# 只跑部分
powershell -ExecutionPolicy Bypass -File hil\repro_all.ps1 -Only selftest,boundary

# 单独构建某个变体（不烧录）
powershell -ExecutionPolicy Bypass -File hil\build_fw.ps1 -Variant bench

# 把工程恢复到 CubeIDE 原始构建状态（并重建日常固件）
powershell -ExecutionPolicy Bypass -File hil\restore_project.ps1 -Rebuild
```

### 运行前提与仓库发布范围

上述命令用于 Windows PowerShell 和 STM32CubeIDE 工具链。当前脚本和构建模板保留了实测时的
固定工程路径 `E:\STM32_Project\CubeIDE\f446_rtos`、CubeIDE 1.13.2 工具路径及默认 COM3。
其他电脑需要先配置工程、编译器、make、CubeProgrammer 和串口路径，再执行新批次；
路径配置发生变化时须保存新的源码身份与证据，不能将新构建认作原始实测镜像。

Git 仓库保留测试源码、脚本、原始文本日志、源码 manifest 和解析结果。
`Debug/`、本地 ELF、`baseline_RTOS/` 等回退快照及 `backup_precleanup/` 不随 Git 发布。
因此，clone 后可以审阅提交的文本证据，但不能原样执行完整原始身份复核：
历史 `validate_evidence.ps1` 还依赖保存的 ELF、本机源码身份及日常构建目录；
缺少这些输入时应视为证据不可用，不能绕过哈希检查或据此生成新的通过结论。

`repro_all.ps1` 只覆盖传统套件。第一轮性能优化、底层优化和综合压力分别使用下表中
`perf_20261012/`、`lowlevel_20261012/`、`production_20261012/` 的独立入口；
历史阶段名称不会自动回退实现，复测前须按对应报告确认当前源码和配置。

---

## 2. 脚本清单

| 脚本 | 用途 |
|---|---|
| `repro_all.ps1` | **传统套件入口**：audit / bench / boundary / selftest / scenario，自动构建+烧录+抓串口并汇总 |
| `build_fw.ps1` | **变体化构建**：靠 `-DFIRMWARE_*` 宏切换固件形态，**不改源码**；自动处理变体切换导致的强制重编译 |
| `run_hil.ps1` | **烧录 + 抓串口**：烧录与监听在同一次调用内原子完成（避免丢失开机输出）；支持终止行与自动应答 |
| `run_scenarios.ps1` | 批量跑场景 S1–S5 |
| `repeat_run.ps1` | 同一固件连续烧录 N 次，用于**确定性验证**（判断指标是否可复现） |
| `restore_project.ps1` | 还原 `Debug/Core/Src/subdir.mk`、`Debug/objects.list`，清中间产物，可选重建 |
| `perf_20261012/run_measurement.ps1`、`run_regression.ps1` | 第一轮性能优化的独立测量及回归；阶段身份与结果见[报告](../docs/rtos_optimization_20261012.md) |
| `lowlevel_20261012/run_measurement.ps1`、`run_regression.ps1`、`run_lowlevel_checks.ps1` | 底层优化测量、完整回归及上下文／FPU 检查；见[报告](../docs/rtos_lowlevel_optimization_20261012.md) |
| `production_20261012/run_production.ps1` | 综合压力批次：smoke / matrix / soak / negative；保留源码身份、构建、ELF、反汇编、真实下载和串口证据 |
| `production_20261012/restore_daily.ps1` | 本轮结束后检查内核身份、恢复日常固件并上板验证；拒绝覆盖已有恢复证据 |
| `production_20261012/validate_evidence.ps1` | 独立读取本轮证据，核对参数、消息与令牌守恒、覆盖、固件身份及日常恢复；不连接硬件 |

## 3. 固件变体（`-Variant`）

| 变体 | 编译进固件的测试代码 | 对应的判读结束行 |
|---|---|---|
| `daily` | 无（日常演示固件：应用任务 + HardFault 测试任务 + USB） | — |
| `audit` | `fw/Src/audit_{regressions,ipc,memory_timer,usb_port}.c`，2026-10-12 专项回归 | `AT\|DONE` |
| `selftest` | `Core/Src/selftest.c` | `ST\|SUMMARY\|pass=… fail=… info=… total=…\|END` |
| `bench` | `Core/Src/benchmark.c` | `BM\|DONE` |
| `perf` | `fw/Src/perf_probes.c`，叠加性能及互斥锁检查 | `BM\|DONE` |
| `lowlevel` | `fw/Src/{perf_probes,lowlevel_checks,lowlevel_fpu_probe}.c`，上下文及真实 FPU 寄存器保存检查 | `BM\|DONE` |
| `flash_diag` | `fw/Src/flash_diag.c`，同一镜像 Flash 缓存诊断 | `BM\|DONE` |
| `boundary` | `fw/Src/boundary_test.c` | `BT\|SUMMARY\|…` |
| `scenario` | `fw/Src/{kern_port_rtos,test_framework,test_scenarios}.c` | `SC\|DONE` |
| `scenario_usb` | 同上，但保留 USB 应用任务（复现历史采集环境） | `SC\|DONE` |
| `diag` | 追加 `fw/Src/diag_scenarios.c`：诊断场景 D1/D4–D10（用 `-Scenario <n>` 选择，见 §4.1） | `SC\|DONE` |
| `production_stress` | `fw/Src/production_{stress,irq}.c`：16 个不同优先级业务任务，共享 queue / sem / mutex / event，TIM6 / TIM7 真实中断 | `STRESS\|DONE\|status=…` |

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
| `production_{stress,irq}.{c,h}`（综合压力与双定时器 IRQ） | `fw/Src/`、`fw/Inc/` | — | `production_stress` |

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
