# hil\run_scenarios.ps1
# Build + flash + capture every scenario in one go.
param(
    [string] $ScenarioList = "1,2,3,4,5",
    [int]   $Seconds   = 120,
    [string] $Tag      = 'sc'
)
$ErrorActionPreference = 'Stop'
$hil  = 'E:\STM32_Project\CubeIDE\f446_rtos\hil'
$ROOT = 'E:\STM32_Project\CubeIDE\f446_rtos'
$logs = Join-Path $hil 'logs'
if (-not (Test-Path $logs)) { New-Item -ItemType Directory -Path $logs | Out-Null }

$all = New-Object System.Collections.Generic.List[string]
$Scenarios = @($ScenarioList -split ',' | ForEach-Object { [int]$_.Trim() })
foreach ($s in $Scenarios) {
    Write-Host "########## scenario $s ##########"
    $b = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$hil\build_fw.ps1" -Variant scenario -Scenario $s 2>&1
    if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAILED s$s"; $b | Select-Object -Last 8 | ForEach-Object { Write-Host $_ }; continue }
    $o = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$hil\run_hil.ps1" `
            -Elf "$ROOT\Debug\f446_rtos.elf" -Tag "${Tag}_S$s" -Seconds $Seconds -Terminator 'SC\|DONE' 2>&1
    $log = Join-Path $logs "log_${Tag}_S$s.txt"
    if (Test-Path $log) {
        $lines = @(Get-Content $log | Select-String -Pattern '^SC\|' | ForEach-Object { $_.Line.Trim() })
        $lines | ForEach-Object { Write-Host $_; $all.Add($_) }
    } else {
        Write-Host "no log for s$s"
        $o | Select-Object -Last 10 | ForEach-Object { Write-Host $_ }
    }
}
$all | Set-Content (Join-Path $logs "results_$Tag.txt")
Write-Host "aggregated -> $(Join-Path $logs "results_$Tag.txt")"
