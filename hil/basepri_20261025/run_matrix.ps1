param(
    [ValidateSet('mask','guard','blocking','nesting','all')][string] $Suite = 'all',
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

function Invoke-Image([string] $tag, [string[]] $buildArguments, [int] $seconds,
                      [string] $prefix = 'BP') {
    $buildOutput = & powershell -NoProfile -ExecutionPolicy Bypass -File $buildScript @buildArguments 2>&1
    $buildCode = $LASTEXITCODE
    $buildOutput | Out-File -LiteralPath (Join-Path $evidenceRoot "build_$tag.txt") -Encoding utf8
    if ($buildCode -ne 0) { throw "Build failed: $tag" }
    $captureOutput = & powershell -NoProfile -ExecutionPolicy Bypass -File $runScript `
        -Elf $firmware -Tag $tag -Seconds $seconds -Terminator "$prefix\|DONE\|status=" `
        -Port $Port -OutDir $evidenceRoot 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Hardware run failed: $tag : $captureOutput" }
    $content = [System.IO.File]::ReadAllText((Join-Path $evidenceRoot "log_$tag.txt"))
    if ($content -notmatch "$prefix\|DONE\|status=0" -or $content -match '\|FAIL(?:\||\r?\n)') {
        throw "Firmware failure: $tag"
    }
    return $content
}

if ($Suite -in 'mask','all') {
    foreach ($mask in @('basepri','primask')) {
        foreach ($assertions in @(1,0)) {
            foreach ($optimization in @('O0','O2','O3')) {
                $tag = "matrix_${mask}_a${assertions}_${optimization}"
                $arguments = @('-Variant','basepri','-CriticalMask',$mask,'-Optimization',$optimization)
                if ($assertions -eq 0) { $arguments += '-DisableAssertions' }
                $content = Invoke-Image $tag $arguments 90
                $cases = @($content -split "`r?`n" | Where-Object { $_ -match '^BP\|CASE\|' })
                if ($cases.Count -ne 600 -or @($cases | Where-Object { $_ -notmatch '^BP\|CASE\|PASS\|' }).Count -ne 0) {
                    throw "Incomplete case matrix: $tag"
                }
                if ($content -notmatch 'BP\|STRESS\|pairs=1000000 requested=1000000 irq=\d+ irq_errors=0 thread_leaks=0') {
                    throw "Incomplete or failed asynchronous stress: $tag"
                }
                foreach ($line in $cases) {
                    if ($line -notmatch 'stage=(\d+) prio=(\d+) in_bp=0x([0-9A-F]+) in_pm=(\d)') {
                        throw "Malformed case: $tag"
                    }
                    $priority = [int]$Matches[2]
                    $basepri = [Convert]::ToUInt32($Matches[3],16)
                    $primask = [int]$Matches[4]
                    if ($mask -eq 'basepri' -and $line -notmatch 'hook_pm=1 in_hook=0 ') {
                        throw "IRQ entered an atomic metadata transaction: $tag $line"
                    }
                    $heldByOriginalMask = ($primask -eq 1) -or
                        ($basepri -ne 0 -and (($priority -shl 4) -ge $basepri))
                    if ($heldByOriginalMask -and $line -notmatch 'before_repair=0 ') {
                        throw "An inherited mask was relaxed before explicit test restoration: $tag $line"
                    }
                }
                $summary = ($content -split "`r?`n" | Where-Object { $_ -match '^BP\|SUMMARY\|' }) -join ''
                $results.Add([ordered]@{tag=$tag; suite='mask'; cases=600; passed=$true; summary=$summary})
                Write-Host "$tag PASS $summary"
                @($results | Where-Object { $_.suite -eq 'mask' }) | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $evidenceRoot 'matrix_results.json') -Encoding utf8
            }
        }
    }
}

