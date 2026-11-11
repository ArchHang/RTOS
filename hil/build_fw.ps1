# hil\build_fw.ps1
# Build one firmware variant of the f446_rtos project.
#
# The variant is chosen purely by preprocessing macros (-DFIRMWARE_*), so no
# source file has to be edited between runs. hil\restore_project.ps1 puts the
# generated build files back to the canonical daily build templates.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File hil\build_fw.ps1 -Variant daily
#   powershell -ExecutionPolicy Bypass -File hil\build_fw.ps1 -Variant scenario -Scenario 2 -Clean
param(
    [Parameter(Mandatory = $true)][string] $Variant,
    [switch] $Clean,
    [int]    $Scenario = 1,
    [uint32] $StressSeed = 1,
    [ValidateRange(6,900)][int] $StressSeconds = 30,
    [ValidateRange(1,64)][int] $StressQueueCapacity = 8,
    [ValidateSet(512,768)][int] $CdcRxCapacity = 768,
    [switch] $StressInjectCorruption,
    [ValidateSet('basepri','primask')][string] $CriticalMask = 'basepri',
    [switch] $DisableAssertions,
    [switch] $UseVectorWrappers,
    [switch] $Cooperative,
    [switch] $PerfHotpath,
    [ValidateSet(115200,460800,921600)][uint32] $UartBaud = 115200,
    [ValidateSet('O2','O3')][string] $UartModelOptimization = 'O2',
    [ValidateSet('task','fpu','isr')][string] $FaultContext = 'task',
    [ValidateSet('O0','O2','O3')][string] $Optimization = 'O2',
    [ValidateRange(1,16)][int] $CriticalGuardCase = 1,
    [ValidateRange(0,15)][int] $CriticalGuardPriority = 0
)
$ErrorActionPreference = 'Stop'

$ROOT  = Split-Path $PSScriptRoot -Parent
$HIL   = Join-Path $ROOT 'hil'
$DBG   = Join-Path $ROOT 'Debug'
$LOCAL = Join-Path $HIL 'fwsrc'
. (Join-Path $HIL 'build_layout.ps1')
$TOOLS = 'E:\ST\STM32CubeIDE_1.13.2\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.11.3.rel1.win32_1.1.100.202309141235\tools\bin'
$MAKE  = 'E:\ST\STM32CubeIDE_1.13.2\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.make.win32_2.1.100.202310302056\tools\bin\make.exe'
$env:PATH = "$TOOLS;$env:PATH"

