# hil\repro_all.ps1
# One-button regression, including deterministic interrupt/mask boundaries.
#
#   bench     : BM0..BM7 micro benchmarks            (docs\rtos_performance_report.md)
#   boundary  : 103 contract-boundary assertions      (docs\rtos_testing_report.md §2)
#   selftest  : 442 checks (431 assertions + 11 informational records)
#   scenario  : S1..S5 system-level comparison        (docs\rtos_performance_report.md §4)
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File hil\repro_all.ps1
#   powershell -ExecutionPolicy Bypass -File hil\repro_all.ps1 -Only bench,boundary
param(
    [string] $Only = 'audit,port,adversarial,bench,boundary,selftest,scenario',
    [string] $Tag  = 'repro'
)
$ErrorActionPreference = 'Stop'
$hil  = 'E:\STM32_Project\CubeIDE\f446_rtos\hil'
$ROOT = 'E:\STM32_Project\CubeIDE\f446_rtos'
$logs = Join-Path $hil 'logs'
if (-not (Test-Path $logs)) { New-Item -ItemType Directory -Path $logs | Out-Null }
$want = @($Only -split ',' | ForEach-Object { $_.Trim() })

function Invoke-Hil {
    param([string]$Variant, [int]$Scenario, [string]$Name, [int]$Seconds,
          [string]$Terminator, [string]$ReplyOn)
    Write-Host "########## $Name ##########"
    if ($Scenario -gt 0) {
        $b = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$hil\build_fw.ps1" -Variant $Variant -Scenario $Scenario 2>&1
    } else {
        $b = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$hil\build_fw.ps1" -Variant $Variant 2>&1
    }
    if ($LASTEXITCODE -ne 0) { $b | Select-Object -Last 10 | ForEach-Object { Write-Host $_ }; throw "BUILD FAILED ($Name)" }
    $pArgs = @('-NoProfile','-ExecutionPolicy','Bypass','-File',"$hil\run_hil.ps1",
              '-Elf',"$ROOT\Debug\f446_rtos.elf",'-Tag',$Name,'-Seconds',"$Seconds",'-Terminator',$Terminator)
    if ($ReplyOn -ne '') { $pArgs += @('-ReplyOn',$ReplyOn,'-Reply','X') }
    $null = & powershell.exe @pArgs 2>&1
    if ($LASTEXITCODE -ne 0) { throw "HIL failed or incomplete: $Name" }
    $log = Join-Path $logs "log_$Name.txt"
    if (-not (Test-Path $log)) { throw "no log for $Name" }
    return @(Get-Content $log)
}

if ($want -contains 'audit') {
    $lines = Invoke-Hil -Variant audit -Scenario 0 -Name "${Tag}_audit" -Seconds 30 -Terminator 'AT\|DONE' -ReplyOn ''
    $lines | Select-String '\|SUMMARY|\|FAIL' | ForEach-Object { Write-Host $_.Line.Trim() }
}

# Keep port fault-injection and blocking-return tests in the normal regression
# path. Otherwise the former blind spot would return when only this entry runs.
if ($want -contains 'port') {
    $portEvidence = Join-Path $logs ("${Tag}_port_" + (Get-Date).ToString('yyyyMMdd_HHmmss'))
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $hil 'basepri_20261025/run_matrix.ps1') -Suite all -OutDir $portEvidence
    if ($LASTEXITCODE -ne 0) { throw 'Port/ISR/blocking-return regression failed' }
}

# ------------------------------------------------------------------ bench ----
if ($want -contains 'adversarial') {
    $adversarialEvidence = Join-Path $logs ("${Tag}_adversarial_" + (Get-Date).ToString('yyyyMMdd_HHmmss'))
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $hil 'adversarial_20261025/run_matrix.ps1') -OutDir $adversarialEvidence
    if ($LASTEXITCODE -ne 0) { throw 'Complex mutex/wait/lifetime regression failed' }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $hil 'adversarial_20261025/run_matrix.ps1') -RegistryOnly `
        -ExpectedRegistryChecks 259 -OutDir (Join-Path $adversarialEvidence 'registry')
    if ($LASTEXITCODE -ne 0) { throw 'Maximum mutex graph/multi-lock deletion regression failed' }
}

if ($want -contains 'bench') {
    $lines = Invoke-Hil -Variant bench -Scenario 0 -Name "${Tag}_bench" -Seconds 45 -Terminator 'BM\|DONE' -ReplyOn ''
    $lines | Select-String '^BM\|' | ForEach-Object { Write-Host $_.Line.Trim() }
}

# --------------------------------------------------------------- boundary ----
if ($want -contains 'boundary') {
    $lines = Invoke-Hil -Variant boundary -Scenario 0 -Name "${Tag}_boundary" -Seconds 60 -Terminator 'BT\|SUMMARY' -ReplyOn ''
    $lines | Select-String '^BT\|PERF|^BT\|GEOM|^BT\|SUMMARY|FAIL' | ForEach-Object { Write-Host $_.Line.Trim() }
}

# --------------------------------------------------------------- selftest ----
if ($want -contains 'selftest') {
    $lines = Invoke-Hil -Variant selftest -Scenario 0 -Name "${Tag}_selftest" -Seconds 150 -Terminator 'ST\|SUMMARY' -ReplyOn 'ST\|ISR\|ARM'
    $lines | Select-String 'FAIL|^ST\|SUMMARY|^ST\|ASSERT' | ForEach-Object { Write-Host $_.Line.Trim() }
}

# --------------------------------------------------------------- scenario ----
if ($want -contains 'scenario') {
    foreach ($s in 1,2,3,4,5) {
        $lines = Invoke-Hil -Variant scenario_usb -Scenario $s -Name "${Tag}_S$s" -Seconds 150 -Terminator 'SC\|DONE' -ReplyOn ''
        $lines | Select-String '^SC\|' | ForEach-Object { Write-Host $_.Line.Trim() }
    }
}
Write-Host "`nall logs under $logs"
