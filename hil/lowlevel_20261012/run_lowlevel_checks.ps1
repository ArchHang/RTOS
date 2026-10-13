param(
    [ValidateSet('lowlevel','stats','flash')][string[]] $Only = @('lowlevel','stats','flash'),
    [ValidatePattern('^[a-z0-9_]+$')][string] $Label = 'final',
    [ValidateRange(1,5)][int] $Repeat = 3
)
$ErrorActionPreference = 'Stop'
$projectRoot = 'E:\STM32_Project\CubeIDE\f446_rtos'
$hilRoot = Join-Path $projectRoot 'hil'
$outputRoot = Join-Path $hilRoot 'lowlevel_20261012'
$elfPath = Join-Path $projectRoot 'Debug\f446_rtos.elf'
$configPath = Join-Path $projectRoot 'RTOS\Core\Inc\rtos_config.h'
$savedConfig = [System.IO.File]::ReadAllBytes($configPath)
$summaryPath = Join-Path $outputRoot "checks_${Label}_results.json"
if (Test-Path -LiteralPath $summaryPath) { throw "Do not overwrite checks: $Label" }
foreach ($mode in $Only) {
    foreach ($iteration in 1..$Repeat) {
        $existingTag = "lowlevel_20261012_checks_${Label}_${mode}_r$iteration"
        if (Test-Path -LiteralPath (Join-Path $outputRoot "run_$existingTag.json")) {
            throw "Do not overwrite an existing capture: $existingTag"
        }
    }
}
$results = @()
$toolRoot = 'E:\ST\STM32CubeIDE_1.13.2\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.11.3.rel1.win32_1.1.100.202309141235\tools\bin'