# ---------------------------------------------------------------- manifest ---
# flags   : -D switches applied to every Core/Src and hil/fwsrc compilation
# exclude : Core/Src translation units dropped from this variant
# extra   : source basenames copied hil\fw\Src -> hil\fwsrc and compiled
$VARIANTS = @{
    systick_empty_bench = @{
        desc = 'Measure real SysTick with empty and future-only delay queues'
        flags = @('FIRMWARE_AUDIT=1','FIRMWARE_USB_APP=0',
            'FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0')
        exclude = @('benchmark')
        extra = @('systick_empty_bench')
    }
    task_registry_bench = @{
        desc = 'DWT timing of task notification by registered handle'
        flags = @('FIRMWARE_AUDIT=1','FIRMWARE_USB_APP=0',
            'FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0')
        exclude = @('benchmark')
        extra = @('task_registry_bench')
    }
    usb_cdc_cursor_regression = @{
        desc = 'CDC byte delivery across receive cursor wrap'
        flags = @('FIRMWARE_AUDIT=1','FIRMWARE_USB_APP=0',
            'FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0')
        exclude = @('benchmark')
        extra = @('usb_cdc_cursor_regression')
    }
    usb_osal_priority_regression = @{
        desc = 'USB scheduling helper preserves inherited mutex priority'
        flags = @('FIRMWARE_AUDIT=1','FIRMWARE_USB_APP=0',
            'FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0')
        exclude = @('benchmark')
        extra = @('usb_osal_priority_regression')
    }
    sem_wake_bench = @{
        desc = 'DWT timing of signal delivery with multiple real waiters'
        flags = @('FIRMWARE_AUDIT=1','FIRMWARE_USB_APP=0',
            'FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0')
        exclude = @('benchmark')
        extra = @('sem_wake_bench')
    }
    timer_dispatch_bench = @{
        desc = 'DWT timing of timer service returning to its command wait'
        flags = @('FIRMWARE_AUDIT=1','FIRMWARE_USB_APP=0',
            'FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0')
        exclude = @('benchmark')
        extra = @('timer_dispatch_bench')
    }
    perf_snapshot_bench = @{
        desc = 'DWT timing of single and batched task performance snapshots'
        flags = @('FIRMWARE_AUDIT=1','FIRMWARE_USB_APP=0',
            'FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0')
        exclude = @('benchmark')
        extra = @('perf_snapshot_bench')
    }
    mutex_release_bench = @{
        desc = 'DWT timing of final mutex release with an empty wait list'
        flags = @('FIRMWARE_AUDIT=1','FIRMWARE_USB_APP=0',
            'FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0')
        exclude = @('benchmark')
        extra = @('mutex_release_bench')
    }
    tick_timeout_bench = @{
        desc = 'DWT timing of simultaneous task timeouts in the tick path'
        flags = @('FIRMWARE_AUDIT=1','FIRMWARE_USB_APP=0',
            'FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0')
        exclude = @('benchmark')
        extra = @('tick_timeout_bench')
    }
    wrap_ipc_regression = @{
        desc = 'Real SysTick rollover with simultaneous IPC timeout and IRQ completion'
        flags = @('FIRMWARE_AUDIT=1','FIRMWARE_USB_APP=0',
            'FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0')
        exclude = @('benchmark')
        extra = @('wrap_ipc_regression')
    }
    pool_free_bench = @{
        desc = 'DWT timing of fixed pool block validation and release'
        flags = @('FIRMWARE_AUDIT=1','FIRMWARE_USB_APP=0',
            'FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0')
        exclude = @('benchmark')
        extra = @('pool_free_bench')
    }
    adc_capture = @{
        desc = 'TIM2 paced ADC and DMA pipeline with fixed block ownership'
        flags = @('FIRMWARE_AUDIT=1','FIRMWARE_USB_APP=0',
            'FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0')
        exclude = @('benchmark')
        extra = @('project_adc_capture')
    }
    bus_recovery = @{
        desc = 'shared bus handoff during device shutdown and reinitialization'
        flags = @('FIRMWARE_AUDIT=1','FIRMWARE_USB_APP=0',
            'FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0')
        exclude = @('benchmark')
        extra = @('project_bus_recovery')
    }
    timer_supervisor = @{
        desc = 'software lease expiry while the timer daemon is preempted'
        flags = @('FIRMWARE_AUDIT=1','FIRMWARE_USB_APP=0',
            'FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0')
        exclude = @('benchmark')
        extra = @('project_timer_supervisor')
    }
    uart_pipeline = @{
        desc = 'USART2 framed telemetry and on-device CNN application'
        flags = @('FIRMWARE_AUDIT=1','FIRMWARE_UART_PIPELINE=1','FIRMWARE_USB_APP=0',
            'FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0',"UART_PIPELINE_BAUD=${UartBaud}U")
        exclude = @('benchmark')
        extra = @('uart_pipeline')
    }
    queue_copy = @{
        desc = 'byte and unaligned queue copies across wrap and real waiters'
        flags = @('FIRMWARE_AUDIT=1','FIRMWARE_USB_APP=0',
            'FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0')
        exclude = @('benchmark')
        extra = @('queue_copy_probe')
    }
    fault_diagnostics = @{
        desc = 'hardware UsageFault and stack/mask report'
        flags = @('FIRMWARE_AUDIT=1','FIRMWARE_USB_APP=0','FIRMWARE_FAULT_TASKS=0',
            'FIRMWARE_APP_TASKS=0',"FAULT_DIAG_$($FaultContext.ToUpperInvariant())=1")
        exclude = @('benchmark')
        extra = @('fault_report_probe')
    }
    startup_clock = @{
        desc = 'SysTick clock rejection before scheduling state is published'
        flags = @('FIRMWARE_AUDIT=1','FIRMWARE_USB_APP=0',
            'FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0')
        exclude = @('benchmark')
        extra = @('startup_clock_regression')
    }
    robustness = @{
        desc = 'task storage, interrupt masks, scheduling, allocator and service regressions'
        flags = @('FIRMWARE_AUDIT=1','FIRMWARE_ROBUSTNESS=1','FIRMWARE_USB_APP=0',
            'FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0')
        exclude = @('benchmark')
        extra = @('robustness_runner','robustness_core','robustness_memory','robustness_services')
    }
    robustness_msp = @{
        desc = 'SVC boot alignment and PendSV MSP balance through real vector entries'
        flags = @('FIRMWARE_AUDIT=1','FIRMWARE_USB_APP=0',
            'FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0')
        exclude = @('benchmark')
        extra = @('robustness_msp')
    }
    adversarial_registry = @{
        desc = 'maximum mutex owner graph and deletion with multiple owned locks'
        flags = @('FIRMWARE_AUDIT=1','FIRMWARE_USB_APP=0',
            'FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0')
        exclude = @('benchmark')
        extra = @('adversarial_registry')
    }
    adversarial_mutex_boundary = @{
        desc = 'recursive count overflow, owner deletion and finite mutex dependency cycles'
        flags = @('FIRMWARE_AUDIT=1','FIRMWARE_USB_APP=0',
            'FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0')
        exclude = @('benchmark')
        extra = @('adversarial_mutex_boundary')
    }
    adversarial_lifetime = @{
        desc = 'legal ISR deinitialization in the object liveness-check window'
        flags = @('FIRMWARE_AUDIT=1','FIRMWARE_ADVERSARIAL_LIFETIME=1',
            'RTOS_HIL_OBJECT_LIFETIME=1','FIRMWARE_USB_APP=0',
            'FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0')
        exclude = @('benchmark')
        extra = @('adversarial_lifetime')
    }
    adversarial_mutex = @{
        desc = 'bounded mutex dependency chains and inherited-priority withdrawal'
        flags = @('FIRMWARE_AUDIT=1','FIRMWARE_ADVERSARIAL_MUTEX=1','FIRMWARE_USB_APP=0',
            'FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0')
        exclude = @('benchmark')
        extra = @('adversarial_mutex')
    }
    adversarial_wait = @{
        desc = 'mixed ISR wait races, exact-once handoff and object lifecycle'
        flags = @('FIRMWARE_AUDIT=1','FIRMWARE_ADVERSARIAL_WAIT=1','FIRMWARE_USB_APP=0',
            'FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0')
        exclude = @('benchmark')
        extra = @('adversarial_wait','production_highirq')
    }
    basepri = @{
        desc = 'deterministic critical-state preemption and interrupt-mask regression'
        flags = @('FIRMWARE_AUDIT=1','FIRMWARE_USB_APP=0','FIRMWARE_FAULT_TASKS=0',
            'FIRMWARE_APP_TASKS=0','RTOS_HIL_CRITICAL_TEST=1')
        exclude = @('benchmark')
        extra = @('basepri_regressions')
    }
    basepri_guard = @{
        desc = 'real IRQ syscall-priority assertions and invalid critical nesting'
        flags = @('FIRMWARE_AUDIT=1','FIRMWARE_USB_APP=0','FIRMWARE_FAULT_TASKS=0',
            'FIRMWARE_APP_TASKS=0',"BASEPRI_GUARD_CASE=${CriticalGuardCase}U",
            "BASEPRI_GUARD_PRIORITY=${CriticalGuardPriority}U")
        exclude = @('benchmark')
        extra = @('basepri_guard')
    }
    basepri_blocking = @{
        desc = 'blocking return values must be read after the pending context switch'
        flags = @('FIRMWARE_AUDIT=1','FIRMWARE_USB_APP=0','FIRMWARE_FAULT_TASKS=0',
            'FIRMWARE_APP_TASKS=0','FIRMWARE_BASEPRI_BLOCKING=1')
        exclude = @('benchmark')
        extra = @('basepri_blocking','production_highirq')
    }
    basepri_nesting = @{
        desc = 'assertion-disabled invalid critical nesting preserves caller state'
        flags = @('FIRMWARE_AUDIT=1','FIRMWARE_USB_APP=0','FIRMWARE_FAULT_TASKS=0',
            'FIRMWARE_APP_TASKS=0','RTOS_CONFIG_ASSERT_ENABLED=0')
        exclude = @('benchmark')
        extra = @('basepri_nesting')
    }
    audit = @{
        desc = '2026-10-12 kernel audit regressions'
        flags = @('FIRMWARE_AUDIT=1','FIRMWARE_USB_APP=0','FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0','KERN_IS_CUSTOM_RTOS=1','RTOS_HIL_AUDIT_PENDSV=1','FIRMWARE_AUDIT_REPEAT_TIMER_INIT=1')
        exclude = @('benchmark'); extra = @('audit_regressions','audit_ipc','audit_memory_timer','audit_usb_port','kern_port_rtos')
    }
    daily = @{
        desc    = 'daily demo firmware (app tasks + HardFault tests + USB)'
        flags   = @(); exclude = @(); extra = @()
    }
    selftest = @{
        desc    = 'full functional regression (selftest.c)'
        flags   = @('FIRMWARE_SELFTEST=1','FIRMWARE_USB_APP=0','FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0')
        exclude = @(); extra = @()
    }
    bench = @{
        desc    = 'micro benchmarks BM0..BM7 (benchmark.c)'
        flags   = @('FIRMWARE_BENCH=1','FIRMWARE_USB_APP=0','FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0')
        exclude = @(); extra = @()
    }
    wake_bench = @{
        desc = 'DWT matrix for task, queue and ISR wake paths plus isolated semaphore give'
        flags = @('FIRMWARE_BENCH=1','FIRMWARE_WAKE_BENCH=1','FIRMWARE_USB_APP=0',
            'FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0')
        exclude = @(); extra = @()
    }
    perf = @{
        desc    = 'controlled performance comparison plus mutex robustness probes'
        flags   = @('FIRMWARE_BENCH=1','FIRMWARE_PERF=1','FIRMWARE_USB_APP=0','FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0')
        exclude = @(); extra = @('perf_probes')
    }
    lowlevel = @{
        desc = 'context-switch return paths and real S16..S31 preservation checks'
        flags = @('FIRMWARE_BENCH=1','FIRMWARE_PERF=1','FIRMWARE_LOWLEVEL_CHECKS=1','FIRMWARE_USB_APP=0','FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0')
        exclude = @(); extra = @('perf_probes','lowlevel_checks','lowlevel_fpu_probe')
    }
    flash_diag = @{
        desc = 'same-image Flash instruction-cache ON/OFF/ON diagnostic'
        flags = @('FIRMWARE_BENCH=1','FIRMWARE_FLASH_DIAG=1','FIRMWARE_USB_APP=0','FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0')
        exclude = @(); extra = @('flash_diag')
    }
    production_stress = @{
        desc = 'industrial pipeline: sixteen priorities, shared IPC and three IRQ levels'
        flags = @('FIRMWARE_PRODUCTION_STRESS=1','FIRMWARE_USB_APP=0',
            'FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0',
            "PRODUCTION_STRESS_SEED=${StressSeed}U",
            "PRODUCTION_STRESS_SECONDS=$StressSeconds",
            "PRODUCTION_STRESS_QUEUE_CAPACITY=$StressQueueCapacity",
            "PRODUCTION_STRESS_INJECT_CORRUPTION=$([int][bool]$StressInjectCorruption)")
        exclude = @('benchmark')
        extra = @('production_stress','production_irq','production_highirq')
    }
    boundary = @{
        desc    = 'boundary / contract fixture (hil\fw\Src\boundary_test.c)'
        flags   = @('FIRMWARE_BOUNDARY=1','FIRMWARE_USB_APP=0','FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0')
        exclude = @(); extra = @('boundary_test')
    }
    scenario = @{
        desc    = 'system-level scenario suite (S1..S5)'
        flags   = @('FIRMWARE_SCENARIO=1','FIRMWARE_USB_APP=0','FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0',
                    'KERN_IS_CUSTOM_RTOS=1', "TEST_SCENARIO_SELECT=$Scenario")
        exclude = @('benchmark')
        extra   = @('kern_port_rtos','test_framework','test_scenarios')
    }
    # Same as 'scenario' but keeps the USB device stack + its application tasks
    # alive, i.e. the environment the original scenario report was captured in.
    scenario_usb = @{
        desc    = 'system-level scenario suite WITH USB app tasks (original setup)'
        flags   = @('FIRMWARE_SCENARIO=1','FIRMWARE_USB_APP=1','FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0',
                    'KERN_IS_CUSTOM_RTOS=1', "TEST_SCENARIO_SELECT=$Scenario")
        exclude = @('benchmark')
        extra   = @('kern_port_rtos','test_framework','test_scenarios')
    }
    # Audit-only diagnostic scenarios (D1/D4-D10) used to validate the fixtures themselves.
    diag = @{
        desc    = 'audit diagnostic scenarios D1/D4-D10'
        flags   = @('FIRMWARE_SCENARIO=1','FIRMWARE_USB_APP=0','FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0',
                    'KERN_IS_CUSTOM_RTOS=1','HIL_DIAG=1', "TEST_SCENARIO_SELECT=$Scenario")
        exclude = @('benchmark')
        extra   = @('kern_port_rtos','test_framework','test_scenarios','diag_scenarios')
    }
}
if (-not $VARIANTS.ContainsKey($Variant)) {
    throw "unknown variant '$Variant'; known: $($VARIANTS.Keys -join ', ')"
}
$cfg = $VARIANTS[$Variant]
if ($Variant -eq 'basepri_guard' -and $DisableAssertions) {
    throw 'basepri_guard requires assertions; use basepri for assertion-disabled mask tests'
}
if ($UseVectorWrappers -and $Variant -ne 'robustness_msp') {
    throw 'UseVectorWrappers is specific to the robustness_msp variant'
}
if ($Cooperative -and $Variant -ne 'robustness') {
    throw 'Cooperative is specific to the robustness variant'
}
if ($PerfHotpath -and $Variant -notin @('robustness','perf_snapshot_bench')) {
    throw 'PerfHotpath requires robustness or perf_snapshot_bench'
}

