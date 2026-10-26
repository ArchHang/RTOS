param(
    [ValidateSet('functional','production','soak','all')][string] $Suite = 'all',
    [string] $Port = 'COM3',
    [string] $OutDir = ''
)
$ErrorActionPreference = 'Stop'
$evidenceRoot = if ($OutDir -eq '') {
    Join-Path $PSScriptRoot ('rerun_' + (Get-Date).ToString('yyyyMMdd_HHmmss'))
} else { [System.IO.Path]::GetFullPath($OutDir) }
New-Item -ItemType Directory -Path $evidenceRoot -Force | Out-Null
$projectRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
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
    $cases.Add(@{tag='final_production_soak900'; variant='production_stress'; timeout=950; end='STRESS\|DONE\|'; reply=''; flags=@('-StressSeed','3735928559','-StressSeconds','900','-StressQueueCapacity','8')})
}

foreach ($case in $cases) {
    Write-Host "START $($case.tag)"
    $sourceFiles = @(Get-ChildItem -LiteralPath (Join-Path $projectRoot 'RTOS') -Recurse -File) +
        @(Get-ChildItem -LiteralPath (Join-Path $projectRoot 'hil/fw') -Recurse -File) +
        @(Get-Item -LiteralPath (Join-Path $projectRoot 'Core/Src/main.c'))
    $sources = @($sourceFiles | Where-Object { $_.Extension -in '.c','.h' } | ForEach-Object {
        [ordered]@{path=$_.FullName.Substring($projectRoot.Length+1); sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}
    })
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
    if ($Suite -eq 'soak' -or $case.tag -like '*soak900') { $captureArguments += '-LiveLog' }
    if ($case.negative) { $captureArguments += '-AllowFailures' }
    $capture = & powershell -NoProfile -ExecutionPolicy Bypass -File $runScript @captureArguments 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Hardware capture failed: $($case.tag) : $capture" }
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
    foreach ($source in $sources) {
        if ((Get-FileHash -LiteralPath (Join-Path $projectRoot $source.path)).Hash -ne $source.sha256) {
            throw "Source changed during hardware capture: $($source.path)"
        }
    }
    $summary = ($content -split "`r?`n" | Where-Object { $_ -match '\|SUMMARY\||^BM\|0\|' }) -join '; '
    $results.Add([ordered]@{tag=$case.tag; passed=$true; negative=[bool]$case.negative; summary=$summary})
    $results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $evidenceRoot "regressions_$Suite.json") -Encoding utf8
    Write-Host "PASS $($case.tag) $summary"
}
Write-Host "Completed $($results.Count) regression captures; source main.c unchanged"
