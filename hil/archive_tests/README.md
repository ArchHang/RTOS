# archive_tests/ — 已作废的测试资产

**不要在这里取用任何代码。** 本目录只保留"曾存在过什么、为什么作废"的溯源记录。

| 子目录 | 文件数 | 原位置 | 作废原因 |
|---|---|---|---|
| `stale_kernel_copies/` | 9 | `.trae/scenario_test/applied/` | **被改过的内核源码旧副本**。与现行内核已漂移（例：`rtos_task.c` 38 557 B vs 现行 42 758 B）。它们从不参与任何构建 —— 当年"改一份副本做实验"留下的快照。误当源码阅读会得出错误结论，故集中隔离并显式标注 |
| `unverified_wip/` | 6 | `.trae/scenario_test/wip_bitmap/` | 等待链位图化的早期实现。**该方案已被采纳并实装进内核**（`docs/rtos_performance_report.md` §4.2：8 等待者退化 +18.4% → 0%）。留作实现溯源，不要复用 |
| `superseded_scripts/` | 2 | `.trae/scenario_test/` | `run.ps1` → 由 `hil/run_hil.ps1` 取代（并修正了"烧录与监听非原子"的缺陷）；`wire.ps1` → 由 `wire2.ps1` 取代 |

## 现行资产在哪

- **测试夹具源码（权威）**：`hil/fw/{Src,Inc}/`
- **构建/烧录/复现脚本**：`hil/*.ps1`，入口 `hil/repro_all.ps1`；说明见 `hil/README.md`
- **FreeRTOS 对照侧的库与构建脚本**：`.trae/scenario_test/`（`library/`、`wire2.ps1`、`deploy.ps1`、`suite.ps1`）
- **当前基线日志**：`hil/logs/current/`；历史轮次：`hil/logs/archive/`