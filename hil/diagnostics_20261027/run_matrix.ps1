param(
    [string] $Python = 'python',
    [string] $Port = 'COM3',
    [string] $Batch = ('rerun_' + (Get-Date).ToString('yyyyMMdd_HHmmss')),
    [ValidateSet('functional','bench','regression','fault')][string[]] $Only = @(
        'functional','bench','regression','fault')
)
$ErrorActionPreference = 'Stop'
$runner = Join-Path $PSScriptRoot 'run_suite.py'

function Invoke-DiagnosticCapture([string[]] $Arguments) {
    & $Python $runner @Arguments --batch $Batch --port $Port
    if ($LASTEXITCODE -ne 0) { throw "Capture failed: $($Arguments -join ' ')" }
}

if ($Only -contains 'functional') {
    foreach ($mask in @('basepri','primask')) {
        foreach ($assertions in @(0,1)) {
            foreach ($optimization in @('O0','O2','O3')) {
                Invoke-DiagnosticCapture @('diagnostic_checks','--channels','--mask',
                    $mask,'--assertions',"$assertions",'--optimization',$optimization)
            }
        }
        Invoke-DiagnosticCapture @('diagnostic_ring','--capacity','8','--mask',$mask)
    }
    Invoke-DiagnosticCapture @('diagnostic_checks','--diag','off')
}
if ($Only -contains 'bench') {
    foreach ($optimization in @('O2','O3')) {
        foreach ($enabled in @('off','on')) {
            Invoke-DiagnosticCapture @('diagnostic_bench','--diag',$enabled,
                '--optimization',$optimization)
        }
    }
}
if ($Only -contains 'regression') {
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
        $capture = @($case.variant,'--channels','--terminator',$case.end)
        if ($case.reply -ne '') { $capture += @('--reply-on',$case.reply) }
        Invoke-DiagnosticCapture $capture
    }
    foreach ($mask in @('basepri','primask')) {
        Invoke-DiagnosticCapture @('basepri','--channels','--mask',$mask,
            '--terminator','BP\|DONE\|status=')
    }
}
if ($Only -contains 'fault') {
    Invoke-DiagnosticCapture @('diagnostic_fault','--fault')
    Write-Host 'Target intentionally halted; attach for SRAM evidence, then restore daily firmware.'
}
