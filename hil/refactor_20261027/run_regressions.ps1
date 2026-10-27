param(
    [ValidateSet('functional','production','soak','all')][string] $Suite = 'all',
    [string] $Port = 'COM3',
    [string] $OutDir = '',
    [ValidateRange(30,900)][int] $SoakSeconds = 120
)
$ErrorActionPreference = 'Stop'
$evidenceRoot = if ($OutDir -eq '') {
    Join-Path $PSScriptRoot ('rerun_' + (Get-Date).ToString('yyyyMMdd_HHmmss'))
} else { [System.IO.Path]::GetFullPath($OutDir) }
New-Item -ItemType Directory -Path $evidenceRoot -Force | Out-Null
$projectRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $PSScriptRoot 'evidence_helpers.ps1')
if (Test-Path -LiteralPath (Join-Path $evidenceRoot "regressions_$Suite.json")) {
    throw 'Result index already exists; select a new output directory.'
}
$buildScript = Join-Path $projectRoot 'hil/build_fw.ps1'
$runScript = Join-Path $projectRoot 'hil/run_hil.ps1'
$firmware = Join-Path $projectRoot 'Debug/f446_rtos.elf'
$results = New-Object System.Collections.Generic.List[object]
$cases = New-Object System.Collections.Generic.List[object]

if ($Suite -in 'functional','all') {
    $cases.Add(@{tag='final_audit'; variant='audit'; timeout=90; end='AT\|DONE'; reply=''; flags=@()})
    $cases.Add(@{tag='final_selftest'; variant='selftest'; timeout=90; end='ST\|SUMMARY\|.*\|END'; reply='ST\|ISR\|ARM'; flags=@()})
    $cases.Add(@{tag='final_boundary'; variant='boundary'; timeout=90; end='BT\|SUMMARY\|'; reply=''; flags=@()})
    $cases.Add(@{tag='final_lowlevel'; variant='lowlevel'; timeout=90; end='BM\|DONE'; reply=''; flags=@()})
    $cases.Add(@{tag='final_nohook_bench'; variant='bench'; timeout=90; end='BM\|DONE'; reply=''; flags=@()})
}
if ($Suite -in 'production','all') {
    $cases.Add(@{tag='final_production_cap1'; variant='production_stress'; timeout=80; end='STRESS\|DONE\|'; reply=''; flags=@('-StressSeed','1','-StressSeconds','30','-StressQueueCapacity','1')})
    $cases.Add(@{tag='final_production_cap8'; variant='production_stress'; timeout=80; end='STRESS\|DONE\|'; reply=''; flags=@('-StressSeed','305419896','-StressSeconds','30','-StressQueueCapacity','8')})
    $cases.Add(@{tag='final_production_cap32'; variant='production_stress'; timeout=80; end='STRESS\|DONE\|'; reply=''; flags=@('-StressSeed','2779096485','-StressSeconds','30','-StressQueueCapacity','32')})
    $cases.Add(@{tag='final_production_negative'; variant='production_stress'; timeout=60; end='STRESS\|DONE\|'; reply=''; flags=@('-StressSeed','1','-StressSeconds','6','-StressQueueCapacity','8','-StressInjectCorruption'); negative=$true})
}
if ($Suite -in 'soak','all') {
    $cases.Add(@{tag="final_production_soak$SoakSeconds"; variant='production_stress'; timeout=($SoakSeconds + 50); end='STRESS\|DONE\|'; reply=''; flags=@('-StressSeed','3735928559','-StressSeconds',"$SoakSeconds",'-StressQueueCapacity','8')})
}