function Save-Sources([string] $Name) {
    $entries = @(Get-ChildItem -LiteralPath (Join-Path $projectRoot 'RTOS') -Recurse -File |
        Where-Object { $_.Extension -in '.c','.h' } | Sort-Object FullName | ForEach-Object {
            [ordered]@{ path=$_.FullName.Substring($projectRoot.Length + 1);
                        sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
        })
    $entries | ConvertTo-Json -Depth 4 |
        Set-Content -LiteralPath (Join-Path $outputRoot "sources_checks_${Name}.json") -Encoding utf8
}

function Build-Checks([string] $Variant, [string] $Name) {
    foreach ($relative in @('Debug/RTOS','Debug/Core/Src','Debug/hil')) {
        $objectRoot = [System.IO.Path]::GetFullPath((Join-Path $projectRoot $relative))
        if (-not $objectRoot.StartsWith($projectRoot + '\',
                [System.StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe object cleanup' }
        if (Test-Path -LiteralPath $objectRoot) {
            Get-ChildItem -LiteralPath $objectRoot -Recurse -File |
                Where-Object { $_.Extension -in '.o','.d' } |
                ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force }
        }
    }
    Save-Sources $Name
    $build = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $hilRoot 'build_fw.ps1') -Variant $Variant 2>&1
    $buildCode = $LASTEXITCODE
    $build | Out-File -LiteralPath (Join-Path $outputRoot "build_console_checks_${Name}.txt") `
        -Encoding utf8
    Copy-Item -LiteralPath (Join-Path $hilRoot "logs/build_${Variant}_1.txt") `
        -Destination (Join-Path $outputRoot "build_checks_${Name}.txt")
    if ($buildCode -ne 0) {
        $build | Select-Object -Last 20 | Write-Host
        throw "Build failed: $Name"
    }
    foreach ($relative in @('RTOS/Core/Src/rtos_sched','RTOS/Port/GCC/ARM_CM7/port',
                             'hil/fwsrc/lowlevel_fpu_probe')) {
        $object = Join-Path $projectRoot "Debug/$relative.o"
        if (-not (Test-Path -LiteralPath $object)) { continue }
        $asm = & (Join-Path $toolRoot 'arm-none-eabi-objdump.exe') -dr $object 2>&1
        if ($LASTEXITCODE -ne 0) { throw "Disassembly failed: $object" }
        $asm | Out-File -LiteralPath (Join-Path $outputRoot `
            ("checks_${Name}_" + [System.IO.Path]::GetFileName($relative) + '.disassembly.txt')) `
            -Encoding utf8
    }
    Write-Host "Built checks '$Name'; collecting $Repeat independent reset runs."
}

try {
    foreach ($mode in $Only) {
        [System.IO.File]::WriteAllBytes($configPath, $savedConfig)
        if ($mode -eq 'stats') {
            $config = [System.IO.File]::ReadAllText($configPath)
            $pattern = '(?m)^#define RTOS_CONFIG_PERF_HOTPATH_STATS \(0\)\s*$'
            if ([regex]::Matches($config, $pattern).Count -ne 1) {
                throw 'Unexpected hotpath stats configuration'
            }
            $config = [regex]::Replace($config, $pattern,
                '#define RTOS_CONFIG_PERF_HOTPATH_STATS (1)')
            [System.IO.File]::WriteAllText($configPath, $config,
                (New-Object System.Text.UTF8Encoding($false)))
        }
        $name = "${Label}_$mode"
        $variant = if ($mode -eq 'flash') { 'flash_diag' } else { 'lowlevel' }
        Build-Checks $variant $name
        $hash = (Get-FileHash -LiteralPath $elfPath -Algorithm SHA256).Hash
        foreach ($iteration in 1..$Repeat) {
            $tag = "lowlevel_20261012_checks_${name}_r$iteration"
            $capture = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
                (Join-Path $hilRoot 'run_hil.ps1') -Elf $elfPath -Tag $tag -Seconds 90 `
                -Terminator 'BM\|DONE' -OutDir $outputRoot 2>&1
            if ($LASTEXITCODE -ne 0) {
                $capture | Select-Object -Last 30 | Write-Host
                throw "Run failed: $tag"
            }
            $raw = [System.IO.File]::ReadAllText((Join-Path $outputRoot "log_$tag.txt"))
            if ($mode -eq 'flash') {
                if ($raw -notmatch 'FD\|SUMMARY\|pass=7 fail=0\|status=0') {
                    throw "Flash cache diagnostic failed: $tag"
                }
                if (@([regex]::Matches($raw, 'FD\|MODE\|')).Count -ne 3) {
                    throw "Flash cache modes missing: $tag"
                }
                $modePattern = 'FD\|MODE\|(?<mode>ICACHE_ON_FIRST|ICACHE_OFF|ICACHE_ON_AGAIN)[^\r\n]*\r?\n' +
                    'BM\|2\|sem_roundtrip\|N=1\|avg=(?<cycles>\d+)[^\r\n]*\r?\nFD\|STATE\|'
                if ([regex]::Matches($raw, $modePattern).Count -ne 3) {
                    throw "A cache mode did not complete its measured callback: $tag"
                }
            } else {
                if ($raw -notmatch 'LC\|SUMMARY\|pass=24 fail=0') {
                    throw "Lowlevel checks failed: $tag"
                }
                if ($raw -notmatch 'PC\|SUMMARY\|pass=22 fail=0') {
                    throw "Mutex checks failed: $tag"
                }
                if ($raw -notmatch 'LC\|DETAIL\|rounds_a=10000 rounds_b=10000 integer=10000 errors_a=0 errors_b=0') {
                    throw "Actual register checks incomplete: $tag"
                }
            }
            if ((Get-FileHash -LiteralPath $elfPath -Algorithm SHA256).Hash -ne $hash) {
                throw 'Firmware changed between reset runs'
            }
            $meta = Get-Content -LiteralPath (Join-Path $outputRoot "run_$tag.json") -Raw |
                ConvertFrom-Json
            if (($meta.sha256 -ne $hash) -or (-not $meta.flashed) -or
                (-not $meta.terminator_seen) -or ($meta.failure_lines.Count -ne 0)) {
                throw "Capture provenance failed: $tag"
            }
            $results += [ordered]@{ tag=$tag; mode=$mode; sha256=$hash; failed=0;
                context_checks=$(if ($mode -eq 'flash') { 0 } else { 24 });
                mutex_checks=$(if ($mode -eq 'flash') { 0 } else { 22 });
                fpu_rounds_per_task=$(if ($mode -eq 'flash') { 0 } else { 10000 });
                hotpath_stats=($mode -eq 'stats'); flash_mode_checks=($mode -eq 'flash') }
            $results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath `
                (Join-Path $outputRoot "checks_${Label}_checkpoint.json") -Encoding utf8
            $raw -split "`r?`n" | Where-Object {
                $_ -match '^LC\||^FD\||^PC\|SUMMARY|^BM\|2\|'
            } | Write-Host
        }
    }
} finally {
    [System.IO.File]::WriteAllBytes($configPath, $savedConfig)
}
$results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $summaryPath -Encoding utf8
Write-Host "All selected lowlevel checks completed; original config restored."
