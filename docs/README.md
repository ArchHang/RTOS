# 工程文档导航

当前实现以源码、[设计手册](rtos_design.md)和逐项API契约相互核对。
历史日志记录当时的修改与有限验证，不应作为当前固件已经复测的证明。

| 阅读目标 | 入口 |
|---|---|
| 快速了解工程与源码职责 | [工程README](../README.md)、[RTOS源码导览](../RTOS/README.md) |
| 理解底层机制、API用法与完整任务/IPC例程 | [rtos_design.md](rtos_design.md) |
| 让AI检索API、编写与检查调用 | [AGENTS.md](../AGENTS.md)、[AI指南/契约/示例](ai/README.md) |
| 定位运行错误、IRQ屏蔽与等待问题 | [AI调试指南](ai/debugging.md)、[HardFault手册](hardfault_debug.md) |
| 查看测试覆盖、判据和测量局限 | [测试参考](rtos_testing_report.md)、[性能参考](rtos_performance_report.md) |
| 执行验证或编译示例 | [HIL方法与夹具](../hil/README.md)、[资料/示例静态验证](../hil/ai_docs/README.md) |
| 学习编译与其他内核设计 | [编译链接原理](compilation_pipeline.md)、[FreeRTOS分析](freertos_design.md) |
| 更新设计插图 | [图片维护说明](images/README.md)、[图源校验/渲染脚本](gen_diagrams.ps1) |

<a id="daily-logs"></a>
## 更新日志

同一天的工作集中在一个`logs/YYYY-MM-DD.md`中，新增工作追加当天文件；长期手册原位
更新，不按每次执行生成叙述报告，不把未实现方案写成已有功能。

| 日期 | 内容 |
|---|---|
| [2026-10-12](logs/2026-10-12.md) | 审计修复、两阶段性能试验、失败与测量方法 |
| [2026-10-13](logs/2026-10-13.md) | 交付汇总、16任务/双IRQ压力与负向对照 |
| [2026-10-25](logs/2026-10-25.md) | BASEPRI共享槽位竞争、出口ISB缺口及回归 |
| [2026-10-26](logs/2026-10-26.md) | 传递继承、对象销毁窗口、递归上限及复杂等待 |
| [2026-10-27](logs/2026-10-27.md) | 源码职责重组、构建迁移与历史验收 |
| [2026-10-28](logs/2026-10-28.md) | AI资料、编译验证、目录清理及[工程交付报告](logs/2026-10-28.md#delivery-report) |
| [2026-10-29](logs/2026-10-29.md) | 设计手册重写、设计细节/API样例扩充及实际ARM编译验证 |

2026-10-28按用户要求删除了过时的HIL工作副本和结果，保留现用测试源码、必要工具及
最新资料验证证据。历史日志的测试数字来自原报告，本次整理没有重新上板；已删除
批次不能在当前目录直接复核。曾提交的原文可由Git历史查找，未入库资产不保证恢复。
