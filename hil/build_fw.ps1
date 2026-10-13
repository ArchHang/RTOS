# hil\build_fw.ps1
# Build one firmware variant of the f446_rtos project.
#
# The variant is chosen purely by preprocessing macros (-DFIRMWARE_*), so no
# source file has to be edited between runs. hil\restore_project.ps1 puts the
# generated build files back to their pristine CubeIDE state.
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
    [switch] $StressInjectCorruption
)
$ErrorActionPreference = 'Stop'

$ROOT  = 'E:\STM32_Project\CubeIDE\f446_rtos'
$HIL   = Join-Path $ROOT 'hil'
$DBG   = Join-Path $ROOT 'Debug'
$LOCAL = Join-Path $HIL 'fwsrc'
$TOOLS = 'E:\ST\STM32CubeIDE_1.13.2\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.11.3.rel1.win32_1.1.100.202309141235\tools\bin'
$MAKE  = 'E:\ST\STM32CubeIDE_1.13.2\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.make.win32_2.1.100.202310302056\tools\bin\make.exe'
$env:PATH = "$TOOLS;$env:PATH"

# ---------------------------------------------------------------- manifest ---
# flags   : -D switches applied to every Core/Src and hil/fwsrc compilation
# exclude : Core/Src translation units dropped from this variant
# extra   : source basenames copied hil\fw\Src -> hil\fwsrc and compiled
$VARIANTS = @{
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
        desc = 'industrial pipeline: sixteen priorities, shared IPC and nested timer IRQs'
        flags = @('FIRMWARE_PRODUCTION_STRESS=1','FIRMWARE_USB_APP=0',
            'FIRMWARE_FAULT_TASKS=0','FIRMWARE_APP_TASKS=0',
            "PRODUCTION_STRESS_SEED=${StressSeed}U",
            "PRODUCTION_STRESS_SECONDS=$StressSeconds",
            "PRODUCTION_STRESS_QUEUE_CAPACITY=$StressQueueCapacity",
            "PRODUCTION_STRESS_INJECT_CORRUPTION=$([int][bool]$StressInjectCorruption)")
        exclude = @('benchmark')
        extra = @('production_stress','production_irq')
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

# The audit injects a real syscall-priority IRQ at the PendSV decision window.
# Keep this hook out of every production build and recompile on macro changes.
foreach ($rel in @('RTOS/Core/Src','RTOS/Port/GCC/ARM_CM7','RTOS/lib/third/CherryUSB/class/msc','RTOS/lib/third/CherryUSB/core','RTOS/lib/third/CherryUSB/port/dwc2')) {
    $kernelMk = Join-Path $DBG "$rel/subdir.mk"
    $kernelTpl = Join-Path $HIL ('templates/' + ($rel -replace '/', '_') + '.mk')
    if (-not (Test-Path -LiteralPath $kernelTpl)) { Copy-Item -LiteralPath $kernelMk -Destination $kernelTpl }
    $kernelRaw = [System.IO.File]::ReadAllText($kernelTpl)
    if ($Variant -eq 'audit') { $kernelRaw = $kernelRaw.Replace('-DDEBUG', '-DDEBUG -DRTOS_HIL_AUDIT_PENDSV=1') }
    if ([System.IO.File]::ReadAllText($kernelMk) -ne $kernelRaw) {
        [System.IO.File]::WriteAllText($kernelMk, $kernelRaw)
        Get-ChildItem -LiteralPath (Join-Path $DBG $rel) -File | Where-Object { $_.Extension -in '.o','.d' } | Remove-Item -Force
    }
}
$auditLinkMk = Join-Path $DBG 'makefile'
$auditLinkTpl = Join-Path $HIL 'templates/makefile.template'
if (-not (Test-Path -LiteralPath $auditLinkTpl)) { Copy-Item -LiteralPath $auditLinkMk -Destination $auditLinkTpl }
$auditLinkRaw = [System.IO.File]::ReadAllText($auditLinkTpl)
if ($Variant -eq 'audit') {
    $auditLinkRaw = $auditLinkRaw.Replace('-Wl,--gc-sections', '-Wl,--gc-sections -Wl,--wrap=usbd_ep_start_write -Wl,--wrap=usbd_ep_start_read')
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
# Everything is regenerated from the archived pristine CubeIDE file, so repeated
# builds never accumulate edits.
$origMk = Join-Path $HIL 'templates\subdir.mk.template'
if (-not (Test-Path $origMk)) {
    $raw = [System.IO.File]::ReadAllText((Join-Path $DBG 'Core\Src\subdir.mk'))
    $raw = ($raw -split "`r?`n" | Where-Object { $_ -notmatch 'hil/fwsrc' -and $_ -notmatch 'HIL_VARIANT' }) -join "`r`n"
    [System.IO.File]::WriteAllText($origMk, $raw)
}
$lines = [System.IO.File]::ReadAllLines($origMk)

$exSrc = @(); $exObj = @(); $exDep = @()
foreach ($e in $cfg.exclude) {
    $exSrc += "../Core/Src/$e.c"; $exObj += "./Core/Src/$e.o"; $exDep += "./Core/Src/$e.d"
}

$flagArgs = ($cfg.flags | ForEach-Object { "-D$_" }) -join ' '
$incExtra = '-I"E:/STM32_Project/CubeIDE/f446_rtos/hil/fwsrc" -I"E:/STM32_Project/CubeIDE/f446_rtos/hil/fw/Inc"'

$out = New-Object System.Collections.Generic.List[string]
$mode = ''
foreach ($l in $lines) {
    if ($l -match '^C_SRCS') { $mode = 'src'; $out.Add($l); continue }
    if ($l -match '^OBJS')   { $mode = 'obj'; $out.Add($l); continue }
    if ($l -match '^C_DEPS') { $mode = 'dep'; $out.Add($l); continue }
    if ($l.Trim() -eq '')    { $mode = '';    $out.Add($l); continue }

    if ($l -match '^\s*arm-none-eabi-gcc') {
        $nl = $l
        if ($flagArgs -ne '') { $nl = $nl -replace '-DDEBUG', ("-DDEBUG " + $flagArgs) }
        $nl = $nl -replace [regex]::Escape('-I"E:/STM32_Project/CubeIDE/f446_rtos/RTOS/Core/Inc"'),
                               ('-I"E:/STM32_Project/CubeIDE/f446_rtos/RTOS/Core/Inc" ' + $incExtra)
        $out.Add($nl)
        continue
    }

    $skip = $false
    foreach ($x in ($exSrc + $exObj + $exDep)) { if ($l.Trim() -eq "$x \") { $skip = $true } }
    if (-not $skip) { $out.Add($l) }
}

if ($cfg.extra.Count -gt 0) {
    $rule = ($out | Where-Object { $_ -match '^\s*arm-none-eabi-gcc' } | Select-Object -First 1)
    if ($Variant -in @('lowlevel','flash_diag','production_stress')) {
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
[System.IO.File]::WriteAllLines((Join-Path $DBG 'Core\Src\subdir.mk'), $out)

# objects.list is REGENERATED from scratch (never filtered incrementally):
# an incremental filter loses entries permanently the first time a variant
# excludes a file, which silently breaks later variants (observed: benchmark.o
# vanished after the scenario build and the bench variant then failed to link).
$baseOl = Join-Path $HIL 'templates/objects.list.template'
if (-not (Test-Path -LiteralPath $baseOl) -and (Test-Path -LiteralPath (Join-Path $LOCAL 'objects_list.template'))) {
    Copy-Item -LiteralPath (Join-Path $LOCAL 'objects_list.template') -Destination $baseOl
}
if (-not (Test-Path $baseOl)) {
    $ol0 = New-Object System.Collections.Generic.List[string]
    foreach ($l in (Get-Content (Join-Path $DBG 'objects.list'))) {
        if ($l -match 'hil/fwsrc') { continue }
        $ol0.Add($l.Trim())
    }
    Set-Content $baseOl -Value $ol0
}
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
$stamp = "$Variant|$Scenario|$($cfg.flags -join ',')"
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
