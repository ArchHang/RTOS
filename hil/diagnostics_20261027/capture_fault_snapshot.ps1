param(
    [Parameter(Mandatory = $true)][string] $CaptureDirectory
)
$ErrorActionPreference = 'Stop'
$capture = (Resolve-Path -LiteralPath $CaptureDirectory).Path
$identityPath = Join-Path $capture 'identity.json'
$identity = Get-Content -LiteralPath $identityPath -Raw | ConvertFrom-Json
if (-not $identity.passed -or $identity.variant -ne 'diagnostic_fault' -or
    $identity.diagnostics -ne 1 -or $identity.capacity -ne 64) {
    throw 'Requires a verified, just-flashed diagnostic_fault ON/64 capture.'
}
$firmware = Join-Path $capture ("firmware_" + $identity.tag + '.elf')
$firmwareHash = (Get-FileHash -LiteralPath $firmware -Algorithm SHA256).Hash.ToLowerInvariant()
if ($firmwareHash -ne $identity.built_elf_sha256) { throw 'Firmware identity mismatch.' }
$out = Join-Path $PSScriptRoot ('fault_debug/' + (Get-Date).ToString('yyyyMMdd_HHmmss_ffff'))
New-Item -ItemType Directory -Path $out | Out-Null
$pluginRoot = 'E:\ST\STM32CubeIDE_1.13.2\STM32CubeIDE\plugins'
$server = Join-Path $pluginRoot ('com.st.stm32cube.ide.mcu.externaltools.' +
    'stlink-gdb-server.win32_2.1.100.202310302101\tools\bin\ST-LINK_gdbserver.exe')
$programmer = Join-Path $pluginRoot ('com.st.stm32cube.ide.mcu.externaltools.' +
    'cubeprogrammer.win32_2.1.100.202311100844\tools\bin')
$debugger = Join-Path $pluginRoot ('com.st.stm32cube.ide.mcu.externaltools.' +
    'gnu-tools-for-stm32.11.3.rel1.win32_1.1.100.202309141235\tools\bin\arm-none-eabi-gdb.exe')
$script = Join-Path $PSScriptRoot 'fault_snapshot.gdb'
$arguments = @('-d','-g','-i','066DFF555775514867092907','-p','42123','-l','31',
    '-v','-cp',$programmer,'--frequency','4000')
$metadata = @{
    capture = $capture; capture_identity_sha256 =
        (Get-FileHash -LiteralPath $identityPath -Algorithm SHA256).Hash.ToLowerInvariant()
    firmware_sha256 = $firmwareHash; attach_only = $true; server_arguments = $arguments
    debugger_arguments = @('--nh','-nx','-q','-batch','-x',$script,$firmware)
    script_sha256 = (Get-FileHash -LiteralPath $script -Algorithm SHA256).Hash.ToLowerInvariant()
    started = (Get-Date).ToString('o')
}
$process = $null
try {
    $process = Start-Process -FilePath $server -ArgumentList $arguments -WindowStyle Hidden `
        -PassThru -RedirectStandardOutput (Join-Path $out 'server.txt') `
        -RedirectStandardError (Join-Path $out 'server_error.txt')
    Start-Sleep -Milliseconds 1200
    & $debugger @($metadata.debugger_arguments) 2>&1 |
        Set-Content -LiteralPath (Join-Path $out 'snapshot.txt')
    $metadata.debugger_exit = $LASTEXITCODE
    if ($LASTEXITCODE -ne 0) { throw 'GDB snapshot failed.' }
} finally {
    if ($null -ne $process -and -not $process.HasExited) { Stop-Process -Id $process.Id }
    $metadata.finished = (Get-Date).ToString('o')
    $metadata | ConvertTo-Json -Depth 6 |
        Set-Content -LiteralPath (Join-Path $out 'identity.json')
}
Write-Host "Fault SRAM evidence: $out"
