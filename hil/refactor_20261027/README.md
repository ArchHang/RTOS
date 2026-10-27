# 2026-10-27 源码重组验证

本批验证新的源码目录、编译单元边界和构建入口。历史证据不迁移、不重写：重组前基准为提交
`6602f8fa1b9acb9b12ba27e17f2ffc4fbcb8b03f`。目录和迁移说明见
[RTOS 源码导览](../../RTOS/README.md)。

本机最终结果为 [独立验收 PASS](verification_summary.json)：145 个实板镜像，193 项严格编译检查。
完整迁移、失败尝试、性能观测及清理结论见[源码重组报告](../../docs/rtos_source_reorganization_20261027.md)。

## 执行入口

在工程根目录运行 Windows PowerShell，编译器和烧录器使用 `hil/` 脚本配置的本机路径。
硬件为 NUCLEO-F446RE，ST-LINK SWD，COM3 / 115200 / 8N1。
每次复测选择新的输出目录；脚本拒绝覆盖已有证据。

```powershell
python hil/refactor_20261027/verify_layout.py
powershell -File hil/refactor_20261027/compile_checks.ps1 -OutDir hil/refactor_20261027/rerun/compile
powershell -File hil/refactor_20261027/run_matrix.ps1 -OutDir hil/refactor_20261027/rerun/matrix
powershell -File hil/refactor_20261027/run_matrix.ps1 -RegistryOnly -ExpectedRegistryChecks 259 -OutDir hil/refactor_20261027/rerun/registry
powershell -File hil/refactor_20261027/run_port_matrix.ps1 -Suite all -OutDir hil/refactor_20261027/rerun/port
powershell -File hil/refactor_20261027/run_regressions.ps1 -Suite functional -OutDir hil/refactor_20261027/rerun/regression
powershell -File hil/refactor_20261027/run_regressions.ps1 -Suite production -OutDir hil/refactor_20261027/rerun/regression
powershell -File hil/refactor_20261027/run_regressions.ps1 -Suite soak -SoakSeconds 120 -OutDir hil/refactor_20261027/rerun/soak
powershell -File hil/restore_project.ps1 -Rebuild
```

最后一条只恢复、重建日常固件；恢复后仍需用 `run_hil.ps1` 重新烧录，并核对启动和任务输出。
本批 `verify_results.py` 的严格模式还要求恢复后的源码身份、日常固件与完整构建/串口证据。
仅有上面部分命令的输出不能生成整批 PASS。默认验收本批 `evidence/`，复测可用
`--evidence <新路径>`；运行期间用 `--in-progress` 查看缺失项，不能生成成功报告。

本批冻结的编译脚本保留首次 CM7 条目的字段形式，完整复测时还需真实执行带参数记录的补充编译。
在当前工具路径配置不变的电脑，可在严格编译之后运行以下步骤，将新输出保存到新批次：

```powershell
$cm7 = Get-Content hil/refactor_20261027/evidence/compile/cm7_supplement.json -Raw | ConvertFrom-Json
$cm7Arguments = @($cm7.arguments)
$cm7Output = & $cm7.compiler @cm7Arguments 2>&1
$cm7.exit_code = $LASTEXITCODE
$cm7.passed = ($cm7.exit_code -eq 0)
$cm7.warnings = @($cm7Output | Select-String 'warning:').Count
$cm7.sha256 = (Get-FileHash -LiteralPath $cm7.source).Hash
$cm7Output | Out-File hil/refactor_20261027/rerun/compile/compile_cm7_supplement.txt -Encoding utf8
$cm7 | ConvertTo-Json -Depth 5 | Set-Content hil/refactor_20261027/rerun/compile/cm7_supplement.json -Encoding utf8
if (-not $cm7.passed) { throw 'CM7 supplementary compilation failed' }
```

这里重用的是实际编译参数，新的成功状态、诊断日志和源码摘要来自再次执行，不复制旧通过结论。

## 检查层次

- 结构检查：263 个函数定义 token、63 项全局/静态声明、16 个公开头文件、主函数及第三方源码。
  任务 ID 原型由局部 `extern` 移至私有头；两个 USB 毫秒转换 wrapper 合并为已有 inline。
- 编译：16 个自研单元 × 两种屏蔽方式 × 两种断言设置 × O0/O2/O3，共 192 项；另有 M7 端口编译。
  本次首轮 M7 记录缺少参数，真实重跑并在 `cm7_supplement.json` 补录；不是 M7 硬件测试。
- 复杂 IPC：48 个继承/混合等待/生命周期/边界镜像，加 12 个最大互斥图容量镜像。
- 端口：12 个屏蔽交错、12 个阻塞返回、6 个非法嵌套、44 个 ISR 优先级与断言镜像。
- 功能与生产：5 个功能/底层/基准镜像，3 个 30 秒业务配置、1 个校验和负向对照及 120 秒压力。

`evidence_helpers.ps1` 在每次构建前后对固件 C/H/S、链接脚本、工程配置、模板和运行脚本取快照。
离线 Python 分析器不参与固件编译，身份单列于验收输出。独立验收复用固定摘要的历史原始日志规则，
仅适配 USB 断言来源的新文件名；历史分析器文件保持不变。

## 证据约束

`preflight/build_first_audit.txt` 保留首次完整链接结果：25 条原有演示代码未使用声明警告，另有
启动汇编 `-x assembler-with-cpp` 位于输入文件之后的警告。后者已在当前权威模板中修正为先指定
语言再给输入文件。自研内核严格编译不得存在警告；整工程的既有演示声明警告不记作内核零警告。

`preflight/before_guard_mapping_fix/` 保留 103 份早期 capture：case 13 把 CDC 查询的断言来源错误映射
到 OSAL，内核在正确的 device 入口断言，但夹具报 FAIL。修正映射并单独 smoke 通过后，最终 145 个镜像
全部重新采样，未沿用早期 PASS。日常采集另有结束文本不匹配记录，保留于
`preflight/daily_terminator_mismatch/`；核对 main 的 `task 0 running` 实际输出后重新采集通过。

文本日志、manifest、结果索引与方法入库；ELF、Debug 目标和 `.su` 为本机证据，遵守现有忽略规则。
隔离 IDE 的 38 份文本构建规则/对象清单精准放行；原始 `.recorded`、XML、生成规则和固定摘要分析器通过
`.gitattributes` 保留字节，二进制仍不入库。
缺少保留 ELF 时严格验收不能完成。原始文件换行改变后也不能直接沿用本机摘要。
有限 HIL 覆盖不能证明没有任何 BUG；USB 包装测试和日常 DWC2 初始化不等于物理 USB 主机互操作。