# Recreate every generated directory from committed templates when Debug is absent.
Initialize-HilBuildTree -ProjectRoot $ROOT -DebugRoot $DBG
$startupTemplate = Join-Path $HIL 'templates/Core_Startup.mk'
$startupRule = [IO.File]::ReadAllText($startupTemplate)
if ($UseVectorWrappers) {
    $startupRule = $startupRule.Replace('-DDEBUG', '-DDEBUG -Wa,--defsym,RTOS_HIL_VECTOR_WRAPPERS=1')
}
Set-HilBuildRule -ProjectRoot $ROOT -DebugRoot $DBG -Relative 'Core/Startup' -Content $startupRule
$modelRule = [IO.File]::ReadAllText((Join-Path $HIL 'templates/AI_output_int8.mk'))
if ($Variant -eq 'uart_pipeline') {
    # Keep model arithmetic comparable to the independently compiled host model.
    $modelRule = $modelRule.Replace('-O2', "-$UartModelOptimization -ffp-contract=off")
}
Set-HilBuildRule -ProjectRoot $ROOT -DebugRoot $DBG -Relative 'AI/output/int8' -Content $modelRule

# These overrides are opt-in and are propagated to all kernel translation units.
$configFlags = @()
if ($CriticalMask -eq 'primask') { $configFlags += '-DRTOS_CONFIG_USE_BASEPRI=0' }
if ($Cooperative) { $configFlags += '-DRTOS_CONFIG_USE_PREEMPTION=0' }
if ($PerfHotpath) { $configFlags += '-DRTOS_CONFIG_PERF_HOTPATH_STATS=1' }
if ($Variant -eq 'startup_clock') { $configFlags += '-DRTOS_CONFIG_TICK_RATE_HZ=100U' }
if ($DisableAssertions -or $Variant -eq 'basepri_nesting') {
    $configFlags += '-DRTOS_CONFIG_ASSERT_ENABLED=0'
}
if ($Variant -eq 'basepri') { $configFlags += '-DRTOS_HIL_CRITICAL_TEST=1' }
if ($Variant -eq 'adversarial_lifetime') { $configFlags += '-DRTOS_HIL_OBJECT_LIFETIME=1' }
if ($Variant -eq 'usb_cdc_cursor_regression') {
    $configFlags += @('-DRTOS_HIL_USB_CDC_CURSOR_TEST=1', "-DRTOS_CONFIG_USB_CDC_RX_BUF_SIZE=$($CdcRxCapacity)U")
}
$configArgs = $configFlags -join ' '

