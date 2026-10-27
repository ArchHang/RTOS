param(
    [string] $Port = 'COM3',
    [string] $OutDir = '',
    [ValidateRange(10,300)][int] $Seconds = 45,
    [switch] $RegistryOnly,
    [ValidateRange(1,10000)][int] $ExpectedRegistryChecks = 1
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $PSScriptRoot 'evidence_helpers.ps1')
$evidenceRoot = if ($OutDir -eq '') {
    Join-Path $PSScriptRoot ('rerun_' + (Get-Date).ToString('yyyyMMdd_HHmmss'))
} else { [System.IO.Path]::GetFullPath($OutDir) }
$historicalRoot = [System.IO.Path]::GetFullPath((Join-Path $projectRoot 'hil/basepri_20261025'))
if ($evidenceRoot.StartsWith($historicalRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'This batch must not write into historical basepri_20261025 evidence.'
}
New-Item -ItemType Directory -Path $evidenceRoot -Force | Out-Null
if ($RegistryOnly -and -not $PSBoundParameters.ContainsKey('ExpectedRegistryChecks')) {
    throw 'RegistryOnly requires the explicitly reviewed expected check count.'
}
$indexName = if ($RegistryOnly) { 'registry_results.json' } else { 'matrix_results.json' }
$indexPath = Join-Path $evidenceRoot $indexName
if (Test-Path -LiteralPath $indexPath) { throw "Existing result index: $indexPath" }
$buildScript = Join-Path $projectRoot 'hil/build_fw.ps1'
$runScript = Join-Path $projectRoot 'hil/run_hil.ps1'
$firmware = Join-Path $projectRoot 'Debug/f446_rtos.elf'
$results = New-Object System.Collections.Generic.List[object]
$variants = @(
    @{ name='am'; prefix='AM'; variant='adversarial_mutex'; pass=910; cases=15 },
    @{ name='aw'; prefix='AW'; variant='adversarial_wait'; pass=1600; cases=0 },
    @{ name='of'; prefix='OF'; variant='adversarial_lifetime'; pass=80; cases=80 },
    @{ name='amb'; prefix='AMB'; variant='adversarial_mutex_boundary'; pass=87; cases=0 }
)
if ($RegistryOnly) {
    $variants = @(@{name='registry'; prefix='AR'; variant='adversarial_registry';
        pass=$ExpectedRegistryChecks; cases=0})
}

function Get-SourceManifest {
    return @(Get-RefactorSourceManifest -ProjectRoot $projectRoot)
}

function Save-Results {
    # InputObject preserves an array even while the first image is in progress.
    ConvertTo-Json -InputObject @($results.ToArray()) -Depth 8 |
        Set-Content -LiteralPath $indexPath -Encoding utf8
}

foreach ($variant in $variants) {
    foreach ($mask in @('basepri','primask')) {
        foreach ($assertions in @(1,0)) {
            foreach ($optimization in @('O0','O2','O3')) {
                $tag = "$($variant.name)_${mask}_a${assertions}_${optimization}"
                $prefix = $variant.prefix
                foreach ($stem in @('build_','build_detail_','sources_','log_','run_',
                                     'flash_','firmware_','capture_stdout_')) {
                    if (@(Get-ChildItem -LiteralPath $evidenceRoot -Filter "$stem$tag.*").Count) {
                        throw "Refusing to replace existing evidence: $tag"
                    }
                }
                $entry = [ordered]@{ tag=$tag; suite=$variant.name; prefix=$prefix;
                    variant=$variant.variant; critical_mask=$mask; assertions=$assertions;
                    optimization=$optimization; passed=$false; stage='source_snapshot';
                    started=(Get-Date).ToString('o') }
                if ($RegistryOnly) { $entry.expected_checks = $ExpectedRegistryChecks }
                $results.Add($entry)
                Save-Results
                try {
                    $manifest = @(Get-SourceManifest)
                    $sourcePath = Join-Path $evidenceRoot "sources_$tag.json"
                    ConvertTo-Json -InputObject $manifest -Depth 4 |
                        Set-Content -LiteralPath $sourcePath -Encoding utf8
                    $entry.source_manifest = "sources_$tag.json"
                    $entry.source_manifest_sha256 = (Get-FileHash $sourcePath -Algorithm SHA256).Hash
                    $entry.stage = 'build'
                    Save-Results
                    $arguments = @('-Variant',$variant.variant,'-CriticalMask',$mask,
                        '-Optimization',$optimization)
                    if ($assertions -eq 0) { $arguments += '-DisableAssertions' }
                    # Ensure the first image has fresh compile-command evidence,
                    # including when a preceding aborted run built this same tag.
                    if ($results.Count -eq 1) { $arguments += '-Clean' }
                    $buildOutput = & powershell -NoProfile -ExecutionPolicy Bypass `
                        -File $buildScript @arguments 2>&1
                    $buildCode = $LASTEXITCODE
                    $buildOutput | Out-File -LiteralPath (Join-Path $evidenceRoot "build_$tag.txt") `
                        -Encoding utf8
                    $detail = Join-Path $projectRoot "hil/logs/build_$($variant.variant)_1.txt"
                    if (Test-Path -LiteralPath $detail) {
                        Copy-Item -LiteralPath $detail `
                            -Destination (Join-Path $evidenceRoot "build_detail_$tag.txt")
                    }
                    if ($buildCode -ne 0) { throw "Build exit $buildCode" }
                    $buildText = ($buildOutput -join "`n")
                    $detailText = [IO.File]::ReadAllText($detail)
                    $warningLines = @($detailText -split "`r?`n" |
                        Where-Object { $_ -match 'warning:' })
                    $unexpectedWarnings = @($warningLines | Where-Object {
                        $_ -notmatch '^(?:make(?:\.exe)? : )?\.\./Core/Src/main\.c:\d+:\d+: warning: .+ \[-Wunused-(?:function|variable)\]\s*$'
                    })
                    if ($detailText -match '(?:fatal )?error:' -or
                        $unexpectedWarnings.Count -ne 0 -or $warningLines.Count -ne 25 -or
                        $buildText -notmatch 'warnings=25\b') {
                        throw 'Build diagnostics differ from the 25 existing unused demo declarations in main.c.'
                    }
                    $fixture = [regex]::Escape($variant.variant + '.c')
                    $fixtureCommands = @($detailText -split "`r?`n" | Where-Object {
                        $_ -match "arm-none-eabi-gcc .*hil/fwsrc/$fixture" -and
                        $_ -match '(?:^| )-Wextra(?: |$)' -and $_ -match '(?:^| )-Werror(?: |$)'
                    })
                    if ($fixtureCommands.Count -ne 1) { throw 'Missing strict fixture compilation.' }
                    $entry.warning_count = $warningLines.Count
                    $entry.allowed_existing_main_unused_warnings = $warningLines.Count
                    $entry.stage = 'capture'
                    Save-Results
                    $capture = & powershell -NoProfile -ExecutionPolicy Bypass -File $runScript `
                        -Elf $firmware -Tag $tag -Seconds $Seconds `
                        -Terminator "$prefix\|DONE\|status=" -Port $Port -OutDir $evidenceRoot 2>&1
                    $captureCode = $LASTEXITCODE
                    $capture | Out-File -LiteralPath (Join-Path $evidenceRoot "capture_stdout_$tag.txt") `
                        -Encoding utf8
                    if ($captureCode -ne 0) { throw "Capture exit $captureCode" }
                    $entry.stage = 'validate'
                    $text = [IO.File]::ReadAllText((Join-Path $evidenceRoot "log_$tag.txt"))
                    if ($text -match '\|FAIL(?:\||\r?\n)') { throw 'Firmware emitted FAIL.' }
                    $summaries = @($text -split "`r?`n" |
                        Where-Object { $_ -match "^$prefix\|SUMMARY\|" })
                    $done = @($text -split "`r?`n" |
                        Where-Object { $_ -match "^$prefix\|DONE\|" })
                    if ($summaries.Count -ne 1 -or $done.Count -ne 1 -or
                        $done[0] -notmatch "^$prefix\|DONE\|status=0\s*$") {
                        throw 'Expected exactly one SUMMARY and one successful DONE.'
                    }
                    if ($summaries[0] -notmatch "^$prefix\|SUMMARY\|pass=$($variant.pass) fail=0(?: |$)") {
                        throw "Unexpected summary: $($summaries[0])"
                    }
                    if ($variant.cases -ne 0 -and
                        $summaries[0] -notmatch " cases=$($variant.cases)\s*$") {
                        throw 'Missing expected complete case count.'
                    }
                    if ($prefix -eq 'AW' -and
                        $summaries[0] -notmatch ' rounds=8 phases=73 irq_requests=120\s*$') {
                        throw 'Incomplete mixed-wait phase/IRQ matrix.'
                    }
                    if ($RegistryOnly -and
                        $summaries[0] -notmatch " checks=$ExpectedRegistryChecks(?: |$)") {
                        throw 'Incomplete registry-capacity boundary checks.'
                    }
                    $after = @(Get-SourceManifest)
                    if ((ConvertTo-Json -InputObject $manifest -Compress) -cne
                        (ConvertTo-Json -InputObject $after -Compress)) {
                        throw 'Sources changed while building or capturing this image.'
                    }
                    $entry.summary = $summaries[0]
                    $entry.passed = $true
                    $entry.stage = 'complete'
                    $entry.finished = (Get-Date).ToString('o')
                    Save-Results
                    Write-Host "$tag PASS"
                } catch {
                    $entry.error = $_.Exception.Message
                    $entry.finished = (Get-Date).ToString('o')
                    Save-Results
                    throw "${tag}: $($_.Exception.Message); evidence=$evidenceRoot"
                }
            }
        }
    }
}
Write-Host "Verified $($results.Count)/$($variants.Count * 12) hardware images; evidence: $evidenceRoot"
