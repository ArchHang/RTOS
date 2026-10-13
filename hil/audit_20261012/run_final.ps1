param([string[]] $Only = @('audit','selftest','boundary','bench','scenarios'))
$ErrorActionPreference = 'Stop'
$projectRoot = 'E:\STM32_Project\CubeIDE\f446_rtos'
$hilRoot = Join-Path $projectRoot 'hil'
$outputRoot = Join-Path $hilRoot 'audit_20261012'
$elfPath = Join-Path $projectRoot 'Debug\f446_rtos.elf'
$results = @()

function Build-Variant([string] $Variant, [int] $Scenario = 1) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $hilRoot 'build_fw.ps1') -Variant $Variant -Scenario $Scenario
    if ($LASTEXITCODE -ne 0) { throw "build failed: $Variant/$Scenario" }
    Copy-Item -LiteralPath (Join-Path $hilRoot "logs/build_${Variant}_${Scenario}.txt") -Destination (Join-Path $outputRoot "build_final_${Variant}_${Scenario}.txt") -Force
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
        $tag = "audit_20261012_verified_audit$iteration"
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
    $tag = 'audit_20261012_verified_selftest'
    $raw = Run-Case $tag 'ST\|SUMMARY' 150 'ST\|ISR\|ARM'
    if ($raw -notmatch 'ST\|SUMMARY\|pass=432 fail=0 info=10 total=442\|END') { throw 'selftest summary mismatch' }
    $results += [ordered]@{ tag=$tag; suite='selftest'; passed=432; failed=0; info=10; total=442 }
}
if ($Only -contains 'boundary') {
    Build-Variant 'boundary'
    $tag = 'audit_20261012_verified_boundary'
    $raw = Run-Case $tag 'BT\|SUMMARY' 60
    if ($raw -notmatch 'BT\|SUMMARY\|pass=103 fail=0') { throw 'boundary summary mismatch' }
    $results += [ordered]@{ tag=$tag; suite='boundary'; passed=103; failed=0 }
}
if ($Only -contains 'bench') {
    Build-Variant 'bench'
    $tag = 'audit_20261012_verified_bench'
    $raw = Run-Case $tag 'BM\|DONE' 45
    $results += [ordered]@{ tag=$tag; suite='bench'; completed=$true }
}
if ($Only -contains 'scenarios') {
    foreach ($scenario in 1..5) {
        Build-Variant 'scenario_usb' $scenario
        $tag = "audit_20261012_verified_S$scenario"
        $raw = Run-Case $tag 'SC\|DONE' 150
        if ($raw -notmatch "SC\|DONE\|scenario=$scenario\|status=0") { throw "scenario failed: S$scenario" }
        $results += [ordered]@{ tag=$tag; suite="S$scenario"; status=0 }
    }
}
$results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $outputRoot 'verified_results.json') -Encoding utf8
Write-Host 'All selected suites completed; evidence in hil/audit_20261012.'
