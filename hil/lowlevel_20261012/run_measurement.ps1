param(
    [Parameter(Mandatory = $true)][ValidatePattern('^[a-z0-9_]+$')][string] $Stage,
    [int] $Repeat = 3,
    [ValidateSet('perf','bench')][string] $Variant = 'perf'
)
$ErrorActionPreference = 'Stop'
$projectRoot = 'E:\STM32_Project\CubeIDE\f446_rtos'
$hilRoot = Join-Path $projectRoot 'hil'
$outputRoot = Join-Path $hilRoot 'lowlevel_20261012'
$elfPath = Join-Path $projectRoot 'Debug\f446_rtos.elf'
$summaryPath = Join-Path $outputRoot ("measurement_$Stage.json")
if (Test-Path -LiteralPath $summaryPath) { throw "Do not overwrite completed stage: $Stage" }

$sources = @(Get-ChildItem -LiteralPath (Join-Path $projectRoot 'RTOS') -Recurse -File |
    Where-Object { $_.Extension -in '.c','.h' } | Sort-Object FullName | ForEach-Object {
        [ordered]@{ path=$_.FullName.Substring($projectRoot.Length + 1);
                    sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
    })
$sources | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $outputRoot "sources_$Stage.json") -Encoding utf8
# Snapshot copies retain their old timestamps. Force recompilation of all
# kernel and application objects so make cannot reuse a different candidate.
foreach ($relative in @('Debug/RTOS','Debug/Core/Src','Debug/hil')) {
    $objectRoot = [System.IO.Path]::GetFullPath((Join-Path $projectRoot $relative))
    if (-not $objectRoot.StartsWith($projectRoot + '\', [System.StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe object cleanup root' }
    if (Test-Path -LiteralPath $objectRoot) {
        Get-ChildItem -LiteralPath $objectRoot -Recurse -File | Where-Object { $_.Extension -in '.o','.d' } |
            ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force }
    }
}
$build = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $hilRoot 'build_fw.ps1') -Variant $Variant 2>&1
$buildExit = $LASTEXITCODE
$build | Out-File -LiteralPath (Join-Path $outputRoot "build_console_$Stage.txt") -Encoding utf8
Copy-Item -LiteralPath (Join-Path $hilRoot "logs/build_${Variant}_1.txt") -Destination (Join-Path $outputRoot "build_$Stage.txt") -Force
if ($buildExit -ne 0) { $build | Select-Object -Last 15 | Write-Host; throw "Build failed: $Stage" }
Write-Host "Built $Variant stage '$Stage'; collecting $Repeat independent reset runs."

$objdump = 'E:\ST\STM32CubeIDE_1.13.2\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.11.3.rel1.win32_1.1.100.202309141235\tools\bin\arm-none-eabi-objdump.exe'
foreach ($unit in @('RTOS/Core/Src/rtos_sched','RTOS/Core/Src/rtos_mutex','RTOS/Core/Src/rtos_sem','RTOS/Core/Src/rtos_task','RTOS/Port/GCC/ARM_CM7/port')) {
    $disassembly = & $objdump -dr (Join-Path $projectRoot "Debug/$unit.o") 2>&1
    if ($LASTEXITCODE -ne 0) { throw "objdump failed: $unit" }
    $disassembly | Out-File -LiteralPath (Join-Path $outputRoot ("${Stage}_" + [System.IO.Path]::GetFileName($unit) + '.disassembly.txt')) -Encoding utf8
}

$hash = (Get-FileHash -LiteralPath $elfPath -Algorithm SHA256).Hash
$runs = @()
foreach ($iteration in 1..$Repeat) {
    $tag = "lowlevel_20261012_${Stage}_r$iteration"
    $capture = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $hilRoot 'run_hil.ps1') -Elf $elfPath -Tag $tag -Seconds 90 -Terminator 'BM\|DONE' -OutDir $outputRoot 2>&1
    if ($LASTEXITCODE -ne 0) { $capture | Select-Object -Last 20 | Write-Host; throw "Run failed: $tag" }
    $raw = [System.IO.File]::ReadAllText((Join-Path $outputRoot "log_$tag.txt"))
    $checks = 0
    if ($Variant -eq 'perf' -and $raw -match 'PC\|SUMMARY\|pass=(\d+) fail=0') {
        $checks = [int]$Matches[1]
        if ($checks -eq 0) { throw "Empty mutex probe summary: $tag" }
    } elseif ($Variant -eq 'perf') { throw "Mutex probe summary missing/failed: $tag" }
    $metrics = @()
    foreach ($line in ($raw -split "`r?`n")) {
        if ($line -match '^BM\|(\d+)\|([^|]+)\|N=(\d+)\|avg=(\d+)\|min=(\d+)\|max=(\d+)') {
            $metrics += [ordered]@{ name=$Matches[2]; samples=[int]$Matches[3]; avg=[int]$Matches[4]; min=[int]$Matches[5]; max=[int]$Matches[6] }
        } elseif ($line -match '^PB\|([^|]+)\|N=(\d+)\|avg=(\d+)\|min=(\d+)\|max=(\d+)') {
            $metrics += [ordered]@{ name=$Matches[1]; samples=[int]$Matches[2]; avg=[int]$Matches[3]; min=[int]$Matches[4]; max=[int]$Matches[5] }
        }
    }
    if ($metrics.Count -lt 8 -or ($Variant -eq 'perf' -and (@($metrics | Where-Object { $_.name -match '^mutex_' }).Count -lt 3 -or $metrics.Count -lt 11))) { throw "Incomplete benchmark metrics: $tag" }
    if ((Get-FileHash -LiteralPath $elfPath -Algorithm SHA256).Hash -ne $hash) { throw 'ELF changed between repetitions' }
    $runs += [ordered]@{ tag=$tag; sha256=$hash; mutex_checks_passed=$checks; metrics=$metrics }
    $raw -split "`r?`n" | Where-Object { $_ -match '^BM\||^PB\||^PC\|SUMMARY' } | Write-Host
}
[ordered]@{ stage=$Stage; variant=$Variant; sha256=$hash; repeated=$Repeat; runs=$runs } |
    ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $summaryPath -Encoding utf8
Write-Host "Completed performance stage '$Stage'."
