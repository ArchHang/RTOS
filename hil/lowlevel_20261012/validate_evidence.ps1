$ErrorActionPreference = 'Stop'
$projectRoot = 'E:\STM32_Project\CubeIDE\f446_rtos'
$evidenceRoot = Join-Path $projectRoot 'hil\lowlevel_20261012'
$excluded = @('lowlevel_20261012_checks_final_flash_r1',
    'lowlevel_20261012_heap_fragment_recheck','lowlevel_20261012_heap_fragment_hotplug')
$runs = @(Get-ChildItem -LiteralPath $evidenceRoot -Filter 'run_*.json')
$verified = @()
foreach ($file in $runs) {
    $meta = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
    $logPath = Join-Path $evidenceRoot ("log_" + $meta.tag + '.txt')
    $firmwarePath = Join-Path $evidenceRoot ("firmware_" + $meta.tag + '.elf')
    if (-not (Test-Path -LiteralPath $logPath) -or
        -not (Test-Path -LiteralPath $firmwarePath)) { throw "Missing capture: $($meta.tag)" }
    if ((Get-FileHash -LiteralPath $firmwarePath).Hash -ne $meta.sha256) {
        throw "Saved ELF checksum mismatch: $($meta.tag)"
    }
    if (-not $meta.flashed -or -not $meta.terminator_seen -or $meta.port -ne 'COM3') {
        throw "Capture provenance mismatch: $($meta.tag)"
    }
    $isDiagnosticFailure = $excluded -contains $meta.tag
    if ($meta.failure_lines.Count -ne 0 -and -not $isDiagnosticFailure) {
        throw "Unexpected failed assertion: $($meta.tag)"
    }
    $verified += [pscustomobject]@{tag=$meta.tag;sha256=$meta.sha256;
        classification=$(if ($isDiagnosticFailure) { 'excluded_diagnostic_failure' } else { 'passed' })}
}
$stages = @(Get-ChildItem -LiteralPath $evidenceRoot -Filter 'measurement_*.json')
foreach ($file in $stages) {
    $stage = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
    if ($stage.repeated -ne 3 -or $stage.runs.Count -ne 3) { throw 'Stage repetitions missing' }
    foreach ($run in $stage.runs) {
        $record = @($verified | Where-Object tag -eq $run.tag)
        if ($record.Count -ne 1 -or $record[0].sha256 -ne $stage.sha256 -or
            $record[0].classification -ne 'passed') { throw "Stage provenance failed: $($run.tag)" }
    }
}
$baseline = Get-Content -LiteralPath (Join-Path $evidenceRoot 'sources_before.json') -Raw |
    ConvertFrom-Json
$final = Get-Content -LiteralPath (Join-Path $evidenceRoot 'sources_final_daily.json') -Raw |
    ConvertFrom-Json
$changed = @()
foreach ($source in $final) {
    if ((Get-FileHash -LiteralPath (Join-Path $projectRoot $source.path)).Hash -ne $source.sha256) {
        throw "Current source differs from final daily: $($source.path)"
    }
    $old = $baseline | Where-Object path -eq $source.path
    if ($old.sha256 -ne $source.sha256) { $changed += $source.path }
}
$matrix = Get-Content -LiteralPath (Join-Path $evidenceRoot 'config_matrix_final\matrix.json') `
    -Raw | ConvertFrom-Json
$units = @($matrix.configurations | ForEach-Object units)
if ($units.Count -ne 91 -or @($units | Where-Object exitCode -ne 0).Count -ne 0 -or
    @($units | ForEach-Object warnings).Count -ne 0) { throw 'Compile matrix incomplete' }
[ordered]@{ total_captures=$runs.Count; successful_captures=@($verified | Where-Object classification -eq 'passed').Count;
    excluded_diagnostic_failures=$excluded; performance_stages=$stages.Count;
    compiled_units=$units.Count; compile_warnings=0; final_source_count=$final.Count;
    changed_production_files=$changed; all_saved_elf_hashes_match=$true;
    final_source_matches_daily=$true; captures=$verified } |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $evidenceRoot 'evidence_validation.json') `
        -Encoding utf8
Write-Host "Validated $($runs.Count) board captures, $($stages.Count) performance stages, 91 compiler invocations, and $($final.Count) final source hashes."
