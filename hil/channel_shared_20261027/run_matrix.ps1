# CPU/shared-memory only. Historical DMA fixtures are never selected here.
param([string] $Python = 'python', [string] $Batch = 'optimized', [string] $Port = 'COM3')
$ErrorActionPreference = 'Stop'
$runner = Join-Path $PSScriptRoot 'run_suite.py'
foreach ($variant in @('channel_checks','channel_stress')) {
    foreach ($mask in @('basepri','primask')) {
        foreach ($assertions in @(1,0)) {
            foreach ($optimization in @('O0','O2','O3')) {
                & $Python $runner $variant --mask $mask --assertions $assertions `
                    --optimization $optimization --port $Port --batch $Batch
                if ($LASTEXITCODE -ne 0) { throw "Failed: $variant $mask $assertions $optimization" }
            }
        }
    }
}
foreach ($mask in @('basepri','primask')) {
    & $Python $runner channel_profile --mask $mask --port $Port --batch $Batch
    if ($LASTEXITCODE -ne 0) { throw "Profile failed: $mask" }
}
& $Python $runner channel_stress_negative --port $Port --batch $Batch
if ($LASTEXITCODE -ne 0) { throw 'Negative control failed' }
foreach ($optimization in @('O2','O3')) {
    & $Python $runner channel_bench --optimization $optimization --port $Port --batch $Batch
    if ($LASTEXITCODE -ne 0) { throw "Benchmark failed: $optimization" }
}
Write-Host '29 shared-memory captures complete; restore daily firmware after regressions.'
