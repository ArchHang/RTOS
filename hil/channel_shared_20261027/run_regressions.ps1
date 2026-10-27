param(
    [string] $Python = 'python',
    [string] $Port = 'COM3',
    [string] $Batch = ('regression_rerun_' + (Get-Date).ToString('yyyyMMdd_HHmmss'))
)
$ErrorActionPreference = 'Stop'
$runner = Join-Path $PSScriptRoot 'run_suite.py'
$cases = @(
    @{variant='audit'; end='AT\|DONE'; reply=''},
    @{variant='selftest'; end='ST\|SUMMARY\|.*\|END'; reply='ST\|ISR\|ARM'},
    @{variant='boundary'; end='BT\|SUMMARY\|'; reply=''},
    @{variant='lowlevel'; end='BM\|DONE'; reply=''},
    @{variant='bench'; end='BM\|DONE'; reply=''},
    @{variant='adversarial_mutex'; end='AM\|DONE\|status='; reply=''},
    @{variant='adversarial_wait'; end='AW\|DONE\|status='; reply=''},
    @{variant='adversarial_lifetime'; end='OF\|DONE\|status='; reply=''},
    @{variant='adversarial_mutex_boundary'; end='AMB\|DONE\|status='; reply=''},
    @{variant='adversarial_registry'; end='AR\|DONE\|status='; reply=''},
    @{variant='production_stress'; end='STRESS\|DONE\|status='; reply=''}
)
foreach ($case in $cases) {
    $arguments = @($runner, $case.variant, '--enable', '--batch', $Batch,
        '--port', $Port, '--terminator', $case.end)
    if ($case.reply -ne '') { $arguments += @('--reply-on', $case.reply) }
    & $Python @arguments
    if ($LASTEXITCODE -ne 0) { throw "Regression failed: $($case.variant)" }
}
foreach ($mask in @('basepri','primask')) {
    & $Python $runner basepri --enable --batch $Batch --port $Port --mask $mask `
        --terminator 'BP\|DONE\|status='
    if ($LASTEXITCODE -ne 0) { throw "Critical regression failed: $mask" }
}
Write-Host '13 enabled-feature integration captures complete.'