foreach ($case in $cases) {
    Write-Host "START $($case.tag)"
    if (Test-Path -LiteralPath (Join-Path $evidenceRoot "sources_$($case.tag).json")) {
        throw "Evidence already exists: $($case.tag)"
    }
    $sources = @(Get-RefactorSourceManifest -ProjectRoot $projectRoot)
    $sources | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $evidenceRoot "sources_$($case.tag).json") -Encoding utf8
    $arguments = @('-Variant',$case.variant) + $case.flags
    $output = & powershell -NoProfile -ExecutionPolicy Bypass -File $buildScript @arguments 2>&1
    $code = $LASTEXITCODE
    $output | Out-File -LiteralPath (Join-Path $evidenceRoot "build_$($case.tag).txt") -Encoding utf8
    Copy-Item -LiteralPath (Join-Path $projectRoot "hil/logs/build_$($case.variant)_1.txt") `
        -Destination (Join-Path $evidenceRoot "build_detail_$($case.tag).txt") -Force
    if ($code -ne 0) { throw "Build failed: $($case.tag)" }
    $captureArguments = @('-Elf',$firmware,'-Tag',$case.tag,'-Seconds',$case.timeout,
        '-Terminator',$case.end,'-Port',$Port,'-OutDir',$evidenceRoot)
    if ($case.reply -ne '') { $captureArguments += @('-ReplyOn',$case.reply,'-Reply','X') }
    if ($Suite -eq 'soak' -or $case.tag -like '*soak*') { $captureArguments += '-LiveLog' }
    if ($case.negative) { $captureArguments += '-AllowFailures' }
    $capture = & powershell -NoProfile -ExecutionPolicy Bypass -File $runScript @captureArguments 2>&1
    $captureCode = $LASTEXITCODE
    $capture | Out-File -LiteralPath (Join-Path $evidenceRoot "capture_$($case.tag).txt") -Encoding utf8
    if ($captureCode -ne 0) { throw "Hardware capture failed: $($case.tag) : $capture" }
    $content = [System.IO.File]::ReadAllText((Join-Path $evidenceRoot "log_$($case.tag).txt"))
    if ($case.negative) {
        if ($content -notmatch 'STRESS\|INJECT\|checksum_corruption' -or
            $content -notmatch 'STRESS\|FAIL\|packet_integrity checksum_errors=1' -or
            $content -notmatch 'STRESS\|SUMMARY\|pass=163 fail=3 worker_errors=1' -or
            $content -notmatch 'STRESS\|HIGHIRQ\|.*errors=0 running=0 restored=1 mismatch=0x0 ' -or
            $content -notmatch 'STRESS\|MASK\|safe_checks=[1-9]\d* leaks=0' -or
            $content -notmatch 'STRESS\|DONE\|status=-1') { throw 'Negative control failed to detect exactly the expected corruption' }
        $unexpectedFailures = @($content -split "`r?`n" | Where-Object {
            $_ -match '^STRESS\|FAIL\|' -and $_ -notmatch '^STRESS\|FAIL\|(packet_integrity checksum_errors=1|first=PACKET_CHECKSUM |check=every_successful_identity_validated_exactly_once$|check=all_runtime_contracts$)'
        })
        if ($unexpectedFailures.Count -ne 0) { throw "Unexpected negative-control failures: $unexpectedFailures" }
    } elseif ($case.variant -eq 'production_stress') {
        if ($content -notmatch 'STRESS\|SUMMARY\|pass=\d+ fail=0 worker_errors=0' -or
            $content -notmatch 'STRESS\|HIGHIRQ\|.*triple=[1-9]\d* .*errors=0 running=0 restored=1 mismatch=0x0 ' -or
            $content -notmatch 'STRESS\|MASK\|safe_checks=[1-9]\d* leaks=0' -or
            $content -notmatch 'STRESS\|DONE\|status=0') { throw "Production acceptance failed: $($case.tag)" }
    }
    $expected = switch ($case.variant) {
        'audit' { '(?m)^AT\|DONE\r?$' }
        'selftest' { 'ST\|SUMMARY\|pass=432 fail=0 info=10 total=442\|END' }
        'boundary' { 'BT\|SUMMARY\|pass=103 fail=0' }
        'lowlevel' { 'LC\|SUMMARY\|pass=24 fail=0' }
        default { '' }
    }
    if ($expected -ne '' -and $content -notmatch $expected) {
        throw "Missing expected complete functional summary: $($case.tag)"
    }
    if ($case.variant -eq 'audit') {
        foreach ($summary in @('SCHEDTASK\|SUMMARY\|pass=25 fail=0',
                                'IPC\|SUMMARY\|pass=15 fail=0',
                                'MEMTIMER\|SUMMARY\|pass=23\|fail=0',
                                'USBPORT\|SUMMARY\|pass=54\|fail=0')) {
            if ($content -notmatch "AT\|$summary") {
                throw "Incomplete audit summary: $summary"
            }
        }
    }
    if ($case.variant -eq 'lowlevel' -and $content -notmatch 'PC\|SUMMARY\|pass=22 fail=0') {
        throw 'Missing performance/PI probe results in lowlevel image.'
    }
    $after = @(Get-RefactorSourceManifest -ProjectRoot $projectRoot)
    if ((ConvertTo-Json -InputObject $sources -Compress) -cne
        (ConvertTo-Json -InputObject $after -Compress)) {
        throw "Sources or build rules changed during capture: $($case.tag)"
    }
    $summary = ($content -split "`r?`n" | Where-Object { $_ -match '\|SUMMARY\||^BM\|0\|' }) -join '; '
    $results.Add([ordered]@{tag=$case.tag; passed=$true; negative=[bool]$case.negative; summary=$summary})
    ConvertTo-Json -InputObject @($results.ToArray()) -Depth 5 |
        Set-Content -LiteralPath (Join-Path $evidenceRoot "regressions_$Suite.json") -Encoding utf8
    Write-Host "PASS $($case.tag) $summary"
}
Write-Host "Completed $($results.Count) regression captures; source main.c unchanged"