# The audit injects a real syscall-priority IRQ at the PendSV decision window.
# Keep this hook out of every production build and recompile on macro changes.
foreach ($rule in @(Get-HilBuildRules | Where-Object { $_.Relative.StartsWith('RTOS/') })) {
    $kernelTpl = Join-Path $HIL ('templates/' + $rule.Template)
    $kernelRaw = [System.IO.File]::ReadAllText($kernelTpl)
    if ($Variant -eq 'audit') {
        $kernelRaw = $kernelRaw.Replace('-DDEBUG', '-DDEBUG -DRTOS_HIL_AUDIT_PENDSV=1')
    }
    if ($configArgs -ne '') { $kernelRaw = $kernelRaw.Replace('-DDEBUG', "-DDEBUG $configArgs") }
    $kernelRaw = $kernelRaw -replace '-O[0-3sg]\b', "-$Optimization"
    Set-HilBuildRule -ProjectRoot $ROOT -DebugRoot $DBG -Relative $rule.Relative -Content $kernelRaw
}
$auditLinkMk = Join-Path $DBG 'makefile'
$auditLinkTpl = Join-Path $HIL 'templates/makefile.template'
$auditLinkRaw = [System.IO.File]::ReadAllText($auditLinkTpl)
if ($Variant -eq 'audit') {
    $auditLinkRaw = $auditLinkRaw.Replace('-Wl,--gc-sections', '-Wl,--gc-sections -Wl,--wrap=usbd_ep_start_write -Wl,--wrap=usbd_ep_start_read')
}
if ($Variant -eq 'basepri_guard') {
    $auditLinkRaw = $auditLinkRaw.Replace('-Wl,--gc-sections', '-Wl,--gc-sections -Wl,--wrap=rtos_assert_fail')
}
if ($Variant -eq 'usb_cdc_cursor_regression') {
    $auditLinkRaw = $auditLinkRaw.Replace('-Wl,--gc-sections',
        '-Wl,--gc-sections -Wl,--wrap=usbd_ep_start_read')
}
if ($Variant -in @('timer_supervisor','timer_dispatch_bench')) {
    $auditLinkRaw = $auditLinkRaw.Replace('-Wl,--gc-sections',
        '-Wl,--gc-sections -Wl,--wrap=rtos_queue_recv -Wl,--wrap=rtos_internal_queue_recv_until')
}
if ($Variant -in @('tick_timeout_bench','wrap_ipc_regression','systick_empty_bench')) {
    $auditLinkRaw = $auditLinkRaw.Replace('-Wl,--gc-sections', '-Wl,--gc-sections -Wl,--wrap=rtos_sched_tick')
}
[System.IO.File]::WriteAllText($auditLinkMk, $auditLinkRaw)

