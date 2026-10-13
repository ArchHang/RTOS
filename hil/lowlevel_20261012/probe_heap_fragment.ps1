param([ValidatePattern('^[a-z0-9_]+$')][string] $Label = 'hotplug')
$ErrorActionPreference = 'Stop'
$projectRoot = 'E:\STM32_Project\CubeIDE\f446_rtos'
$outputRoot = Join-Path $projectRoot 'hil\lowlevel_20261012'
$tag = "lowlevel_20261012_heap_fragment_$Label"
$elf = Join-Path $outputRoot 'firmware_lowlevel_20261012_checks_final_flash_r1.elf'
if (Test-Path -LiteralPath (Join-Path $outputRoot "run_$tag.json")) {
    throw 'Do not overwrite the diagnostic recheck'
}
$toolRoot = 'E:\ST\STM32CubeIDE_1.13.2\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.11.3.rel1.win32_1.1.100.202309141235\tools\bin'
$programmer = 'E:\ST\STM32CubeIDE_1.13.2\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.1.100.202311100844\tools\bin\STM32_Programmer_CLI.exe'
$symbols = & (Join-Path $toolRoot 'arm-none-eabi-nm.exe') -an $elf 2>&1
if ($LASTEXITCODE -ne 0) { throw 'Saved diagnostic ELF symbols could not be read' }
$addresses = @{}
foreach ($name in @('s_heap_buf','s_alloc_count','s_free_count')) {
    $entries = @($symbols | Where-Object { $_ -match ('^[0-9a-fA-F]+ [bBdD] ' + $name + '$') })
    if ($entries.Count -ne 1) { throw "Ambiguous heap symbol: $name" }
    $addresses[$name] = [Convert]::ToUInt32(($entries[0] -split ' ')[0], 16)
}
if ($addresses['s_heap_buf'] -ne 0x20001320) { throw 'Model addresses do not match saved ELF' }
$symbols | Where-Object { $_ -match 's_heap_buf|s_alloc_count|s_free_count' } |
    Out-File -LiteralPath (Join-Path $outputRoot "heap_fragment_${Label}_symbols.txt") -Encoding utf8
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $projectRoot 'hil\run_hil.ps1') `
    -Elf $elf -Tag $tag -Seconds 90 -Terminator 'BM\|DONE' -OutDir $outputRoot -AllowFailures |
    Out-File -LiteralPath (Join-Path $outputRoot "heap_fragment_${Label}_capture_console.txt") -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw 'Diagnostic image did not reach its end' }
$reads = @(
    @{name='free_header_a'; address=[uint32]0x200023C8; length=8; expected='800004A8'},
    @{name='free_header_b'; address=[uint32]0x20002870; length=8; expected='80006AB0'},
    @{name='alloc_count'; address=$addresses['s_alloc_count']; length=4; expected=''},
    @{name='free_count'; address=$addresses['s_free_count']; length=4; expected=''}
)
foreach ($entry in $reads) {
    $addressText = '0x{0:X8}' -f $entry.address
    # Hotplug keeps the completed diagnostic RAM intact; Under Reset restarts it.
    $read = & $programmer -c port=SWD mode=HOTPLUG -r32 $addressText $entry.length 2>&1
    $code = $LASTEXITCODE
    $read | Out-File -LiteralPath (Join-Path $outputRoot ("heap_fragment_${Label}_" + $entry.name + '.txt')) `
        -Encoding utf8
    if ($code -ne 0) { throw "RAM read failed: $($entry.name)" }
    $raw = $read -join "`n"
    if (($entry.expected -ne '') -and
        (($raw -notmatch $entry.expected) -or ($raw -notmatch 'A5A5A5A5'))) {
        throw "Actual free header does not match the source model: $($entry.name)"
    }
    $read | Where-Object { $_ -match '0x2000' } | Write-Host
}
[ordered]@{ tag=$tag; sha256=(Get-FileHash -LiteralPath $elf).Hash;
    expected_diagnostic_failure=$true; hardware_headers_match=$true;
    physical_free_bytes=28504; payload_free_bytes=28488; extra_header_bytes=8 } |
    ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $outputRoot "heap_fragment_${Label}_verified.json") `
    -Encoding utf8
Write-Host 'Actual board RAM headers match the fragmentation model; this was a diagnostic failure, not a passing cache comparison.'
