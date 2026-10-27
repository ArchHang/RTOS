# 纯 CPU 共享内存通道算法优化

当前实现用有序区间索引替代登记表重复扫描。核心不依赖 DMA/STM32 外设；测试 BSP 仍针对 F446。
设计及实测收益/代价见[优化报告](../../docs/rtos_channel_shared_20261027.md)。

本批保留历史目录，只写当前目录的新证据。实际基线来自上一批已验证的 benchmark/profile ELF，
重新烧录并采集，明确标记 `replayed_verified_image`；优化后记录为 `fresh_build`。
`baseline/manifest.json` 和4个原源码快照关联这两份归档镜像。基线不是本批重编，不能混淆两者。

## 入口

Python 3.10+ 标准库；原生测试另需 GCC；硬件依赖 CubeIDE 11.3 工具链、ST-LINK、COM3/115200。
路径配置沿用 `hil/build_fw.ps1` 和 `hil/run_hil.ps1`，支持传入实际 Python 路径。
所有硬件命令须串行执行，不要同时打开 CubeIDE 调试或另一个串口程序。

```powershell
# 新批次拒绝覆盖已有目录；以下批次名请按实际改为新名字
python hil/channel_shared_20261027/host/run_host.py
powershell -NoProfile -File hil/channel_shared_20261027/run_matrix.ps1 -Python python -Batch my_run
powershell -NoProfile -File hil/channel_shared_20261027/run_regressions.ps1 -Python python -Batch my_regression

# 当前正式证据的离线复核，不操作硬件；完整关联检查需要本地ELF
python hil/channel_shared_20261027/verify_results.py hil/channel_shared_20261027/evidence/optimized
python hil/channel_shared_20261027/oracle_tests.py
python hil/channel_shared_20261027/inspect_portability.py
python hil/channel_shared_20261027/finalize_evidence.py

# 完成所有板测后恢复日常构建；另应烧录并确认串口启动
powershell -NoProfile -File hil/restore_project.ps1 -Rebuild
```

`run_suite.py --replay <原capture目录>` 只接受配置一致、原证据散列通过、4份原生产源快照一致的
已验证 bench/profile 镜像。它复制历史构建证据并重新采集，不临时覆盖当前生产源。
新机器没有归档ELF时不能重放本批基线，应从保存的源码建立自己的新批次。

`compare_performance.py` 参数为 `--baseline-bench`、`--optimized-bench`、
`--baseline-profile`、`--optimized-profile` 四个 capture 目录和不存在的 `--output` 目录。
它校验同夹具/工作量/配置及完整270/280格，保留copy/pool/loan所有变化和退步项，输出CSV与JSON。
硬件性能只有单活跃通道；不能用来声称8通道最坏时延。原生专项覆盖8 owner的索引正确性。

## 正式证据

| 路径 | 内容 |
| --- | --- |
| `baseline/` | 4个原生产文件、固定夹具/构建输入清单及静态依赖审计 |
| `host/evidence/baseline_pre_optimization/` | 原实现6进程 |
| `host/evidence/optimized_regions_v1/` | 真实新C的8进程，含独立无序区间oracle |
| `strict/evidence/` | 429项与文档1项分批编译，均验证输入稳定 |
| `evidence/baseline_replay/` | 两份归档ELF真实重放捕获 |
| `evidence/optimized/` | 29个新功能镜像；损坏对照只接纳精确的指定失败 |
| `evidence/regression/` | 13个旧功能集成镜像 |
| `evidence/default_off/`、`default_equivalence/` | 默认关闭、日常启动和可装载镜像比较 |
| `evidence/comparison/` | 全部性能变化与对照归一化结果 |
| `evidence/oracle_tests_*/` | 27个真实证据控制/篡改检查，临时副本结束后删除 |
| `evidence/final_summary.json` | 当前源码、构建、ELF、串口和辅助测试最终关联 |

历史 source manifest 保守包含未编入对应镜像的夹具；基线的旧 stress 源差异已单独列明，
没有把它当成 bench/profile 的编译输入。验收器单独散列，不混入固件输入闭包。
静态依赖扫描仅说明通道直接引用边界，不能代替跨 CPU/BSP 上板。

Git 默认忽略ELF、对象和 `.su`；文本可审阅，原始完整身份复核需要本地镜像。
SHA用于关联证据，不是正确性证明。当前 `run_suite.py` 明确拒绝选择 `channel_dma`。
