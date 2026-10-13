$ErrorActionPreference = 'Stop'
$projectRoot = 'E:\STM32_Project\CubeIDE\f446_rtos'
$outputRoot = Join-Path $projectRoot 'hil\production_20261012\strict_compile'
$resultPath = Join-Path $outputRoot 'results.json'
if (Test-Path -LiteralPath $resultPath) { throw 'Preserve previous compilation evidence' }
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
$compiler = 'E:\ST\STM32CubeIDE_1.13.2\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.11.3.rel1.win32_1.1.100.202309141235\tools\bin\arm-none-eabi-gcc.exe'
$matrixPath = Join-Path $projectRoot 'hil\lowlevel_20261012\config_matrix_final\matrix.json'
$matrix = Get-Content -LiteralPath $matrixPath -Raw | ConvertFrom-Json
$includes = @(('-I' + (Join-Path $projectRoot 'hil/fw/Inc')),
    ('-I' + (Join-Path $projectRoot 'RTOS/Core/Inc')))
$includes += @($matrix.configurations[0].units[0].arguments |
    Where-Object { $_ -like '-I*' -and $_ -notmatch 'config_matrix_final' })
$common = @('-mcpu=cortex-m4','-mthumb','-mfpu=fpv4-sp-d16','-mfloat-abi=hard',
    '-std=gnu11','-Wall','-Wextra','-Werror','-DSTM32F446xx','-DUSE_HAL_DRIVER',
    '-DFIRMWARE_PRODUCTION_STRESS=1')
$results = @()
foreach ($optimization in @('O0','O2','O3')) {
    foreach ($name in @('production_stress','production_irq')) {
        $source = Join-Path $projectRoot "hil/fw/Src/$name.c"
        $arguments = $common + @("-$optimization") + $includes + @('-c',$source,
            '-o',(Join-Path $outputRoot "${name}_${optimization}.o"))
        $ErrorActionPreference = 'Continue'
        $output = & $compiler @arguments 2>&1
        $exitCode = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        $output | Out-File -LiteralPath (Join-Path $outputRoot "${name}_${optimization}.txt")
        $warnings = @($output | Select-String 'warning:').Count
        $results += [ordered]@{name=$name; optimization=$optimization;
            source_sha256=(Get-FileHash -LiteralPath $source).Hash; arguments=$arguments;
            exit_code=$exitCode; warnings=$warnings}
        if ($exitCode -ne 0 -or $warnings -ne 0) { throw "Strict compile failed: $name $optimization" }
    }
}
foreach ($name in @('production_stress','production_irq')) {
    $source = Join-Path $outputRoot "header_$name.c"
    [System.IO.File]::WriteAllText($source, "#include `"$name.h`"`r`n")
    $arguments = $common + @('-O2') + $includes + @('-c',$source,
        '-o',(Join-Path $outputRoot "header_$name.o"))
    $ErrorActionPreference = 'Continue'
    $output = & $compiler @arguments 2>&1
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    $output | Out-File -LiteralPath (Join-Path $outputRoot "header_$name.txt")
    $warnings = @($output | Select-String 'warning:').Count
    $results += [ordered]@{name="header_$name"; optimization='O2'; arguments=$arguments;
        exit_code=$exitCode; warnings=$warnings}
    if ($exitCode -ne 0 -or $warnings -ne 0) { throw "Header compile failed: $name" }
}
$results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $resultPath -Encoding utf8
Write-Host "Strict fixture compilation passed: $($results.Count) tasks, zero warnings"
