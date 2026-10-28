# 当前布局的回归与验收脚本

本目录保留源码重组后可用于新批次的构建、复杂 IPC、端口和生产负载脚本。
目录名表示脚本建立日期，不表示执行时会切换到该日期的内核。源码布局见
[RTOS 导览](../../RTOS/README.md)，总入口见 [hil/README.md](../README.md)。

2026-10-28 按用户要求清退本目录旧 `evidence/`、源码身份清单和历史验收摘要，
不再保留旧固件、IDE 临时工程、预检失败镜像和重复采样。脚本本轮保持原字节，
没有编译固件、连接板卡或重新执行历史验收。原始输入已不在工程内，不能据历史
叙述重新宣称整批通过，也不能将脚本保留本身当成运行结果。

## 1. 保留文件

| 文件 | 用途 |
|---|---|
| `verify_layout.py` | 与 Git 基线 `6602f8fa1b9acb9b12ba27e17f2ffc4fbcb8b03f` 比较函数 token、全局声明、公开头、主函数、第三方源码和链接清单 |
| `compile_checks.ps1` | 16 个自研 C 单元 × 屏蔽方式 × 断言 × O0/O2/O3，共 192 项严格编译，另有 M7 端口编译 |
| `evidence_helpers.ps1` | 构建及采集前后记录固件、工程、模板和运行脚本的输入身份 |
| `run_matrix.ps1` | 48 个复杂 IPC 镜像；`-RegistryOnly` 另选 12 个最大容量镜像 |
| `run_port_matrix.ps1` | 屏蔽交错、阻塞返回、非法嵌套和 ISR 优先级矩阵 |
| `run_regressions.ps1` | 功能、生产负载、校验和负向对照与长时间运行 |
| `verify_results.py` | 对满足固定历史协议的完整证据进行严格离线验收 |

验收器按固定 SHA-256 导入下列两个文件，其原路径与原字节必须保留：

- [基础日志验收器](../basepri_20261025/verify_evidence.py)
- [复杂并发验收器](../adversarial_20261025/verify_evidence.py)

这些文件没有附带旧批次原始结果。修改分析器后不能继续沿用原分析器身份和通过结论。

## 2. 新批次执行方法

从项目根目录执行 Windows PowerShell；工具链、下载器和默认 COM3 配置见总入口。
每次使用从未写入过的输出目录，按实际日期替换 `YYYYMMDD`。以下是未来运行示例，
本轮文件整理没有执行这些命令：

```powershell
# 只读结构检查；需本地 Git 历史中仍有指定基线。
python -B hil/refactor_20261027/verify_layout.py

# 编译检查，不连接硬件。M7 项仅编译，不代表 M7 实板测试。
powershell -NoProfile -File hil/refactor_20261027/compile_checks.ps1 `
    -OutDir hil/logs/rerun_YYYYMMDD/compile

# 下列命令会构建、烧录并使用串口，需真实 F446 板卡。
powershell -NoProfile -File hil/refactor_20261027/run_matrix.ps1 `
    -OutDir hil/logs/rerun_YYYYMMDD/matrix
powershell -NoProfile -File hil/refactor_20261027/run_matrix.ps1 `
    -RegistryOnly -ExpectedRegistryChecks 259 `
    -OutDir hil/logs/rerun_YYYYMMDD/registry
powershell -NoProfile -File hil/refactor_20261027/run_port_matrix.ps1 `
    -Suite all -OutDir hil/logs/rerun_YYYYMMDD/port
powershell -NoProfile -File hil/refactor_20261027/run_regressions.ps1 `
    -Suite functional -OutDir hil/logs/rerun_YYYYMMDD/regression
powershell -NoProfile -File hil/refactor_20261027/run_regressions.ps1 `
    -Suite production -OutDir hil/logs/rerun_YYYYMMDD/regression
powershell -NoProfile -File hil/refactor_20261027/run_regressions.ps1 `
    -Suite soak -SoakSeconds 120 -OutDir hil/logs/rerun_YYYYMMDD/soak

# 恢复并重建日常固件；之后另用 run_hil.ps1 重新烧录并检查任务输出。
powershell -NoProfile -File hil/restore_project.ps1 -Rebuild
```

生产负载 `-SoakSeconds` 接受 30–900 秒；运行时间必须记录在新批次中。
以上命令各自执行检查，但不是生成历史整批严格 PASS 的完整配方。

## 3. 严格验收的边界

`verify_results.py` 是冻结的历史整批协议验收器，不是任意新目录的通用日志工具。
它除最终矩阵外，还要求固定的源码身份清单、完整 ELF、构建和下载记录、栈用量记录、
日常恢复，以及一次特定错误断言映射的失败样例。原 `compile_checks.ps1` 的 M7 记录
缺少完整参数，历史批次曾通过真实补充编译记录弥补；该补充记录也已清退。

因此，仅执行上面的新测试命令并不自动满足它的所有输入要求。默认 `evidence/`
已删除，直接执行严格验收会报告缺失，不能绕过检查、复制旧 PASS 或伪造被清退的
失败样例来生成新成功结论。未来需要重新建立完整验收协议时，应先审核新批次的
输入约定及独立判据，再运行和记录；本轮没有修改该验收器或放宽任何判据。

离线查看一个新批次目前缺哪些输入，可使用：

```powershell
python -B hil/refactor_20261027/verify_results.py `
    --evidence hil/logs/rerun_YYYYMMDD --in-progress
```

`--in-progress` 不会写出成功报告。缺少输入不是通过，叙述摘要不能替代原始证据。

## 4. 采样纪律

每个镜像记录实际源码与构建输入，构建后核对身份未变化，再核对烧录、串口结束行、
检查数量和不变量。负向对照必须命中特定预期失败，不能与正常通过合并计数。
只允许报告实际执行的层次：结构检查、编译、离线解析、寄存器观察或真实硬件测试。
恢复日常构建后还须重新下载与核对任务进展；只生成日常 ELF 不等于已恢复板上状态。

USB 包装测试不等于物理 USB 主机互操作，M7 交叉编译不等于 M7 硬件验证，
有限矩阵不能证明不存在任何 BUG。后续执行产生的新结果使用独立批次管理，
叙述性更新追加每日记录，不再创建一批旧工程副本和按每次运行命名的报告。