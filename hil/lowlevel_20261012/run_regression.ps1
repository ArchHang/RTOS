param(
    [string[]] $Only = @('audit','selftest','boundary','bench','scenarios'),
    [ValidatePattern('^[a-z0-9_]+$')][string] $Label = 'final_selected'
)
$ErrorActionPreference = 'Stop'
$projectRoot = 'E:\STM32_Project\CubeIDE\f446_rtos'
$hilRoot = Join-Path $projectRoot 'hil'
$outputRoot = Join-Path $hilRoot 'lowlevel_20261012'
$elfPath = Join-Path $projectRoot 'Debug\f446_rtos.elf'
$results = @()
$summaryPath = Join-Path $outputRoot "regression_${Label}_results.json"
if (Test-Path -LiteralPath $summaryPath) { throw "Do not overwrite completed regression: $Label" }
foreach ($relative in @('Debug/RTOS','Debug/Core/Src','Debug/hil')) {
    $objectRoot = [System.IO.Path]::GetFullPath((Join-Path $projectRoot $relative))
    if (-not $objectRoot.StartsWith($projectRoot + '\', [System.StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe object cleanup root' }
    if (Test-Path -LiteralPath $objectRoot) {
        Get-ChildItem -LiteralPath $objectRoot -Recurse -File | Where-Object { $_.Extension -in '.o','.d' } |
            ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force }
    }
}

function Build-Variant([string] $Variant, [int] $Scenario = 1) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $hilRoot 'build_fw.ps1') -Variant $Variant -Scenario $Scenario
    if ($LASTEXITCODE -ne 0) { throw "build failed: $Variant/$Scenario" }
    Copy-Item -LiteralPath (Join-Path $hilRoot "logs/build_${Variant}_${Scenario}.txt") -Destination (Join-Path $outputRoot "build_${Label}_${Variant}_${Scenario}.txt") -Force
}

function Run-Case([string] $Tag, [string] $Terminator, [int] $Seconds, [string] $ReplyOn = '') {
    $arguments = @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $hilRoot 'run_hil.ps1'),
                   '-Elf',$elfPath,'-Tag',$Tag,'-Seconds',"$Seconds",'-Terminator',$Terminator,'-OutDir',$outputRoot)
    if ($ReplyOn -ne '') { $arguments += @('-ReplyOn',$ReplyOn) }
    $capture = & powershell.exe @arguments 2>&1
    if ($LASTEXITCODE -ne 0) { $capture | Select-Object -Last 12 | Write-Host; throw "run failed: $Tag" }
    $raw = [System.IO.File]::ReadAllText((Join-Path $outputRoot "log_$Tag.txt"))
    $raw -split "`r?`n" | Where-Object { $_ -match '\|SUMMARY|^BM\||^SC\|' } | Write-Host
    return $raw
}

if ($Only -contains 'audit') {
    Build-Variant 'audit'
    $previousHash = ''
    foreach ($iteration in 1..3) {
        $tag = "lowlevel_20261012_${Label}_audit$iteration"
        $raw = Run-Case $tag 'AT\|DONE' 30
        foreach ($expect in @('SCHEDTASK\|SUMMARY\|pass=25 fail=0','IPC\|SUMMARY\|pass=15 fail=0',
                              'MEMTIMER\|SUMMARY\|pass=23\|fail=0','USBPORT\|SUMMARY\|pass=54\|fail=0')) {
            if ($raw -notmatch $expect) { throw "audit summary mismatch: $expect" }
        }
        $hash = (Get-FileHash -LiteralPath $elfPath -Algorithm SHA256).Hash
        if ($previousHash -ne '' -and $previousHash -ne $hash) { throw 'repeated audit firmware changed' }
        $previousHash = $hash
        $results += [ordered]@{ tag=$tag; suite='audit'; passed=117; failed=0; sha256=$hash }
    }
}
if ($Only -contains 'selftest') {
    Build-Variant 'selftest'
    $tag = "lowlevel_20261012_${Label}_selftest"
    $raw = Run-Case $tag 'ST\|SUMMARY' 150 'ST\|ISR\|ARM'
    if ($raw -notmatch 'ST\|SUMMARY\|pass=432 fail=0 info=10 total=442\|END') { throw 'selftest summary mismatch' }
    $results += [ordered]@{ tag=$tag; suite='selftest'; passed=432; failed=0; info=10; total=442 }
}
if ($Only -contains 'boundary') {
    Build-Variant 'boundary'
    $tag = "lowlevel_20261012_${Label}_boundary"
    $raw = Run-Case $tag 'BT\|SUMMARY' 60
    if ($raw -notmatch 'BT\|SUMMARY\|pass=103 fail=0') { throw 'boundary summary mismatch' }
    $results += [ordered]@{ tag=$tag; suite='boundary'; passed=103; failed=0 }
}
if ($Only -contains 'bench') {
    Build-Variant 'bench'
    $tag = "lowlevel_20261012_${Label}_bench"
    $raw = Run-Case $tag 'BM\|DONE' 45
    $results += [ordered]@{ tag=$tag; suite='bench'; completed=$true }
}
if ($Only -contains 'scenarios') {
    foreach ($scenario in 1..5) {
        Build-Variant 'scenario_usb' $scenario
        $tag = "lowlevel_20261012_${Label}_S$scenario"
        $raw = Run-Case $tag 'SC\|DONE' 150
        if ($raw -notmatch "SC\|DONE\|scenario=$scenario\|status=0") { throw "scenario failed: S$scenario" }
        $results += [ordered]@{ tag=$tag; suite="S$scenario"; status=0 }
    }
}
$results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $summaryPath -Encoding utf8
Write-Host 'All selected suites completed; evidence in hil/lowlevel_20261012.'
