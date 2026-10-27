# 静态借用通道：2026-10-27 验证批次

本目录对应新增 `rtos_channel`，不修改历史批次的日志、源码身份或固定数量判定。
设计和应用示例见 [通道设计](../../docs/rtos_channel_design.md)，结果及限制见
[验证报告](../../docs/rtos_channel_validation_20261027.md)。

## 运行

Windows、F446 NUCLEO、ST-LINK SWD、USART2 COM3 / 115200 / 8N1；工具路径沿用
`hil/build_fw.ps1`、`hil/run_hil.ps1`。所有烧录和串口捕获必须串行执行，不要同时使用 CubeIDE 调试。
Python 为 3.10+，仅依赖标准库；宿主 C 编译另需本机 GCC。

```powershell
# 宿主：真实生产 C + 调度替身；不是 Cortex-M 时序测试
python hil/channel_20261027/host/run_host.py

# 33 个新功能镜像；默认新建带时间戳批次，拒绝覆盖既有证据
powershell -NoProfile -File hil/channel_20261027/run_matrix.ps1 -Python python

# 13 个旧内核集成镜像，统一启用通道；原有夹具不改写
powershell -NoProfile -File hil/channel_20261027/run_regressions.ps1 -Python python

# 单镜像，目录已存在会拒绝覆盖；negative 的失败白名单由离线验证器严格判定
python hil/channel_20261027/run_suite.py channel_checks --batch my_run --optimization O2

# 读取已保存证据，不操作板子；完整身份检查需要本地保留的 ELF
python hil/channel_20261027/verify_results.py hil/channel_20261027/evidence/matrix
python hil/channel_20261027/verify_results.py hil/channel_20261027/evidence/regression
python hil/channel_20261027/oracle_tests.py
python hil/channel_20261027/analyze_performance.py
python hil/channel_20261027/finalize_evidence.py

# 全部完成后恢复日常模板，并重建；烧录与串口确认应作为单独最后一步
powershell -NoProfile -File hil/restore_project.ps1 -Rebuild
```

严格编译的 430 项入口和限制见 [strict/README.md](strict/README.md)。
`run_matrix.ps1` 的配置选择与当前正式 `evidence/matrix` 对应；分析脚本默认读取该正式目录。
新批次做分析时需显式调整输入目录并保存脚本身份，不能把新的数字混入本批结果。

## 证据结构与判据

| 位置 | 内容 |
| --- | --- |
| `host/` | 独立 FIFO / 四态账本模型、保护页和边界测试；最终六个进程结果 |
| `strict/` | 408 个 M4 编译、17 个 M7 代码生成、示例与公共头检查 |
| `evidence/matrix/` | 33 个正式新功能捕获；包含预期失败的单包损坏对照 |
| `evidence/regression/` | 13 个通道启用时的旧功能集成回归 |
| `evidence/analysis/` | 从实际 DWT 行聚合的 CSV 和 JSON，不生成模拟数据 |
| `evidence/baseline/` | 扩展前已验证日常 ELF 的身份，以及关闭扩展后的可装载区比较 |
| `evidence/preflight/` | 早期调试、工具启动及解析失败；不能冒充正式 PASS |
| `evidence/oracle_tests_*/` | 在临时副本中破坏证据，证明验收器确实拒绝错误 |

每次正式捕获保留 `sources.json`、完整编译命令、ELF 副本及其散列、CubeProgrammer
物理探针/烧录验证日志、串口原文、捕获元数据和执行命令。构建前后快照必须一致；
构建 ELF、烧录身份和副本三方一致。快照保守收集全部受维护夹具，不代表每个夹具都参与该镜像。
不同捕获间未使用夹具发生变化时，可查各自编译命令与源码散列确定实际闭包。

验收不只读 SUMMARY：核对全部 CHECK 编号、失败行、IRQ 优先级、截止预算、DMA 实际
在途计数、消息唯一性、各消费者参与、9 行任务/IRQ 诊断、270 格性能矩阵、280 格 IRQ 矩阵。
负向测试仅接受一处指定损坏和三项指定失败，任何额外失败都会拒绝。
旧混合等待夹具只输出阶段汇总，验收其 73 个有序阶段的完整增量及真实 IRQ 记录。

验收器在固件编译之后可独立完善，所以不混入固件输入身份；identity 记录当次验收器散列。
最终离线复核再次使用当前验收器，保留各次历史身份，不把更新后的判据伪称为原始固件结果。

Git 默认忽略 ELF、对象和 `.su`。仓库文本足以审阅日志及命令，完整原始身份复核还需本地 ELF；
重新构建应当产生一个新证据批次。SHA 用于防止把不同镜像的结果混用，不是正确性证明。

## 范围

F446 无 D-cache；DMA 测试使用真实 DMA2 Stream0 的内存到内存传输，既不是 USB 总线测试，
也不是 ADC 外设采样验证。IRQ 延迟测试使用软件置 pending 的真实 TIM7 NVIC 异常，timer 关闭，
包含测试 hook 成本。它用于解释屏蔽窗口，不证明 WCET。宿主未使用 ASan/UBSan。
票据不能阻止旧 C 指针访问、描述符覆写或应用越界；不提供 MPU 隔离和任务删除自动回收。