if (-not (Test-Path $LOCAL)) { New-Item -ItemType Directory -Path $LOCAL | Out-Null }
# make does not create the object directory for a pattern rule; do it here.
$objDir = Join-Path $DBG 'hil\fwsrc'
if (-not (Test-Path $objDir)) { New-Item -ItemType Directory -Path $objDir | Out-Null }
foreach ($b in $cfg.extra) {
    $sourcePath = Join-Path $HIL "fw\Src\$b.c"
    if (-not (Test-Path -LiteralPath $sourcePath)) {
        $sourcePath = Join-Path $HIL "fw\Port\ARM\$b.c"
    }
    if (-not (Test-Path -LiteralPath $sourcePath)) {
        throw "Variant source missing: $b"
    }
    Copy-Item -LiteralPath $sourcePath -Destination (Join-Path $LOCAL "$b.c") -Force
}

# ------------------------------------------------------------ regenerate mk ---
# Everything is regenerated from the canonical application rule, so repeated
# builds never accumulate edits.
$origMk = Join-Path $HIL 'templates\subdir.mk.template'
if (-not (Test-Path -LiteralPath $origMk)) { throw "Missing template: $origMk" }
$lines = [System.IO.File]::ReadAllLines($origMk)

$exSrc = @(); $exObj = @(); $exDep = @()
foreach ($e in $cfg.exclude) {
    $exSrc += "../Core/Src/$e.c"; $exObj += "./Core/Src/$e.o"; $exDep += "./Core/Src/$e.d"
}

