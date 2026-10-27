# 有界故障记录与等待解释验证批次

本批次验证 `rtos_diag.c` 的记录完整性、等待解释、编译裁剪以及实际 F446 开销。
历史 `channel_20261027`、`channel_shared_20261027` 的接受规则和证据保持原样。

## 执行与证据

从工程根目录运行，`python` 可以替换为本机实际 Python 解释器：

```powershell
# 真实任务、SysTick、PendSV 和软件挂起的 TIM7 IRQ；需要 F446 ST-LINK / COM3。
python hil/diagnostics_20261027/run_suite.py diagnostic_checks --channels --diag on
python hil/diagnostics_20261027/run_suite.py diagnostic_checks --diag off
python hil/diagnostics_20261027/run_suite.py diagnostic_ring --diag on --capacity 8

# 两次执行完成相同工作，比较调用链包含真实调度的成本。
python hil/diagnostics_20261027/run_suite.py diagnostic_bench --diag off
python hil/diagnostics_20261027/run_suite.py diagnostic_bench --diag on

# 故意 UDF；UART 接受只证明已进入预期故障处理路径。
python hil/diagnostics_20261027/run_suite.py diagnostic_fault --fault --diag on

# 故障后立即只附加、不复位；使用刚才同一份已验证 ELF 读取 SRAM。
powershell -File hil/diagnostics_20261027/capture_fault_snapshot.ps1 `
  -CaptureDirectory hil/diagnostics_20261027/evidence/matrix/diagnostic_fault_on_c64_basepri_a1_O2
python hil/diagnostics_20261027/verify_fault_snapshot.py <fault_debug/本轮目录>

# 只读重新检查某个已完成的矩阵目录。
python hil/diagnostics_20261027/verify_results.py hil/diagnostics_20261027/evidence/matrix

# 无硬件：本机真实生产 C + 单线程 mock port。
python hil/diagnostics_20261027/host/run_host.py

# 无硬件：严格 ARM 编译及 OFF 与修改前代码比较。
python hil/diagnostics_20261027/strict/run_strict.py
python hil/diagnostics_20261027/check_disabled_equivalence.py

# 仅校验接受器能拒绝被人为破坏的输入，不是板上测试。
python hil/diagnostics_20261027/oracle_selftest.py
```

最终通过批次位于 `evidence/final/`，旧布局性能与初次联调保留为阶段记录；
结果与最终源码的对应关系见[验证报告](../../docs/rtos_diagnostics_validation_20261027.md)。
`run_matrix.ps1` 可一次运行功能、等工作量基准和 13 组集成回归；fault 模式最后运行，
读取 SRAM 后须执行 `hil/restore_project.ps1 -Rebuild` 并重新烧录日常固件。

本机 `baseline/` 的旧源码副本和 ELF 不随 Git 发布，保留 `sources.json` 身份清单。
完整 OFF 字节比较需要该原始快照；仅 clone 文本证据不能冒充已重新完成原始身份核对。

`run_suite.py` 通过既有 `hil/run_hil.ps1` 在烧录前打开串口，保存 ELF、原始串口、
编译详情、烧录验证、开始/结束时间和输入身份。输出目录必须不存在，失败记录也不会
被后来的 PASS 覆盖。当前约定的物理探针是序号 `066DFF555775514867092907`，串口
115200 8N1。不能并发运行两个板上任务，也不能由另一程序同时占用 COM3。

源文件、头文件、构建模板、夹具、驱动脚本和接受器在一次构建/捕获前后必须相同。
生产代码修改后需要新捕获。`identity.json` 只有 `passed=true`、`stage=complete`
并且所有已记录产物身份仍匹配时才能作为通过证据。

## 板上接受合同

| 夹具 | 固定检查 | 额外原始证据 |
| --- | ---: | --- |
| diagnostic_checks，ON | 472 | 28 个有顺序的 CASE、3 个实际 IRQ 结果 |
| diagnostic_checks，OFF | 11 | 所有接口返回 DISABLED，输出参数不改变 |
| diagnostic_ring，ON | 52 | 最小 8 槽覆盖、旧会话失效、实际 IRQ 拒绝/接纳 |
| diagnostic_bench | 72 | 6 条路径 × 3 轮，共 18 个等工作量 DWT 测量 |
| diagnostic_fault | 故意停机 | ARMED、UNDEFINSTR、完整故障报告及诊断集成提示 |

`contracts.json` 固定功能检查的名称及顺序。接受器还独立核对 CASE 中的操作、阶段、
唤醒/返回结果、延时取消、通道同址重建、晚运行超时以及历史缺失，不能仅凭 SUMMARY
中的 PASS 数通过。正常测试出现 ASSERT、FAULT、NESTED FAULT 或任意 FAIL 均拒绝。

基准的每一行执行 1024 次完整交换，校验和为 524800，要求错误数为零。DWT 计时包括
循环、API、调度和 SysTick 干扰，不能视为单个 API 指令数或最坏执行时间。三轮样本
可比较中位数，同时应保留全部原始样本。

故障路径的 UART 提示由集成代码打印，不能据此证明 RAM 中记录内容正确。需要另外
通过调试器读取同一已烧录 ELF 对应的 `diag_frozen`、`diag_fault_valid`、槽位 commit
及记录，保留调试命令与原始读数。终止故障测试完成后应重新烧录日常固件。

## 宿主与编译覆盖边界

宿主测试直接包含实际 `rtos_diag.c`，用独立 FIFO 参考模型检查覆盖、会话和保留数据。
白盒只用于让计数器接近耗尽、制造缺口和在三次提交边界模拟终止故障；不会把 mock
临界区声称为真实 Cortex-M 中断测试。矩阵是 O0/O3 × 容量 8/64/1024，以及两个 OFF
构建。覆盖九种等待操作、原因解释、缺失 RUN、不同操作 RETURN、取消/恢复、终态
保护、读取期间 reset，以及序列/会话/任务身份耗尽后的保守行为。

严格编译共 454 项：18 个自研 C 单元 × 诊断 ON/OFF × BASEPRI/PRIMASK × 断言
ON/OFF × O0/O2/O3，共 432；另有 18 个 M7 代码生成和四个 C11/C++17 公共头探针。
全部使用 `-Wall -Wextra -Werror`。M7 仍使用 F446 BSP 头文件，不能作为 H7 上板、
缓存、DMA 或 MPU 的验证。

OFF 比较将修改前的 17 个单元和当前相同单元用相同编译器、当前公共头和 O2/
CHANNEL=1/DIAGNOSTICS=0/ASSERT=0 编译，比较 ELF 对象的所有已分配节内容、尺寸、
对齐及重定位目标。断言关闭是为了排除增加源码行后必然改变的 `__LINE__` 常量。
独立检查新诊断模块 OFF 分支没有静态状态；应用主动调用诊断 API 时仍可链接很小的
DISABLED stub，因此不承诺调用 stub 本身完全不占指令。

这些测试没有使用 ASan/UBSan，不能保证发现所有 RTOS 缺陷；它们提供可重跑的明确
覆盖范围，真实中断与时序结果应以板上捕获和单独调试器证据为准。

故障接受器初版误把启动菜单中的 UNDEFINSTR 计入解码数量，原版保存在
`oracles/before_fault_context_fix/`。修正后只检查故障状态段；48 个损坏输入必须被拒绝，
其中包含“只有菜单、没有解码”。既有 32 个普通捕获通过新旧规则双重复核，
结果见 `reverification/20261027_170042_925970/`，原始 identity 未改写。
