param(
    [ValidateSet('smoke','matrix','soak','negative')][string] $Suite = 'smoke',
    [ValidatePattern('^[a-z0-9_]+$')][string] $Label = 'initial'
)
$ErrorActionPreference = 'Stop'
$projectRoot = 'E:\STM32_Project\CubeIDE\f446_rtos'
$outputRoot = Join-Path $projectRoot 'hil\production_20261012'
$elfPath = Join-Path $projectRoot 'Debug\f446_rtos.elf'
$toolRoot = 'E:\ST\STM32CubeIDE_1.13.2\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.11.3.rel1.win32_1.1.100.202309141235\tools\bin'
$summaryPath = Join-Path $outputRoot "results_${Suite}_${Label}.json"
if (Test-Path -LiteralPath $summaryPath) { throw 'Do not overwrite existing evidence' }
$cases = @(switch ($Suite) {
    'smoke' { @([ordered]@{seed=[uint32]1; seconds=30; capacity=8}) }
    'matrix' {
        @([ordered]@{seed=[uint32]1; seconds=60; capacity=1},
          [ordered]@{seed=[uint32]305419896; seconds=60; capacity=8},
          [ordered]@{seed=[uint32]2779096485; seconds=60; capacity=32})
    }
    'soak' { @([ordered]@{seed=[uint32]3735928559; seconds=900; capacity=8}) }
    'negative' { @([ordered]@{seed=[uint32]1; seconds=6; capacity=8}) }
})
$results = @()
for ($caseIndex = 0; $caseIndex -lt $cases.Count; $caseIndex++) {
    $case = $cases[$caseIndex]
    $tag = "production_20261012_${Suite}_${Label}_$($caseIndex + 1)"
    if (Test-Path -LiteralPath (Join-Path $outputRoot "run_$tag.json")) {
        throw "Capture already exists: $tag"
    }
    $sources = Get-ChildItem -LiteralPath (Join-Path $projectRoot 'RTOS') -Recurse -File |
        Where-Object { $_.Extension -in '.c','.h' } | ForEach-Object {
            [ordered]@{ path=$_.FullName.Substring($projectRoot.Length + 1);
                sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
        }
    $fixtures = @('hil/fw/Inc/production_stress.h','hil/fw/Src/production_stress.c',
        'hil/fw/Inc/production_irq.h','hil/fw/Src/production_irq.c','Core/Src/main.c',
        'hil/build_fw.ps1') | ForEach-Object {
            [ordered]@{path=$_;
                sha256=(Get-FileHash -LiteralPath (Join-Path $projectRoot $_)).Hash}
        }
    [ordered]@{kernel=$sources; fixtures=$fixtures; configuration=$case} |
        ConvertTo-Json -Depth 6 | Set-Content -LiteralPath `
            (Join-Path $outputRoot "sources_$tag.json") -Encoding utf8
    foreach ($relative in @('Debug/RTOS','Debug/Core/Src','Debug/hil')) {
        $objectRoot = [System.IO.Path]::GetFullPath((Join-Path $projectRoot $relative))
        if (-not $objectRoot.StartsWith($projectRoot + '\',
                [System.StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe cleanup root' }
        if (Test-Path -LiteralPath $objectRoot) {
            Get-ChildItem -LiteralPath $objectRoot -Recurse -File |
                Where-Object { $_.Extension -in '.o','.d' } |
                ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force }
        }
    }
    $buildArguments = @('-NoProfile','-ExecutionPolicy','Bypass','-File',
        (Join-Path $projectRoot 'hil\build_fw.ps1'),'-Variant','production_stress',
        '-StressSeed',$case.seed,'-StressSeconds',$case.seconds,
        '-StressQueueCapacity',$case.capacity)
    if ($Suite -eq 'negative') { $buildArguments += '-StressInjectCorruption' }
    & powershell.exe @buildArguments 2>&1 |
        Out-File -LiteralPath (Join-Path $outputRoot "build_console_$tag.txt") -Encoding utf8
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $tag" }
    Copy-Item -LiteralPath (Join-Path $projectRoot 'hil\logs\build_production_stress_1.txt') `
        -Destination (Join-Path $outputRoot "build_$tag.txt")
    $hash = (Get-FileHash -LiteralPath $elfPath -Algorithm SHA256).Hash
    $disassembly = & (Join-Path $toolRoot 'arm-none-eabi-objdump.exe') -d $elfPath 2>&1
    if ($LASTEXITCODE -ne 0) { throw 'Disassembly failed' }
    $disassembly | Out-File -LiteralPath (Join-Path $outputRoot "disassembly_$tag.txt")
    & (Join-Path $toolRoot 'arm-none-eabi-size.exe') $elfPath 2>&1 |
        Out-File -LiteralPath (Join-Path $outputRoot "size_$tag.txt")
    if ($LASTEXITCODE -ne 0) { throw 'Size check failed' }
    $captureArguments = @('-NoProfile','-ExecutionPolicy','Bypass','-File',
        (Join-Path $projectRoot 'hil\run_hil.ps1'),'-Elf',$elfPath,'-Tag',$tag,
        '-Seconds',($case.seconds + 50),'-Terminator','STRESS\|DONE\|','-OutDir',$outputRoot)
    if ($Suite -eq 'negative') { $captureArguments += '-AllowFailures' }
    & powershell.exe @captureArguments
    if ($LASTEXITCODE -ne 0) { throw "Hardware run failed: $tag" }
    $raw = Get-Content -LiteralPath (Join-Path $outputRoot "log_$tag.txt") -Raw
    $meta = Get-Content -LiteralPath (Join-Path $outputRoot "run_$tag.json") -Raw |
        ConvertFrom-Json
    if ((-not $meta.flashed) -or
        (-not $meta.terminator_seen) -or ($meta.sha256 -ne $hash) -or
        ($meta.port -ne 'COM3')) { throw "Capture provenance failed: $tag" }
    if ($Suite -eq 'negative') {
        if (($raw -notmatch 'STRESS\|FAIL\|') -or
            ($raw -notmatch 'STRESS\|INJECT\|checksum_corruption\|source=0\|sequence=3') -or
            ($raw -notmatch 'STRESS\|FAIL\|packet_integrity checksum_errors=1(?:\r?\n|$)') -or
            ($raw -notmatch 'STRESS\|SUMMARY\|pass=\d+ fail=[1-9]\d*') -or
            ($raw -match 'STRESS\|DONE\|status=0(?:\r?\n|$)') -or
            ($meta.failure_lines.Count -eq 0)) { throw "Negative control missed fault: $tag" }
    } elseif (($raw -notmatch 'STRESS\|DONE\|status=0(?:\r?\n|$)') -or
        ($raw -notmatch 'STRESS\|SUMMARY\|pass=\d+ fail=0') -or
        ($raw -match 'STRESS\|FAIL\|') -or ($meta.failure_lines.Count -ne 0)) {
        throw "Acceptance failed: $tag"
    }
    foreach ($source in $sources) {
        if ((Get-FileHash -LiteralPath (Join-Path $projectRoot $source.path)).Hash -ne
            $source.sha256) { throw "Kernel source changed during run: $($source.path)" }
    }
    foreach ($fixture in $fixtures) {
        if ((Get-FileHash -LiteralPath (Join-Path $projectRoot $fixture.path)).Hash -ne
            $fixture.sha256) { throw "Fixture source changed during run: $($fixture.path)" }
    }
    if ((Get-FileHash -LiteralPath $elfPath).Hash -ne $hash) {
        throw 'ELF changed during capture'
    }
    $results += [ordered]@{tag=$tag; sha256=$hash; seed=$case.seed;
        seconds=$case.seconds; capacity=$case.capacity;
        result=$(if ($Suite -eq 'negative') { 'expected_corruption_detected' } else { 'passed' })}
    $results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath `
        (Join-Path $outputRoot "checkpoint_${Suite}_${Label}.json") -Encoding utf8
}
$results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $summaryPath -Encoding utf8
Write-Host "Production stress suite completed: $Suite ($($results.Count) captures)"