$flagArgs = ($cfg.flags | ForEach-Object { "-D$_" }) -join ' '
if ($cfg.flags -contains 'FIRMWARE_BENCH=1') {
    $flagArgs += " -DRTOS_HIL_BENCH_OPT=$($Optimization.Substring(1))"
}
if ($configArgs -ne '') { $flagArgs += " $configArgs" }
$incExtra = '-I"../hil/fwsrc" -I"../hil/fw/Inc"'

$out = New-Object System.Collections.Generic.List[string]
$mode = ''
foreach ($l in $lines) {
    if ($l -match '^C_SRCS') { $mode = 'src'; $out.Add($l); continue }
    if ($l -match '^OBJS')   { $mode = 'obj'; $out.Add($l); continue }
    if ($l -match '^C_DEPS') { $mode = 'dep'; $out.Add($l); continue }
    if ($l.Trim() -eq '')    { $mode = '';    $out.Add($l); continue }

    if ($l -match '^\s*arm-none-eabi-gcc') {
        $nl = $l
        $nl = $nl -replace '-O[0-3sg]\b', "-$Optimization"
        if ($flagArgs -ne '') { $nl = $nl -replace '-DDEBUG', ("-DDEBUG " + $flagArgs) }
        $nl += " $incExtra"
        $out.Add($nl)
        continue
    }

    $skip = $false
    foreach ($x in ($exSrc + $exObj + $exDep)) { if ($l.Trim() -eq "$x \") { $skip = $true } }
    if (-not $skip) { $out.Add($l) }
}

if ($cfg.extra.Count -gt 0) {
    $rule = ($out | Where-Object { $_ -match '^\s*arm-none-eabi-gcc' } | Select-Object -First 1)
    if ($Variant -in @('systick_empty_bench','task_registry_bench',
        'usb_cdc_cursor_regression','usb_osal_priority_regression',
        'wrap_ipc_regression','sem_wake_bench','timer_dispatch_bench','perf_snapshot_bench',
        'mutex_release_bench','tick_timeout_bench','pool_free_bench','adc_capture',
        'bus_recovery','timer_supervisor','uart_pipeline','queue_copy','fault_diagnostics','startup_clock',
        'robustness','robustness_msp','lowlevel','flash_diag','production_stress','basepri','basepri_guard',
        'basepri_blocking','basepri_nesting','adversarial_mutex','adversarial_wait','adversarial_lifetime',
        'adversarial_mutex_boundary','adversarial_registry')) {
        # Enforce the requested warning policy for the new low-level fixtures.
        $rule = $rule.Replace('-Wall', '-Wall -Wextra -Werror')
    }
    $out.Add('')
    $out.Add('# HIL_VARIANT: variant-specific sources compiled from hil\fwsrc')
    $out.Add('OBJS += \')
    foreach ($b in $cfg.extra) { $out.Add("./hil/fwsrc/$b.o \") }
    $out.Add('')
    $out.Add('C_DEPS += \')
    foreach ($b in $cfg.extra) { $out.Add("./hil/fwsrc/$b.d \") }
    $out.Add('')
    $out.Add('hil/fwsrc/%.o: ../hil/fwsrc/%.c')
    $out.Add("`t$rule")
    $out.Add('')
}
$appRule = ($out -join [Environment]::NewLine) + [Environment]::NewLine
Set-HilBuildRule -ProjectRoot $ROOT -DebugRoot $DBG -Relative 'Core/Src' -Content $appRule

# objects.list is regenerated from the complete canonical template:
# an incremental filter loses entries permanently the first time a variant
# excludes a file, which silently breaks later variants (observed: benchmark.o
# vanished after the scenario build and the bench variant then failed to link).
$baseOl = Join-Path $HIL 'templates/objects.list.template'
if (-not (Test-Path -LiteralPath $baseOl)) { throw "Missing template: $baseOl" }
$ol = New-Object System.Collections.Generic.List[string]
foreach ($l in (Get-Content $baseOl)) {
    $skip = $false
    foreach ($x in $exObj) { if ($l -match [regex]::Escape($x)) { $skip = $true } }
    if (-not $skip) { $ol.Add($l) }
}
foreach ($b in $cfg.extra) { $ol.Add("./hil/fwsrc/$b.o") }
Set-Content (Join-Path $DBG 'objects.list') -Value $ol

# ---------------------------------------------------------------- build ------
# make does not track macro changes, so a variant/scenario switch must not be
# allowed to reuse objects compiled with the previous -D set. Keep a stamp of
# the last build and force-recompile the affected translation units when it
# changes (a stale main.o silently runs the previous scenario).
$stampFile = Join-Path $DBG '.hil_build_stamp'
$stamp = "$Variant|$Scenario|$($cfg.flags -join ',')|$configArgs|$Optimization|$UseVectorWrappers"
$prev = if (Test-Path $stampFile) { (Get-Content $stampFile -Raw).Trim() } else { '' }
if ($prev -ne $stamp) {
    foreach ($f in @('Core\Src\main.o','Core\Src\main.d')) {
        Remove-Item (Join-Path $DBG $f) -Force -ErrorAction SilentlyContinue
    }
    foreach ($b in $cfg.extra) {
        Remove-Item (Join-Path $DBG "hil\fwsrc\$b.o") -Force -ErrorAction SilentlyContinue
        Remove-Item (Join-Path $DBG "hil\fwsrc\$b.d") -Force -ErrorAction SilentlyContinue
    }
    Set-Content $stampFile -Value $stamp
    Write-Host "variant changed -> forced recompile of main.c and variant sources"
}

$ErrorActionPreference = 'Continue'  # make 的 clean 提示走 stderr, 会被 Stop 误判为终止错误
if ($Clean) { & $MAKE -C $DBG clean 2>&1 | Out-Null }
$ErrorActionPreference = 'Continue'
$b = & $MAKE -C $DBG all -j8 2>&1
$code = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
$buildLog = Join-Path $HIL ("logs/build_${Variant}_${Scenario}.txt")
if (-not (Test-Path -LiteralPath (Split-Path $buildLog))) { New-Item -ItemType Directory -Path (Split-Path $buildLog) | Out-Null }
$b | Out-File -LiteralPath $buildLog -Encoding utf8
$warn = ($b | Select-String 'warning:').Count
if ($code -ne 0) {
    Write-Host "BUILD FAILED ($Variant)"
    $b | Select-String 'error|undefined|No rule|Interrupt' | Select-Object -First 12 | ForEach-Object { Write-Host $_ }
    exit 1
}
Write-Host "built '$Variant' OK -- $($cfg.desc); warnings=$warn"
& "$TOOLS\arm-none-eabi-size.exe" (Join-Path $DBG 'f446_rtos.elf')
