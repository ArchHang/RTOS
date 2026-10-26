# Run only after all batch captures finish. This restores and flashes the daily
# image; the independent evidence verifier must be run separately afterward.
param([string] $Port = 'COM3', [switch] $KeepCompileArtifacts)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Split-Path (Split-Path $PSScriptRoot -Parent) -Parent))
if ($projectRoot -ne 'E:\STM32_Project\CubeIDE\f446_rtos') { throw 'Unexpected project root.' }
$batchRoot = [IO.Path]::GetFullPath($PSScriptRoot)
$evidenceRoot = Join-Path $batchRoot 'evidence'
$dailyRoot = Join-Path $evidenceRoot 'daily'
$mainHash = '32C1E6F3D50C754A8E09DB169982551AE1CB4828760662AB106BDEE482012874'
$toolsRoot = 'E:\ST\STM32CubeIDE_1.13.2\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.11.3.rel1.win32_1.1.100.202309141235\tools\bin'

function Assert-SafePath([string] $Path) {
    $absolute = [IO.Path]::GetFullPath($Path)
    if (-not $absolute.StartsWith($projectRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path escapes project: $absolute"
    }
    $probe = $absolute
    while ($probe.Length -ge $projectRoot.Length) {
        if ((Test-Path -LiteralPath $probe) -and
            ((Get-Item -LiteralPath $probe -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Reparse point is not an approved target: $probe"
        }
        $probe = Split-Path $probe -Parent
    }
    return $absolute
}

function Save-Json([string] $Path, $Value) {
    $safe = Assert-SafePath $Path
    ConvertTo-Json -InputObject $Value -Depth 12 | Set-Content -LiteralPath $safe -Encoding utf8
}

function Get-SourceManifest([string[]] $Directories, [switch] $IncludeBuildInputs) {
    $paths = New-Object System.Collections.Generic.List[string]
    foreach ($relative in $Directories) {
        Get-ChildItem -LiteralPath (Join-Path $projectRoot $relative) -Recurse -File |
            Where-Object { $_.Extension.ToLowerInvariant() -in @('.c','.h','.s','.ld') } |
            ForEach-Object { $paths.Add($_.FullName) }
    }
    if ($IncludeBuildInputs) {
        foreach ($relative in @('hil/build_fw.ps1','hil/run_hil.ps1','hil/restore_project.ps1',
            'hil/repro_all.ps1','hil/adversarial_20261025/run_matrix.ps1',
            'hil/adversarial_20261025/compile_checks.ps1',
            'hil/adversarial_20261025/finalize_batch.ps1',
            'hil/adversarial_20261025/verify_evidence.py',
            'hil/basepri_20261025/run_matrix.ps1','hil/basepri_20261025/run_regressions.ps1',
            'hil/basepri_20261025/verify_evidence.py','Core/Src/main.c')) {
            $paths.Add((Join-Path $projectRoot $relative))
        }
        Get-ChildItem -LiteralPath $projectRoot -File -Filter '*.ld' |
            ForEach-Object { $paths.Add($_.FullName) }
    }
    return @($paths | Sort-Object -Unique | ForEach-Object {
        $safe = Assert-SafePath $_
        [ordered]@{path=$safe.Substring($projectRoot.Length + 1).Replace('\','/');
            sha256=(Get-FileHash -LiteralPath $safe -Algorithm SHA256).Hash}
    })
}

function Require-CompletedIndex([string] $Relative, [int] $Count) {
    $path = Assert-SafePath (Join-Path $evidenceRoot $Relative)
    # Windows PowerShell can emit the entire JSON array as one pipeline item.
    # Assign first, then enumerate that array explicitly for stable counts.
    $decoded = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    $rows = @($decoded)
    $failed = @($rows | Where-Object { $_.passed -ne $true }).Count
    $duplicates = @($rows | Group-Object tag | Where-Object { $_.Count -ne 1 }).Count
    $unfinished = @($rows | Where-Object {
        $_.PSObject.Properties.Name -contains 'stage' -and $_.stage -ne 'complete'
    }).Count
    if ($rows.Count -ne $Count -or $failed -ne 0 -or $duplicates -ne 0 -or $unfinished -ne 0) {
        throw "Incomplete prerequisite: $Relative (rows=$($rows.Count), expected=$Count, failed=$failed, duplicates=$duplicates, unfinished=$unfinished)"
    }
}

function Clear-OwnedCompileArtifacts {
    $directory = Assert-SafePath (Join-Path $projectRoot 'Debug/adversarial_compile_checks')
    $manifestPath = Join-Path $batchRoot 'cleanup_final_manifest.json'
    if (Test-Path -LiteralPath $manifestPath) { throw 'Final cleanup manifest already exists.' }
    $targets = New-Object System.Collections.Generic.List[string]
    foreach ($file in @(Get-ChildItem -LiteralPath $directory -File)) {
        if ($file.Name -notmatch '^(?:m[01]_a[01]_O[023]_(?:rtos_heap|rtos_perf|rtos_usb|port|rtos_mutex|rtos_sem|rtos_queue|rtos_event)\.(?:o|su)|cm7_port_compile_only\.o)$') {
            throw "Unrecognized compile artifact: $($file.FullName)"
        }
        $targets.Add($file.FullName)
    }
    if ($targets.Count -ne 193 -or @(Get-ChildItem -LiteralPath $directory -Directory).Count -ne 0) {
        throw 'Expected exactly the 193 reviewed compiler artifacts and no subdirectories.'
    }
    # Preserve every original stack-usage record before deleting its Debug copy.
    foreach ($file in @(Get-ChildItem -LiteralPath $directory -File -Filter '*.su')) {
        $retained = Join-Path $evidenceRoot ('compile/' + $file.Name)
        if (-not (Test-Path -LiteralPath $retained) -or
            (Get-FileHash -LiteralPath $retained).Hash -ne (Get-FileHash -LiteralPath $file.FullName).Hash) {
            throw "Missing or different retained stack evidence: $retained"
        }
    }
    foreach ($relative in @('Debug/adversarial_lifetime_agent_check.o','Debug/adversarial_wait_agent_check.o')) {
        $target = Assert-SafePath (Join-Path $projectRoot $relative)
        if (Test-Path -LiteralPath $target) { $targets.Add($target) }
    }
    $retainedBuildLogs = @{}
    foreach ($mapping in @(@('adversarial_mutex','am','matrix_final'),
        @('adversarial_wait','aw','matrix_final'), @('adversarial_lifetime','of','matrix_final'),
        @('adversarial_mutex_boundary','amb','matrix_final'),
        @('adversarial_registry','registry','registry'))) {
        $relative = 'hil/logs/build_' + $mapping[0] + '_1.txt'
        $target = Assert-SafePath (Join-Path $projectRoot $relative)
        $retained = Assert-SafePath (Join-Path $evidenceRoot ($mapping[2] +
            '/build_detail_' + $mapping[1] + '_primask_a0_O3.txt'))
        if (Test-Path -LiteralPath $target) {
            $tracked = @(& git -C $projectRoot ls-files -- $relative)
            if ($LASTEXITCODE -ne 0 -or $tracked.Count -ne 0 -or
                -not (Test-Path -LiteralPath $retained) -or
                (Get-FileHash -LiteralPath $target).Hash -ne
                (Get-FileHash -LiteralPath $retained).Hash) {
                throw "Refusing to remove a tracked or non-duplicated build log: $relative"
            }
            $targets.Add($target)
            $retainedBuildLogs[$target] = $retained.Substring($projectRoot.Length + 1).Replace('\','/')
        }
    }
    $entries = @($targets | ForEach-Object {
        $safe = Assert-SafePath $_
        [ordered]@{path=$safe.Substring($projectRoot.Length + 1).Replace('\','/');
            bytes=(Get-Item -LiteralPath $safe).Length;
            sha256=(Get-FileHash -LiteralPath $safe).Hash;
            reason=if ($retainedBuildLogs.ContainsKey($safe)) {
                'duplicate_build_log_with_identical_retained_evidence'
            } else { 'regenerable_compiler_artifact' };
            retained_copy=$retainedBuildLogs[$safe]}
    })
    $cleanup = [ordered]@{status='planned'; entries=$entries; deleted_files=0;
        deleted_bytes=0; started=(Get-Date).ToString('o')}
    Save-Json $manifestPath $cleanup
    foreach ($entry in $entries) {
        $safe = Assert-SafePath (Join-Path $projectRoot $entry.path)
        if ((Get-FileHash -LiteralPath $safe).Hash -ne $entry.sha256) {
            throw "Artifact changed after cleanup planning: $safe"
        }
        Remove-Item -LiteralPath $safe -Force
        $cleanup.deleted_files++
        $cleanup.deleted_bytes += $entry.bytes
    }
    $safeDirectory = Assert-SafePath $directory
    if (@(Get-ChildItem -LiteralPath $safeDirectory -Force).Count -eq 0) {
        Remove-Item -LiteralPath $safeDirectory -Force
    }
    $remaining = @($entries | Where-Object { Test-Path -LiteralPath (Join-Path $projectRoot $_.path) })
    if ($remaining.Count -ne 0) { throw 'Final cleanup verification failed.' }
    $cleanup.status = 'complete'
    $cleanup.completed = (Get-Date).ToString('o')
    $cleanup.recorded_targets_remaining = 0
    Save-Json $manifestPath $cleanup
}

Assert-SafePath $dailyRoot | Out-Null
if ((Test-Path -LiteralPath $dailyRoot) -and
    @(Get-ChildItem -LiteralPath $dailyRoot -File -Filter '*final_daily*').Count -ne 0) {
    throw 'final_daily evidence already exists; do not overwrite or reflash it.'
}
foreach ($requirement in @(
    @('matrix_final/matrix_results.json',48), @('registry/registry_results.json',12),
    @('port/matrix_results.json',12), @('port/blocking_results.json',12),
    @('port/nesting_results.json',6), @('port/guard_results.json',44),
    @('regression/regressions_functional.json',5), @('regression/regressions_production.json',4),
    @('soak/regressions_soak.json',1), @('compile/compile_results.json',97))) {
    Require-CompletedIndex $requirement[0] $requirement[1]
}
if ((Get-FileHash -LiteralPath (Join-Path $projectRoot 'Core/Src/main.c')).Hash -ne $mainHash) {
    throw 'Preserved daily main.c differs from the fixed identity.'
}
New-Item -ItemType Directory -Path $dailyRoot -Force | Out-Null
$before = @(Get-SourceManifest -Directories @('RTOS'))
Save-Json (Join-Path $dailyRoot 'kernel_before.json') $before
$state = [ordered]@{restored=$false; main_unchanged=$false; kernel_preserved=$false;
    build_files_restored=$false; new_test_symbols_absent=$false; daily_flashed=$false;
    variant_artifacts_removed=$false; daily_booted=$false; daily_tag='final_daily';
    main_sha256=$mainHash; started=(Get-Date).ToString('o')}
$statePath = Join-Path $dailyRoot 'restore_status.json'
Save-Json $statePath $state
try {
    $restoreStarted = [DateTime]::UtcNow
    & (Join-Path $projectRoot 'hil/restore_project.ps1') -Rebuild *>&1 |
        Tee-Object -FilePath (Join-Path $dailyRoot 'restore_final_daily_stdout.txt') | Out-Host
    $buildDetail = Join-Path $projectRoot 'hil/logs/build_restored_daily.txt'
    if (-not (Test-Path -LiteralPath $buildDetail) -or
        (Get-Item -LiteralPath $buildDetail).LastWriteTimeUtc -lt $restoreStarted) {
        throw 'No fresh restored daily build detail.'
    }
    Copy-Item -LiteralPath $buildDetail -Destination (Join-Path $dailyRoot 'build_final_daily.txt')
    $after = @(Get-SourceManifest -Directories @('RTOS'))
    Save-Json (Join-Path $dailyRoot 'kernel_after.json') $after
    if ((ConvertTo-Json -InputObject $before -Depth 4 -Compress) -ne
        (ConvertTo-Json -InputObject $after -Depth 4 -Compress)) { throw 'RTOS source changed during restoration.' }
    $state.kernel_preserved = $true
    if ((Get-FileHash -LiteralPath (Join-Path $projectRoot 'Core/Src/main.c')).Hash -ne $mainHash) {
        throw 'main.c changed during restoration.'
    }
    $state.main_unchanged = $true
    $pairs = @(@('Core/Src/subdir.mk','subdir.mk.template'),
        @('makefile','makefile.template'), @('objects.list','objects.list.template'))
    foreach ($relative in @('RTOS/Core/Src','RTOS/Port/GCC/ARM_CM7',
        'RTOS/lib/third/CherryUSB/class/msc','RTOS/lib/third/CherryUSB/core','RTOS/lib/third/CherryUSB/port/dwc2')) {
        $pairs += ,@(($relative + '/subdir.mk'), ($relative.Replace('/','_') + '.mk'))
    }
    foreach ($pair in $pairs) {
        $actual = [IO.File]::ReadAllText((Join-Path $projectRoot ('Debug/' + $pair[0]))).Replace("`r`n","`n")
        $template = [IO.File]::ReadAllText((Join-Path $projectRoot ('hil/templates/' + $pair[1]))).Replace("`r`n","`n")
        if ($actual -ne $template) { throw "Unrestored build template: $($pair[0])" }
    }
    $state.build_files_restored = $true
    $state.template_pairs = $pairs.Count
    foreach ($relative in @('hil/fwsrc','Debug/hil','Debug/.hil_build_stamp')) {
        if (Test-Path -LiteralPath (Join-Path $projectRoot $relative)) { throw "Variant artifact remains: $relative" }
    }
    $state.variant_artifacts_removed = $true
    $elf = Join-Path $projectRoot 'Debug/f446_rtos.elf'
    $symbols = @(& (Join-Path $toolsRoot 'arm-none-eabi-nm.exe') --defined-only --format=posix $elf 2>&1)
    if ($LASTEXITCODE -ne 0) { throw 'nm failed; do not claim test symbols are absent.' }
    $symbols | Set-Content -LiteralPath (Join-Path $dailyRoot 'nm_final_daily.txt') -Encoding utf8
    $names = @($symbols | ForEach-Object { ($_ -split '\s+')[0] })
    $forbidden = @($names | Where-Object {
        $_ -match '^(?:rtos_hil_|rtos_port_critical_test_hook$|__wrap_rtos_assert_fail$|audit_pre_start$|audit_regressions_task$|production_stress_task$|production_irq_start$|production_highirq_start$)'
    })
    if ($forbidden.Count -ne 0 -or $names -notcontains 'rtos_port_critical_nesting_error') {
        throw "Daily symbol check failed: $($forbidden -join ', ')"
    }
    $state.new_test_symbols_absent = $true
    & (Join-Path $projectRoot 'hil/run_hil.ps1') -Elf $elf -Tag 'final_daily' -Port $Port `
        -Seconds 15 -Terminator 'task 0 running' -OutDir $dailyRoot *>&1 |
        Tee-Object -FilePath (Join-Path $dailyRoot 'capture_final_daily_stdout.txt') | Out-Host
    $capture = Get-Content -LiteralPath (Join-Path $dailyRoot 'run_final_daily.json') -Raw | ConvertFrom-Json
    $text = Get-Content -LiteralPath (Join-Path $dailyRoot 'log_final_daily.txt') -Raw
    if ($capture.tag -ne 'final_daily' -or $capture.flashed -ne $true -or
        $capture.terminator_seen -ne $true -or $capture.port -ne $Port -or
        $capture.sha256 -ne (Get-FileHash -LiteralPath (Join-Path $dailyRoot 'firmware_final_daily.elf')).Hash -or
        $text -notmatch 'System init done' -or $text -notmatch 'task 0 running' -or
        $text -match '\|FAIL(?:\||\r?\n)' -or
        $text -match '(?m)^(?:AM|AMB|AW|OF|AR|BP|BLOCK|NEST|AT|STRESS|BT|LC)\|') {
        throw 'Restored daily image did not satisfy the saved capture checks.'
    }
    $state.daily_flashed = $true
    $state.daily_booted = $true
    if (-not $KeepCompileArtifacts) { Clear-OwnedCompileArtifacts }
    $finalKernel = @(Get-SourceManifest -Directories @('RTOS'))
    if ((ConvertTo-Json -InputObject $before -Depth 4 -Compress) -ne
        (ConvertTo-Json -InputObject $finalKernel -Depth 4 -Compress) -or
        (Get-FileHash -LiteralPath (Join-Path $projectRoot 'Core/Src/main.c')).Hash -ne $mainHash) {
        throw 'Source identity changed after the daily capture.'
    }
    $final = @(Get-SourceManifest -Directories @('RTOS','Core','hil/fw') -IncludeBuildInputs)
    Save-Json (Join-Path $batchRoot 'sources_final.json') $final
    $state.restored = $true
    $state.completed = (Get-Date).ToString('o')
    Save-Json $statePath $state
    Write-Host 'Daily restoration complete. Run the independent verifier separately.'
} catch {
    $state.error = $_.Exception.Message
    $state.failed = (Get-Date).ToString('o')
    Save-Json $statePath $state
    throw
}
