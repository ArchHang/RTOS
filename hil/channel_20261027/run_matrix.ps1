# New feature matrix. The Python runner records sources, ELF, raw UART and checks.
# Run with a new batch name; existing captures are never overwritten.
param(
    [string] $Python = 'python',
    [string] $Port = 'COM3',
    [string] $Batch = ('rerun_' + (Get-Date).ToString('yyyyMMdd_HHmmss'))
)
$ErrorActionPreference = 'Stop'
$runner = Join-Path $PSScriptRoot 'run_suite.py'
function Invoke-ChannelCapture {
    param([string] $Variant, [string] $Mask, [int] $Assertions, [string] $Optimization)
    & $Python $runner $Variant --mask $Mask --assertions $Assertions `
        --optimization $Optimization --port $Port --batch $Batch
    if ($LASTEXITCODE -ne 0) {
        throw "Capture failed: $Variant $Mask assertions=$Assertions $Optimization"
    }
}
foreach ($variant in @('channel_checks','channel_stress')) {
    foreach ($mask in @('basepri','primask')) {
        foreach ($assertions in @(1,0)) {
            foreach ($optimization in @('O0','O2','O3')) {
                Invoke-ChannelCapture $variant $mask $assertions $optimization
            }
        }
    }
}
foreach ($mask in @('basepri','primask')) {
    foreach ($assertions in @(1,0)) {
        Invoke-ChannelCapture 'channel_dma' $mask $assertions 'O2'
    }
    Invoke-ChannelCapture 'channel_profile' $mask 1 'O2'
}
Invoke-ChannelCapture 'channel_stress_negative' 'basepri' 1 'O2'
foreach ($optimization in @('O2','O3')) {
    Invoke-ChannelCapture 'channel_bench' 'basepri' 1 $optimization
}
Write-Host '33 captures complete. Restore and flash the daily image after any further regressions.'
