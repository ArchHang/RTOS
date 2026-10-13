$ErrorActionPreference = 'Stop'
$projectRoot = 'E:\STM32_Project\CubeIDE\f446_rtos'
$outputRoot = Join-Path $projectRoot 'hil\lowlevel_20261012'
$elfPath = Join-Path $projectRoot 'Debug\f446_rtos.elf'
$tag = 'lowlevel_20261012_final_daily'
$validationPath = Join-Path $outputRoot 'daily_validation.json'
if (Test-Path -LiteralPath $validationPath) { throw 'Do not overwrite final daily evidence' }
$expected = Get-Content -LiteralPath (Join-Path $outputRoot 'sources_isr_cached.json') -Raw |
    ConvertFrom-Json
foreach ($source in $expected) {
    $path = Join-Path $projectRoot $source.path
    if ((Get-FileHash -LiteralPath $path).Hash -ne $source.sha256) {
        throw "Production source changed after verified candidate: $($source.path)"
    }
}
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
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    (Join-Path $projectRoot 'hil\restore_project.ps1') -Rebuild
if ($LASTEXITCODE -ne 0) { throw 'Daily restore or build failed' }
Copy-Item -LiteralPath (Join-Path $projectRoot 'hil\logs\build_restored_daily.txt') `
    -Destination (Join-Path $outputRoot 'build_final_daily.txt')
$rules = Get-ChildItem -LiteralPath (Join-Path $projectRoot 'Debug') -Recurse -File |
    Where-Object { $_.Name -in 'subdir.mk','objects.list','makefile' }
foreach ($rule in $rules) {
    $text = [System.IO.File]::ReadAllText($rule.FullName)
    if ($text -match 'FIRMWARE_(PERF|AUDIT|LOWLEVEL_CHECKS|FLASH_DIAG)|RTOS_HIL_AUDIT|--wrap=|hil/fwsrc') {
        throw "Test build rule remains active: $($rule.FullName)"
    }
}
$toolRoot = 'E:\ST\STM32CubeIDE_1.13.2\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.11.3.rel1.win32_1.1.100.202309141235\tools\bin'
$symbols = & (Join-Path $toolRoot 'arm-none-eabi-nm.exe') -an $elfPath 2>&1
if ($LASTEXITCODE -ne 0) { throw 'Daily symbol read failed' }
$symbols | Out-File -LiteralPath (Join-Path $outputRoot 'daily_symbols.txt') -Encoding utf8
$active = @($symbols | Where-Object {
    $_ -match ' (?:bench_task|selftest_task|perf_probes_run|lowlevel_\w+|flash_diag_\w+|audit_\w+|__wrap_\w+)$'
})
if ($active.Count -ne 0) { throw 'A test entry is linked into daily firmware' }
$size = & (Join-Path $toolRoot 'arm-none-eabi-size.exe') $elfPath 2>&1
if ($LASTEXITCODE -ne 0) { throw 'Daily size read failed' }
$size | Out-File -LiteralPath (Join-Path $outputRoot 'daily_size.txt') -Encoding utf8
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    (Join-Path $projectRoot 'hil\run_hil.ps1') -Elf $elfPath -Tag $tag -Seconds 15 -OutDir $outputRoot |
    Out-File -LiteralPath (Join-Path $outputRoot 'daily_capture_console.txt') -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw 'Daily flash or capture failed' }
$meta = Get-Content -LiteralPath (Join-Path $outputRoot "run_$tag.json") -Raw | ConvertFrom-Json
$raw = Get-Content -LiteralPath (Join-Path $outputRoot "log_$tag.txt") -Raw
if (($meta.failure_lines.Count -ne 0) -or (-not $meta.flashed) -or
    ($raw -notmatch 'System init done') -or ($raw -notmatch 'task_uart|Task 0|task_0|Task 1|task_1')) {
    throw 'Daily did not initialize and produce periodic task output'
}
$expected | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath (Join-Path $outputRoot 'sources_final_daily.json') -Encoding utf8
[ordered]@{ tag=$tag; sha256=$meta.sha256; source_count=$expected.Count;
    matches_verified_candidate=$true; active_test_entries=0; active_test_rules=0;
    serial_capture_seconds=15; initialized=$true; failure_lines=0; size_lines=$size;
    note='Existing inactive USART selftest and TIM6 benchmark hooks are retained by the application; no test tasks were started.' } |
    ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $validationPath -Encoding utf8
$size | Write-Host
Write-Host "Daily restored and verified on board: $($meta.sha256)"
