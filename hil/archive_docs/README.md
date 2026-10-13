# 归档：已合并的历史文档

本目录存放**已被合并**的旧报告。它们的内容已并入 `docs/` 下的两份权威文档，
保留原件仅供追溯历史测量值与结论演变。**不要在这里更新内容。**

| 归档文件 | 原内容 | 合并去向 |
|---|---|---|
| `rtos_vs_freertos_perf_report.md` | BM0–BM7 微基准 + 与 FreeRTOS 的逐项对比与归因 | `docs/rtos_performance_report.md` §2、§3 |
| `rtos_perf_optimization_report.md` | P-1~P-5 优化项的收益与原理 | `docs/rtos_performance_report.md` §5.1 |
| `rtos_vs_freertos_scenario_report.md` | 系统级场景 S1–S5 + 边界夹具 + 勘误章节 | `docs/rtos_performance_report.md` §4；边界夹具见 `docs/rtos_testing_report.md` §2 |
| `rtos_selftest_report.md` | 369 用例自测、P1 缺陷、R1~R9 风险清单、HIL 闭环实录 | `docs/rtos_testing_report.md` §1、§4、§5 |
| `rtos_repro_audit_report.md` | 2026-09-17 HIL 复现审计：夹具错误、10 项内核缺陷、复现工具链 | `docs/rtos_testing_report.md` §3、§6 |
| `rtos_cleanup_report.md` | 源码清理的判据、删除清单、尺寸对比 | `docs/rtos_testing_report.md` §8 |
| `_audit_test_coverage_gaps.md` | 测试覆盖度审计（缺口清单、文档不一致表、建议优先级） | 缺口见 `docs/rtos_testing_report.md` §6.3；不一致见 §9 |
| `normalize_heading_punct.ps1` | 一次性脚本：把 Markdown 标题里的全角标点规范化为半角 | 已完成使命；正文不受影响，重跑无意义 |

## 为什么合并

合并前同一批测量数据分散在 4 份文档里，且互相矛盾：

- 用例总数出现 369 / 399 / 402 / 419 四种说法；
- 延时链复杂度在性能报告（"单向+摘除 O(n)"）、优化报告（"双向+O(1)"）、
  设计文档（"插入 O(n)、摘除 O(1)"）里三处不同，只有设计文档准确；
- 场景报告 §2 的结论已被同一文件 §0 的勘误作废，但 §2 仍留在正文里；
- tick 回绕在自测报告里记为"模块全 PASS"，实际只是**注入式仿真**。

**合并原则**：每个结论只保留一处权威表述；被更正/作废的结论以"已作废"表格显式列出，
不留在正文里误导读者。

## 相关

- 权威文档：`docs/rtos_performance_report.md`、`docs/rtos_testing_report.md`
- 清理前的源码回滚点：`hil/backup_precleanup/*.before`
- 原始串口日志：`hil/logs/`；FreeRTOS 侧归档结果：`.trae/scenario_test/results_freertos.txt`