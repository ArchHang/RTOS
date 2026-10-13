param()
$ErrorActionPreference = 'Stop'

# Compile an isolated snapshot. The production config, Debug objects and HIL
# firmware are never edited by this compile-only configuration check.
$projectRoot = 'E:\STM32_Project\CubeIDE\f446_rtos'
$auditRoot = Join-Path $projectRoot 'hil\audit_20261012'
$snapshotRoot = Join-Path $auditRoot 'primask_compile'
$includeRoot = Join-Path $snapshotRoot 'Inc'
$sourceRoot = Join-Path $snapshotRoot 'Src'
$objectRoot = Join-Path $snapshotRoot 'Obj'
$toolRoot = 'E:\ST\STM32CubeIDE_1.13.2\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.11.3.rel1.win32_1.1.100.202309141235\tools\bin'
$gcc = Join-Path $toolRoot 'arm-none-eabi-gcc.exe'
$objdump = Join-Path $toolRoot 'arm-none-eabi-objdump.exe'
function Invoke-AuditTool {
    param([string]$FileName, [string[]]$ToolArguments)
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $FileName
    $startInfo.Arguments = ($ToolArguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = [System.Diagnostics.Process]::Start($startInfo)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    $result = [pscustomobject]@{ exitCode = $process.ExitCode; output = $stdout.Result + $stderr.Result }
    $process.Dispose()
    return $result
}
foreach ($dir in @($snapshotRoot, $includeRoot, $sourceRoot, $objectRoot)) {
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
}
Get-ChildItem -LiteralPath (Join-Path $projectRoot 'RTOS\Core\Inc') -Filter '*.h' -File |
    Copy-Item -Destination $includeRoot -Force
Get-ChildItem -LiteralPath (Join-Path $projectRoot 'RTOS\Core\Src') -Filter '*.c' -File |
    Copy-Item -Destination $sourceRoot -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'RTOS\Port\GCC\ARM_CM7\port.c') -Destination $sourceRoot -Force

$configPath = Join-Path $includeRoot 'rtos_config.h'
$configText = [System.IO.File]::ReadAllText($configPath)
$configPattern = '(?m)^#define\s+RTOS_CONFIG_USE_BASEPRI\s+\(1\)\s*\r?$'
if ([regex]::Matches($configText, $configPattern).Count -ne 1) {
    throw 'Expected exactly one production RTOS_CONFIG_USE_BASEPRI (1) definition'
}
$configText = [regex]::Replace($configText, $configPattern, '#define RTOS_CONFIG_USE_BASEPRI (0)')
[System.IO.File]::WriteAllText($configPath, $configText, [System.Text.UTF8Encoding]::new($false))

$includeDirs = @(
    $includeRoot,
    (Join-Path $projectRoot 'Core\Inc'),
    (Join-Path $projectRoot 'Drivers\STM32F4xx_HAL_Driver\Inc'),
    (Join-Path $projectRoot 'Drivers\STM32F4xx_HAL_Driver\Inc\Legacy'),
    (Join-Path $projectRoot 'Drivers\CMSIS\Device\ST\STM32F4xx\Include'),
    (Join-Path $projectRoot 'Drivers\CMSIS\Include'),
    (Join-Path $projectRoot 'RTOS\lib\third\CherryUSB\core'),
    (Join-Path $projectRoot 'RTOS\lib\third\CherryUSB\common'),
    (Join-Path $projectRoot 'RTOS\lib\third\CherryUSB\class\cdc'),
    (Join-Path $projectRoot 'RTOS\lib\third\CherryUSB\class\msc'),
    (Join-Path $projectRoot 'RTOS\lib\third\CherryUSB\class\hid'),
    (Join-Path $projectRoot 'RTOS\lib\third\CherryUSB\port\dwc2')
)
$commonArgs = @('-mcpu=cortex-m4', '-std=gnu11', '-O2', '-g3', '-DDEBUG',
    '-DUSE_HAL_DRIVER', '-DSTM32F446xx', '-ffunction-sections', '-fdata-sections',
    '-Wall', '--specs=nano.specs', '-mfpu=fpv4-sp-d16', '-mfloat-abi=hard', '-mthumb')
foreach ($dir in $includeDirs) { $commonArgs += '-I' + $dir }

$log = New-Object System.Collections.Generic.List[string]
$log.Add('Independent ARM compile-only check: RTOS_CONFIG_USE_BASEPRI=0')
$log.Add('Snapshot UTC: ' + [DateTime]::UtcNow.ToString('o'))
$log.Add('Compiler: ' + $gcc)
$log.Add('RTOS headers: isolated Inc copy; original RTOS/Core/Inc is absent from include paths')
$manifest = @()
$failures = 0
foreach ($source in (Get-ChildItem -LiteralPath $sourceRoot -Filter '*.c' -File | Sort-Object Name)) {
    $objectPath = Join-Path $objectRoot ($source.BaseName + '.o')
    $argsForSource = $commonArgs + @('-c', $source.FullName, '-o', $objectPath)
    $compilerResult = Invoke-AuditTool -FileName $gcc -ToolArguments $argsForSource
    $compilerOutput = $compilerResult.output
    $compilerExit = $compilerResult.exitCode
    $log.Add($source.Name + ': exit=' + $compilerExit)
    if ($compilerOutput) { $log.Add(($compilerOutput | Out-String).TrimEnd()) }
    if ($compilerExit -ne 0) { $failures++ }
    $manifest += [pscustomobject]@{
        source = $source.Name
        sha256 = (Get-FileHash -LiteralPath $source.FullName -Algorithm SHA256).Hash
        compilerArguments = $argsForSource
        exitCode = $compilerExit
    }
}
$log.Add('Translation units: ' + $manifest.Count + '; failures: ' + $failures)
$logPath = Join-Path $snapshotRoot 'compile.log'
[System.IO.File]::WriteAllLines($logPath, $log)
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $snapshotRoot 'manifest.json') -Encoding UTF8
if ($failures -ne 0) {
    Get-Content -LiteralPath $logPath
    throw ('PRIMASK compilation failed for ' + $failures + ' translation units')
}

$portDisassembly = (Invoke-AuditTool -FileName $objdump -ToolArguments @('-dr', (Join-Path $objectRoot 'port.o'))).output
$portDisassembly | Set-Content -LiteralPath (Join-Path $snapshotRoot 'port.disassembly.txt') -Encoding UTF8
$schedulerDisassembly = (Invoke-AuditTool -FileName $objdump -ToolArguments @('-dr', (Join-Path $objectRoot 'rtos_sched.o'))).output
$schedulerDisassembly | Set-Content -LiteralPath (Join-Path $snapshotRoot 'sched.disassembly.txt') -Encoding UTF8
Get-Content -LiteralPath $logPath
