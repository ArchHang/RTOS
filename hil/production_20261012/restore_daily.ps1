# Restore and verify the daily image after the production stress suites.
# This script performs a real SWD download and a 15-second COM3 capture when run.
# Do not run it while a hardware test is active. Existing evidence is never replaced.
$ErrorActionPreference = 'Stop'
$projectRoot = [System.IO.Path]::GetFullPath('E:\STM32_Project\CubeIDE\f446_rtos')
$workspaceRoot = [System.IO.Path]::GetFullPath('E:\STM32_Project\CubeIDE').TrimEnd('\') + '\'
$projectPrefix = $projectRoot.TrimEnd('\') + '\'
$outputRoot = Join-Path $projectRoot 'hil\production_20261012'
$elfPath = Join-Path $projectRoot 'Debug\f446_rtos.elf'
$tag = 'production_20261012_final_daily'
$validationPath = Join-Path $outputRoot 'daily_validation.json'
$baselinePath = Join-Path $outputRoot 'kernel_before.json'
$toolRoot = 'E:\ST\STM32CubeIDE_1.13.2\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.11.3.rel1.win32_1.1.100.202309141235\tools\bin'

function Assert-ProjectPath([string] $Path) {
    $absolutePath = [System.IO.Path]::GetFullPath($Path)
    if (-not $absolutePath.StartsWith($projectPrefix,
            [System.StringComparison]::OrdinalIgnoreCase) -or
        -not $absolutePath.StartsWith($workspaceRoot,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Unsafe project path: $absolutePath"
    }
    $ancestorPath = $absolutePath
    while ($ancestorPath.StartsWith($workspaceRoot,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        if (Test-Path -LiteralPath $ancestorPath) {
            $ancestor = Get-Item -LiteralPath $ancestorPath -Force
            if (($ancestor.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Unsafe project path reparse point: $ancestorPath"
            }
        }
        $ancestorPath = [System.IO.Path]::GetDirectoryName($ancestorPath)
    }
    return $absolutePath
}

function Get-KernelManifest {
    @(Get-ChildItem -LiteralPath (Join-Path $projectRoot 'RTOS') -Recurse -File |
        Where-Object { $_.Extension -in '.c','.h' } | Sort-Object FullName |
        ForEach-Object {
            $absolutePath = Assert-ProjectPath $_.FullName
            [ordered]@{
                path = $absolutePath.Substring($projectPrefix.Length)
                sha256 = (Get-FileHash -LiteralPath $absolutePath -Algorithm SHA256).Hash
            }
        })
}

function Assert-KernelUnchanged([object[]] $Expected) {
    $current = @(Get-KernelManifest)
    if (($Expected.Count -ne 61) -or ($current.Count -ne $Expected.Count)) {
        throw "Unexpected kernel source count: baseline=$($Expected.Count) current=$($current.Count)"
    }
    $expectedPaths = @{}
    foreach ($source in $Expected) {
        $absolutePath = Assert-ProjectPath (Join-Path $projectRoot $source.path)
        if ($expectedPaths.ContainsKey($source.path)) {
            throw "Duplicate kernel baseline path: $($source.path)"
        }
        $expectedPaths[$source.path] = $source.sha256
        if ((Get-FileHash -LiteralPath $absolutePath -Algorithm SHA256).Hash -ne $source.sha256) {
            throw "Kernel changed after the production baseline: $($source.path)"
        }
    }
    foreach ($source in $current) {
        if (-not $expectedPaths.ContainsKey($source.path)) {
            throw "Unexpected kernel source: $($source.path)"
        }
    }
    return $current
}

# Refuse all output collisions before changing build state or connecting hardware.
$reservedNames = @('daily_validation.json','daily_symbols.txt','daily_size.txt',
    'build_final_daily.txt','restore_daily_console.txt','daily_capture_console.txt',
    'sources_final_daily.json',"run_$tag.json","log_$tag.txt","flash_$tag.txt",
    "firmware_$tag.elf")
foreach ($name in $reservedNames) {
    $reservedPath = Assert-ProjectPath (Join-Path $outputRoot $name)
    if (Test-Path -LiteralPath $reservedPath) {
        throw "Do not overwrite final daily evidence: $reservedPath"
    }
}
if (-not (Test-Path -LiteralPath $baselinePath -PathType Leaf)) {
    throw "Missing production kernel baseline: $baselinePath"
}
[object[]]$expected = Get-Content -LiteralPath $baselinePath -Raw | ConvertFrom-Json
$null = Assert-KernelUnchanged $expected

# Macro changes are invisible to make. Remove only .o/.d files under checked roots.
# Reject reparse-point directories so an object traversal cannot leave the project.
foreach ($relative in @('Debug/RTOS','Debug/Core/Src','Debug/hil')) {
    $objectRoot = Assert-ProjectPath (Join-Path $projectRoot $relative)
    if (-not (Test-Path -LiteralPath $objectRoot)) {
        continue
    }
    $directories = @((Get-Item -LiteralPath $objectRoot))
    $directories += @(Get-ChildItem -LiteralPath $objectRoot -Recurse -Directory -Force)
    foreach ($directory in $directories) {
        $null = Assert-ProjectPath $directory.FullName
        if (($directory.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Unsafe object directory reparse point: $($directory.FullName)"
        }
    }
    $objects = @(Get-ChildItem -LiteralPath $objectRoot -Recurse -File |
        Where-Object { $_.Extension -in '.o','.d' })
    foreach ($object in $objects) {
        $absolutePath = Assert-ProjectPath $object.FullName
        if (-not $absolutePath.StartsWith($objectRoot.TrimEnd('\') + '\',
                [System.StringComparison]::OrdinalIgnoreCase) -or
            (($object.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)) {
            throw "Unsafe object deletion target: $absolutePath"
        }
        Remove-Item -LiteralPath $absolutePath -Force
    }
}

& powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    (Join-Path $projectRoot 'hil\restore_project.ps1') -Rebuild 2>&1 |
    Out-File -LiteralPath (Join-Path $outputRoot 'restore_daily_console.txt') -Encoding utf8
if ($LASTEXITCODE -ne 0) {
    throw 'Daily restore or build failed; see restore_daily_console.txt'
}
Copy-Item -LiteralPath (Join-Path $projectRoot 'hil\logs\build_restored_daily.txt') `
    -Destination (Join-Path $outputRoot 'build_final_daily.txt')

$rules = @(Get-ChildItem -LiteralPath (Join-Path $projectRoot 'Debug') -Recurse -File |
    Where-Object { $_.Name -in 'subdir.mk','objects.list','makefile' })
foreach ($rule in $rules) {
    $ruleText = [System.IO.File]::ReadAllText($rule.FullName)
    if ($ruleText -match ('FIRMWARE_(PERF|AUDIT|LOWLEVEL_CHECKS|FLASH_DIAG|PRODUCTION_STRESS|' +
        'BENCH|SELFTEST|BOUNDARY|SCENARIO)' +
        '|PRODUCTION_STRESS_|RTOS_HIL_AUDIT|--wrap=|hil/fwsrc|production_(stress|irq)')) {
        throw "Test build rule remains active: $($rule.FullName)"
    }
}
$symbols = & (Join-Path $toolRoot 'arm-none-eabi-nm.exe') -an $elfPath 2>&1
if ($LASTEXITCODE -ne 0) {
    throw 'Daily symbol read failed'
}
$symbols | Out-File -LiteralPath (Join-Path $outputRoot 'daily_symbols.txt') -Encoding utf8
$active = @($symbols | Where-Object {
    $_ -match (' (?:production_\w+|stress_\w+|bench_task|selftest_task|' +
        'boundary_test_task|boundary_task|scenario_task|scenario_\w+|test_scenario_\w+|' +
        'perf_probes_run|lowlevel_\w+|flash_diag_\w+|audit_\w+|__wrap_\w+)$')
})
if ($active.Count -ne 0) {
    throw 'A test fixture or task entry is linked into the daily firmware'
}
$size = & (Join-Path $toolRoot 'arm-none-eabi-size.exe') $elfPath 2>&1
if ($LASTEXITCODE -ne 0) {
    throw 'Daily size read failed'
}
$size | Out-File -LiteralPath (Join-Path $outputRoot 'daily_size.txt') -Encoding utf8
$finalSources = @(Assert-KernelUnchanged $expected)
$finalSources | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath (Join-Path $outputRoot 'sources_final_daily.json') -Encoding utf8
$firmwareHash = (Get-FileHash -LiteralPath $elfPath -Algorithm SHA256).Hash

# run_hil opens COM3 before SWD download, verifies Flash, resets and captures boot.
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    (Join-Path $projectRoot 'hil\run_hil.ps1') -Elf $elfPath -Tag $tag -Seconds 15 `
    -Port COM3 -Baud 115200 -OutDir $outputRoot 2>&1 |
    Out-File -LiteralPath (Join-Path $outputRoot 'daily_capture_console.txt') -Encoding utf8
if ($LASTEXITCODE -ne 0) {
    throw 'Daily flash or capture failed; see daily_capture_console.txt'
}
$meta = Get-Content -LiteralPath (Join-Path $outputRoot "run_$tag.json") -Raw | ConvertFrom-Json
$raw = Get-Content -LiteralPath (Join-Path $outputRoot "log_$tag.txt") -Raw
$flashLog = Get-Content -LiteralPath (Join-Path $outputRoot "flash_$tag.txt") -Raw
$savedHash = (Get-FileHash -LiteralPath (Join-Path $outputRoot "firmware_$tag.elf") `
    -Algorithm SHA256).Hash
$bootIndex = $raw.LastIndexOf('System init done', [System.StringComparison]::Ordinal)
if ($bootIndex -lt 0) {
    throw 'Daily capture does not contain the post-reset initialization marker'
}
# Keep the complete raw log, but exclude pre-download serial output from acceptance.
$bootText = $raw.Substring($bootIndex)
$task0Count = [regex]::Matches($bootText, '(?i)task[ _]0 running').Count
$task1Count = [regex]::Matches($bootText, '(?i)task[ _]1 running').Count
if (($meta.failure_lines.Count -ne 0) -or (-not $meta.flashed) -or
    ($meta.port -ne 'COM3') -or ($meta.baud -ne 115200) -or
    ($meta.sha256 -ne $firmwareHash) -or ($savedHash -ne $firmwareHash) -or
    ($flashLog -notmatch 'File download complete') -or
    ($flashLog -notmatch 'Download verified successfully') -or
    ($raw -notmatch 'System init done') -or ($task0Count -lt 2) -or ($task1Count -lt 2) -or
    ($bootText -match '(?m)^(?:STRESS|BM|AU|ST|BT)\|')) {
    throw 'Daily provenance, initialization or continuing periodic output verification failed'
}
$null = Assert-KernelUnchanged $expected
[ordered]@{
    tag = $tag
    sha256 = $firmwareHash
    source_count = $finalSources.Count
    matches_production_kernel_baseline = $true
    active_test_entries = 0
    active_test_rules = 0
    serial_port = $meta.port
    serial_baud = $meta.baud
    serial_capture_seconds = 15
    flash_download_verified = $true
    initialized = $true
    acceptance_start = 'last System init done marker in the complete raw capture'
    task0_output_count = $task0Count
    task1_output_count = $task1Count
    failure_lines = 0
    size_lines = $size
    note = 'Existing inactive USART selftest and TIM6 benchmark hooks remain in the application; no test tasks were started. USB initialization output does not prove physical USB enumeration or transfer.'
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $validationPath -Encoding utf8
$size | Write-Host
Write-Host "Daily restored and verified on board: $firmwareHash"