if ($Suite -in 'blocking','all') {
    foreach ($mask in @('basepri','primask')) {
        foreach ($assertions in @(1,0)) {
            foreach ($optimization in @('O0','O2','O3')) {
                $tag = "blocking_${mask}_a${assertions}_${optimization}"
                $arguments = @('-Variant','basepri_blocking','-CriticalMask',$mask,
                    '-Optimization',$optimization)
                if ($assertions -eq 0) { $arguments += '-DisableAssertions' }
                $content = Invoke-Image $tag $arguments 30 'BLOCK'
                $kinds = @($content -split "`r?`n" | Where-Object { $_ -match '^BLOCK\|KIND\|' })
                if ($kinds.Count -ne 5 -or @($kinds | Where-Object { $_ -notmatch 'attempts=100 fail=0 ' }).Count -ne 0) {
                    throw "Blocking return matrix failed: $tag"
                }
                $summary = ($content -split "`r?`n" | Where-Object { $_ -match '^BLOCK\|SUMMARY\|' }) -join ''
                $results.Add([ordered]@{tag=$tag; suite='blocking'; attempts=500; passed=$true; summary=$summary})
                Write-Host "$tag PASS $summary"
                @($results | Where-Object { $_.suite -eq 'blocking' }) | ConvertTo-Json -Depth 5 |
                    Set-Content -LiteralPath (Join-Path $evidenceRoot 'blocking_results.json') -Encoding utf8
            }
        }
    }
}

if ($Suite -in 'nesting','all') {
    foreach ($mask in @('basepri','primask')) {
        foreach ($optimization in @('O0','O2','O3')) {
            $tag = "nesting_${mask}_${optimization}"
            $content = Invoke-Image $tag @('-Variant','basepri_nesting','-CriticalMask',$mask,
                '-Optimization',$optimization,'-DisableAssertions') 20 'NEST'
            if ($content -notmatch 'NEST\|SUMMARY\|pass=20 fail=0 cases=20') {
                throw "Assertion-disabled invalid nesting failed: $tag"
            }
            $results.Add([ordered]@{tag=$tag; suite='invalid_nesting'; cases=20; passed=$true})
            Write-Host "$tag PASS (20 invalid-state preservation cases)"
            @($results | Where-Object { $_.suite -eq 'invalid_nesting' }) | ConvertTo-Json -Depth 5 |
                Set-Content -LiteralPath (Join-Path $evidenceRoot 'nesting_results.json') -Encoding utf8
        }
    }
}

if ($Suite -in 'guard','all') {
    foreach ($priority in @(0,4,5)) {
        foreach ($case in (1..14)) {
            $tag = "guard_c${case}_p${priority}"
            $content = Invoke-Image $tag @('-Variant','basepri_guard','-CriticalGuardCase',"$case",
                '-CriticalGuardPriority',"$priority") 10
            if ($content -notmatch "BP\|GUARD\|PASS\|case=$case prio=$priority ") {
                throw "Missing expected priority-contract result: $tag"
            }
            if ($priority -lt 5 -and $content -notmatch 'BP\|ASSERT\|') {
                throw "Missing expected assertion: $tag"
            }
            $results.Add([ordered]@{tag=$tag; suite='guard'; case=$case; priority=$priority; passed=$true})
            Write-Host "$tag PASS"
            @($results | Where-Object { $_.suite -in 'guard','nesting' }) | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $evidenceRoot 'guard_results.json') -Encoding utf8
        }
    }
    foreach ($case in @(15,16)) {
        $tag = "guard_c${case}_p0"
        $content = Invoke-Image $tag @('-Variant','basepri_guard','-CriticalGuardCase',"$case",
            '-CriticalGuardPriority','0') 10
        if ($content -notmatch "BP\|GUARD\|PASS\|case=$case prio=0 ") { throw "Nesting guard failed: $tag" }
        $results.Add([ordered]@{tag=$tag; suite='nesting'; case=$case; priority=0; passed=$true})
        Write-Host "$tag PASS"
    }
    @($results | Where-Object { $_.suite -in 'guard','nesting' }) | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $evidenceRoot 'guard_results.json') -Encoding utf8
}
Write-Host "Verified $($results.Count) hardware images; evidence: $evidenceRoot"
