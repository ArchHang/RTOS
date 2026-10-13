param(
    [ValidatePattern('^[a-z0-9_]+$')]
    [string] $Label = ('configs_' + [DateTime]::UtcNow.ToString('yyyyMMdd_HHmmss_fff')),
    [switch] $PrepareOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Compile-only snapshots: no production files, Debug objects, firmware, probes,
# serial ports or board state are modified. An existing run is never replaced.
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$runRoot = Join-Path $PSScriptRoot ('config_matrix_' + $Label)
$inputRoot = Join-Path $runRoot 'Input'
$inputHeaders = Join-Path $inputRoot 'Inc'
$inputSources = Join-Path $inputRoot 'Src'
$toolRoot = 'E:\ST\STM32CubeIDE_1.13.2\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.11.3.rel1.win32_1.1.100.202309141235\tools\bin'
$gcc = Join-Path $toolRoot 'arm-none-eabi-gcc.exe'
$objdump = Join-Path $toolRoot 'arm-none-eabi-objdump.exe'
$utf8 = [Text.UTF8Encoding]::new($false)

function ConvertTo-NativeQuotedArgument {
    param([string] $Value)
    # Windows CRT argv quoting. These strings go directly to the executable,
    # never through cmd.exe or a shell.
    $quoted = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    $quoted = [regex]::Replace($quoted, '(\\+)$', '$1$1')
    return '"' + $quoted + '"'
}

function Invoke-ConfigTool {
    param([string] $FileName, [string[]] $ToolArguments, [string] $WorkingDirectory)
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $FileName
    $startInfo.Arguments = ($ToolArguments | ForEach-Object {
        ConvertTo-NativeQuotedArgument -Value $_
    }) -join ' '
    $startInfo.WorkingDirectory = $WorkingDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = $null
    try {
        $process = [Diagnostics.Process]::Start($startInfo)
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        return [pscustomobject]@{
            started = $true
            exitCode = $process.ExitCode
            stdout = $stdout.Result
            stderr = $stderr.Result
            launchError = $null
        }
    } catch {
        return [pscustomobject]@{
            started = $false
            exitCode = $null
            stdout = ''
            stderr = ''
            launchError = $_.Exception.Message
        }
    } finally {
        if ($null -ne $process) {
            $process.Dispose()
        }
    }
}

function Set-ConfigMacro {
    param([string] $Text, [string] $Name, [int] $Value)
    $pattern = '(?m)^[ \t]*#define[ \t]+' + [regex]::Escape($Name) +
        '[ \t]+\(([01])\)[ \t]*(?=\r?$)'
    if ([regex]::Matches($Text, $pattern).Count -ne 1) {
        throw ('Expected exactly one literal 0/1 definition of ' + $Name)
    }
    return [regex]::Replace($Text, $pattern, ('#define ' + $Name + ' (' + $Value + ')'))
}

function Get-StatsReturnEvidence {
    param([string] $Disassembly)
    $pattern = '(?ms)^Disassembly of section \.text\.rtos_sched_commit_context_switch:' +
        '(?<body>.*?)(?=^Disassembly of section |\z)'
    $match = [regex]::Match($Disassembly, $pattern)
    if (-not $match.Success) {
        return [pscustomobject]@{
            status = 'manual_review_required'
            reason = 'Commit function section was not found in scheduler object disassembly'
            preservedRegister = $null
            functionDisassembly = ''
        }
    }
    $body = $match.Groups['body'].Value
    $lines = @($body -split '\r?\n')
    $hookLine = -1
    for ($index = 0; $index -lt $lines.Count; $index++) {
        if ($lines[$index] -match 'R_ARM_THM_CALL\s+rtos_perf_on_context_switch\s*$') {
            if ($hookLine -ne -1) {
                return [pscustomobject]@{
                    status = 'manual_review_required'
                    reason = 'More than one performance-hook relocation needs control-flow review'
                    preservedRegister = $null
                    functionDisassembly = $body
                }
            }
            $hookLine = $index
        }
    }
    if ($hookLine -lt 1) {
        return [pscustomobject]@{
            status = 'failed'
            reason = 'PERF_HOTPATH_STATS=1 commit function has no performance-hook call'
            preservedRegister = $null
            functionDisassembly = $body
        }
    }

    # Certify only the simple emitted ABI sequence: next is moved from a
    # callee-saved register into r1 before BL, then that same
    # preserved register is moved into r0 immediately after BL. Do not claim
    # a pass for other valid but unanalysed instruction/control-flow patterns.
    $before = @($lines[0..($hookLine - 2)] | Where-Object {
        $_ -match '^\s*[0-9a-f]+:\s+[0-9a-f]'
    })
    $after = @($lines[($hookLine + 1)..($lines.Count - 1)] | Where-Object {
        $_ -match '^\s*[0-9a-f]+:\s+[0-9a-f]'
    })
    if (($before.Count -gt 0) -and ($after.Count -gt 0)) {
        $argument = [regex]::Match('', 'never_match')
        $argumentIndex = -1
        for ($index = $before.Count - 1; $index -ge 0; $index--) {
            # Do not cross an earlier call or control-flow instruction. A mov
            # into r0 between argument setup and BL is harmless to r1.
            if ($before[$index] -match '\s+(?:bl|blx|b(?:\.[nw])?|b(?:eq|ne|hi|ls|cc|cs|ge|gt|le|lt)|cbz|cbnz)\s') {
                break
            }
            if ($before[$index] -match '\s+[a-z][a-z0-9.]*\s+r1(?:,|\s|$)') {
                $argument = [regex]::Match($before[$index],
                    '\s+mov(?:\.w)?\s+r1,\s*(r[4-9]|r10|r11)\s*$')
                $argumentIndex = $index
                break
            }
        }
        if ($argument.Success) {
            $register = $argument.Groups[1].Value
            $returnPattern = '\s+mov(?:\.w)?\s+r0,\s*' + $register + '\s*$'
            $preserved = $true
            for ($index = $argumentIndex + 1; $index -lt $before.Count; $index++) {
                if ($before[$index] -notmatch '\s+mov(?:s|\.w)?\s+r0,') {
                    $preserved = $false
                }
            }
            $directReturn = $false
            if ($after.Count -ge 2) {
                $directReturn = ($after[1] -match '\s+bx\s+lr\s*$') -or
                    (($after[1] -match '\s+(?:pop|ldmia(?:\.w)?\s+sp!,)\s*\{[^}]*\bpc\}') -and
                    ($after[1] -notmatch '(?:\{|,)\s*r0\b'))
            }
            if ($preserved -and $directReturn -and ($after[0] -match $returnPattern)) {
                return [pscustomobject]@{
                    status = 'confirmed'
                    reason = 'Hook arg next and returned r0 use the same AAPCS callee-saved register'
                    preservedRegister = $register
                    functionDisassembly = $body
                }
            }
        }
    }
    return [pscustomobject]@{
        status = 'manual_review_required'
        reason = 'Review saved next value and r0 after the hook; simple ABI pattern did not match'
        preservedRegister = $null
        functionDisassembly = $body
    }
}

if (Test-Path -LiteralPath $runRoot) {
    throw ('Run directory already exists; choose a fresh -Label: ' + $runRoot)
}
foreach ($directory in @($runRoot, $inputRoot, $inputHeaders, $inputSources)) {
    New-Item -ItemType Directory -Path $directory | Out-Null
}

$report = [ordered]@{
    label = $Label
    startedUtc = [DateTime]::UtcNow.ToString('o')
    finishedUtc = $null
    status = 'preparing'
    compileOnly = $true
    prepareOnly = [bool]$PrepareOnly
    projectRoot = $projectRoot
    runRoot = $runRoot
    compiler = $gcc
    objdump = $objdump
    expectedConfigurations = 7
    expectedUnitsPerConfiguration = 13
    nativeLaunchFailure = $null
    inputs = @()
    configurations = @()
    statsReturnEvidence = $null
}
$failureCount = 0
$terminalError = $null

try {
    $productionHeaders = @(Get-ChildItem -LiteralPath (Join-Path $projectRoot 'RTOS\Core\Inc') `
        -Filter '*.h' -File | Sort-Object Name)
    $productionSources = @(Get-ChildItem -LiteralPath (Join-Path $projectRoot 'RTOS\Core\Src') `
        -Filter '*.c' -File | Sort-Object Name)
    $productionSources += Get-Item -LiteralPath (Join-Path $projectRoot 'RTOS\Port\GCC\ARM_CM7\port.c')
    if ($productionSources.Count -ne 13) {
        throw ('Expected 13 RTOS translation units, found ' + $productionSources.Count)
    }
    foreach ($inputFile in @($productionHeaders) + @($productionSources)) {
        $destination = if ($inputFile.Extension -eq '.h') { $inputHeaders } else { $inputSources }
        $copiedPath = Join-Path $destination $inputFile.Name
        Copy-Item -LiteralPath $inputFile.FullName -Destination $copiedPath
        $report.inputs += [pscustomobject]@{
            original = $inputFile.FullName
            snapshot = $copiedPath
            sha256 = (Get-FileHash -LiteralPath $copiedPath -Algorithm SHA256).Hash
        }
    }

    $defaultValues = [ordered]@{
        RTOS_CONFIG_PERF_HOTPATH_STATS = 0
        RTOS_CONFIG_USE_BASEPRI = 1
        RTOS_CONFIG_USE_PREEMPTION = 1
        RTOS_CONFIG_ASSERT_ENABLED = 1
    }
    $originalConfig = [IO.File]::ReadAllText((Join-Path $inputHeaders 'rtos_config.h'))
    foreach ($name in $defaultValues.Keys) {
        $valuePattern = '(?m)^#define[ \t]+' + $name + '[ \t]+\((' + $defaultValues[$name] + ')\)[ \t]*(?=\r?$)'
        if ([regex]::Matches($originalConfig, $valuePattern).Count -ne 1) {
            throw ('Production default is not the expected literal definition: ' + $name)
        }
    }
    $cases = @(
        [pscustomobject]@{ name = 'default_o2'; optimization = '-O2'; macro = $null; value = 0 },
        [pscustomobject]@{ name = 'perf_stats_o2'; optimization = '-O2'; macro = 'RTOS_CONFIG_PERF_HOTPATH_STATS'; value = 1 },
        [pscustomobject]@{ name = 'primask_o2'; optimization = '-O2'; macro = 'RTOS_CONFIG_USE_BASEPRI'; value = 0 },
        [pscustomobject]@{ name = 'cooperative_o2'; optimization = '-O2'; macro = 'RTOS_CONFIG_USE_PREEMPTION'; value = 0 },
        [pscustomobject]@{ name = 'assert_off_o2'; optimization = '-O2'; macro = 'RTOS_CONFIG_ASSERT_ENABLED'; value = 0 },
        [pscustomobject]@{ name = 'default_o0'; optimization = '-O0'; macro = $null; value = 0 },
        [pscustomobject]@{ name = 'default_o3'; optimization = '-O3'; macro = $null; value = 0 }
    )
    $sharedIncludeDirs = @(
        'Core\Inc', 'Drivers\STM32F4xx_HAL_Driver\Inc',
        'Drivers\STM32F4xx_HAL_Driver\Inc\Legacy',
        'Drivers\CMSIS\Device\ST\STM32F4xx\Include', 'Drivers\CMSIS\Include',
        'RTOS\lib\third\CherryUSB\core', 'RTOS\lib\third\CherryUSB\common',
        'RTOS\lib\third\CherryUSB\class\cdc', 'RTOS\lib\third\CherryUSB\class\msc',
        'RTOS\lib\third\CherryUSB\class\hid', 'RTOS\lib\third\CherryUSB\port\dwc2'
    ) | ForEach-Object { Join-Path $projectRoot $_ }
    $sources = @(Get-ChildItem -LiteralPath $inputSources -Filter '*.c' -File | Sort-Object Name)

    foreach ($case in $cases) {
        $caseRoot = Join-Path $runRoot $case.name
        $headers = Join-Path $caseRoot 'Inc'
        $objects = Join-Path $caseRoot 'Obj'
        foreach ($directory in @($caseRoot, $headers, $objects)) {
            New-Item -ItemType Directory -Path $directory | Out-Null
        }
        Get-ChildItem -LiteralPath $inputHeaders -Filter '*.h' -File |
            Copy-Item -Destination $headers
        $values = [ordered]@{}
        $configText = $originalConfig
        foreach ($name in $defaultValues.Keys) {
            $values[$name] = $defaultValues[$name]
            if ($name -eq $case.macro) { $values[$name] = $case.value }
            $configText = Set-ConfigMacro -Text $configText -Name $name -Value $values[$name]
        }
        $configPath = Join-Path $headers 'rtos_config.h'
        [IO.File]::WriteAllText($configPath, $configText, $utf8)
        $commonArguments = @('-mcpu=cortex-m4', '-std=gnu11', $case.optimization, '-g3',
            '-DDEBUG', '-DUSE_HAL_DRIVER', '-DSTM32F446xx', '-ffunction-sections',
            '-fdata-sections', '-Wall', '-Wextra', '-Werror', '--specs=nano.specs',
            '-mfpu=fpv4-sp-d16', '-mfloat-abi=hard', '-mthumb', ('-I' + $headers))
        foreach ($directory in $sharedIncludeDirs) { $commonArguments += '-I' + $directory }
        $entry = [ordered]@{
            name = $case.name
            optimization = $case.optimization
            effectiveMacros = $values
            configSha256 = (Get-FileHash -LiteralPath $configPath -Algorithm SHA256).Hash
            expectedUnits = 13
            status = 'in_progress'
            units = @()
            disassembly = @()
        }
        $report.configurations += $entry
        $caseFailures = 0
        foreach ($source in $sources) {
            $objectPath = Join-Path $objects ($source.BaseName + '.o')
            $arguments = $commonArguments + @('-c', $source.FullName, '-o', $objectPath)
            $command = (ConvertTo-NativeQuotedArgument -Value $gcc) + ' ' +
                (($arguments | ForEach-Object { ConvertTo-NativeQuotedArgument -Value $_ }) -join ' ')
            $result = if ($PrepareOnly) {
                [pscustomobject]@{ started = $false; exitCode = $null; stdout = ''; stderr = ''; launchError = $null }
            } else {
                Invoke-ConfigTool -FileName $gcc -ToolArguments $arguments -WorkingDirectory $caseRoot
            }
            $output = $result.stdout + $result.stderr
            $warnings = @($output -split '\r?\n' | Where-Object { $_ -match 'warning:|\[-Werror(?:=|\])' })
            $errors = @($output -split '\r?\n' | Where-Object { $_ -match 'error:|fatal error:' })
            $logPath = Join-Path $caseRoot ($source.BaseName + '.compile.log')
            [IO.File]::WriteAllText($logPath, ($command + [Environment]::NewLine +
                'exit=' + $result.exitCode + [Environment]::NewLine + $output +
                $result.launchError), $utf8)
            $entry.units += [pscustomobject]@{
                source = $source.FullName
                sha256 = (Get-FileHash -LiteralPath $source.FullName -Algorithm SHA256).Hash
                executable = $gcc
                arguments = $arguments
                command = $command
                status = if ($PrepareOnly) { 'planned' } elseif (-not $result.started) {
                    'launch_failed'
                } elseif ($result.exitCode -eq 0) { 'passed' } else { 'failed' }
                exitCode = $result.exitCode
                warnings = $warnings
                errors = $errors
                stdout = $result.stdout
                stderr = $result.stderr
                launchError = $result.launchError
                log = $logPath
                object = $objectPath
            }
            if (-not $PrepareOnly -and -not $result.started) {
                $report.nativeLaunchFailure = $result.launchError
                throw ('Native compiler launch failed; stopped without retry: ' + $result.launchError)
            }
            if (-not $PrepareOnly -and $result.exitCode -ne 0) {
                $caseFailures++
                $failureCount++
            }
        }
        $entry.status = if ($PrepareOnly) { 'prepared' } elseif ($caseFailures -eq 0) { 'passed' } else { 'failed' }
        if (-not $PrepareOnly -and $caseFailures -eq 0) {
            foreach ($unit in @('rtos_sched', 'port')) {
                $arguments = @('-dr', (Join-Path $objects ($unit + '.o')))
                $result = Invoke-ConfigTool -FileName $objdump -ToolArguments $arguments -WorkingDirectory $caseRoot
                $outputPath = Join-Path $caseRoot ($unit + '.disassembly.txt')
                [IO.File]::WriteAllText($outputPath, ($result.stdout + $result.stderr + $result.launchError), $utf8)
                $entry.disassembly += [pscustomobject]@{
                    executable = $objdump
                    arguments = $arguments
                    exitCode = $result.exitCode
                    launchError = $result.launchError
                    path = $outputPath
                }
                if (-not $result.started) {
                    $report.nativeLaunchFailure = $result.launchError
                    throw ('Native objdump launch failed; stopped without retry: ' + $result.launchError)
                }
                if ($result.exitCode -ne 0) {
                    $failureCount++
                    $entry.status = 'disassembly_failed'
                } elseif (($case.name -eq 'perf_stats_o2') -and ($unit -eq 'rtos_sched')) {
                    $report.statsReturnEvidence = Get-StatsReturnEvidence -Disassembly $result.stdout
                    if ($report.statsReturnEvidence.status -eq 'failed') { $failureCount++ }
                }
            }
        }
        [IO.File]::WriteAllText((Join-Path $caseRoot 'manifest.json'),
            (ConvertTo-Json -InputObject $entry -Depth 12), $utf8)
        Write-Output ($case.name + ': ' + $entry.status + '; units=' + $entry.units.Count)
    }
    if ($PrepareOnly) {
        $report.status = 'prepared_no_native_tools_run'
    } elseif ($failureCount -gt 0) {
        $report.status = 'failed'
    } elseif (($null -eq $report.statsReturnEvidence) -or
        ($report.statsReturnEvidence.status -ne 'confirmed')) {
        $report.status = 'compiled_manual_asm_review_required'
    } else {
        $report.status = 'passed'
    }
} catch {
    $report.status = 'aborted'
    $terminalError = $_.Exception.Message
} finally {
    $report.finishedUtc = [DateTime]::UtcNow.ToString('o')
    $report['compileFailures'] = $failureCount
    $report['terminalError'] = $terminalError
    [IO.File]::WriteAllText((Join-Path $runRoot 'matrix.json'),
        (ConvertTo-Json -InputObject $report -Depth 15), $utf8)
}

Write-Output ('CONFIG_MATRIX|' + $report.status + '|json=' + (Join-Path $runRoot 'matrix.json'))
if ($null -ne $terminalError) { throw $terminalError }
if ($failureCount -gt 0) { throw ('Configuration matrix has ' + $failureCount + ' failures; see matrix.json') }
