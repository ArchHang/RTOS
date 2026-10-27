param([string] $OutDir = '')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$evidenceRoot = if ($OutDir -eq '') {
    Join-Path $PSScriptRoot ('compile_' + (Get-Date).ToString('yyyyMMdd_HHmmss'))
} else { [IO.Path]::GetFullPath($OutDir) }
if (Test-Path -LiteralPath $evidenceRoot) { throw "Output already exists: $evidenceRoot" }
$objectRoot = Join-Path $projectRoot 'Debug/refactor_compile_checks'
New-Item -ItemType Directory -Path $evidenceRoot -Force | Out-Null
New-Item -ItemType Directory -Path $objectRoot -Force | Out-Null
$compiler = 'E:\ST\STM32CubeIDE_1.13.2\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.11.3.rel1.win32_1.1.100.202309141235\tools\bin\arm-none-eabi-gcc.exe'
$includes = @('RTOS/Core/Inc','RTOS/Core/Private','Core/Inc',
    'Drivers/STM32F4xx_HAL_Driver/Inc','Drivers/STM32F4xx_HAL_Driver/Inc/Legacy',
    'Drivers/CMSIS/Device/ST/STM32F4xx/Include','Drivers/CMSIS/Include',
    'RTOS/lib/third/CherryUSB/core','RTOS/lib/third/CherryUSB/common',
    'RTOS/lib/third/CherryUSB/class/cdc','RTOS/lib/third/CherryUSB/class/hid',
    'RTOS/lib/third/CherryUSB/class/msc','RTOS/lib/third/CherryUSB/port/dwc2')
$includeArguments = @($includes | ForEach-Object { '-I' + (Join-Path $projectRoot $_) })
$sources = @(foreach ($directory in @('Core/Src','IPC','Memory','Services','USB',
                                      'Port/GCC/ARM_CM4F_CM7F')) {
    Get-ChildItem -LiteralPath (Join-Path $projectRoot "RTOS/$directory") -Filter '*.c' -File |
        Sort-Object Name | ForEach-Object { $_.FullName.Substring($projectRoot.Length + 1).Replace('\','/') }
})
if ($sources.Count -ne 16) { throw "Unexpected source count: $($sources.Count)" }
$results = New-Object System.Collections.Generic.List[object]
$matrix = @(foreach ($mask in @(0,1)) {
    foreach ($assertions in @(0,1)) {
        foreach ($optimization in @('O0','O2','O3')) {
            [pscustomobject]@{mask=$mask; assertions=$assertions; optimization=$optimization}
        }
    }
})
foreach ($configuration in $matrix) {
    foreach ($source in $sources) {
        $tag = "m$($configuration.mask)_a$($configuration.assertions)_$($configuration.optimization)_$([IO.Path]::GetFileNameWithoutExtension($source))"
        $arguments = @('-mcpu=cortex-m4','-mthumb','-mfpu=fpv4-sp-d16',
            '-mfloat-abi=hard','-std=gnu11','-Wall','-Wextra','-Werror','-fstack-usage',
            "-$($configuration.optimization)",'-DSTM32F446xx','-DUSE_HAL_DRIVER',
            "-DRTOS_CONFIG_USE_BASEPRI=$($configuration.mask)",
            "-DRTOS_CONFIG_ASSERT_ENABLED=$($configuration.assertions)") +
            $includeArguments + @('-c',(Join-Path $projectRoot $source),
                '-o',(Join-Path $objectRoot "$tag.o"))
        $before = (Get-FileHash -LiteralPath (Join-Path $projectRoot $source)).Hash
        $output = & $compiler @arguments 2>&1
        $code = $LASTEXITCODE
        $output | Out-File -LiteralPath (Join-Path $evidenceRoot "compile_$tag.txt") -Encoding utf8
        if ($code -ne 0) { throw "Strict compilation failed: $tag : $output" }
        if ($before -ne (Get-FileHash -LiteralPath (Join-Path $projectRoot $source)).Hash) {
            throw "Source changed during compilation: $source"
        }
        Copy-Item -LiteralPath (Join-Path $objectRoot "$tag.su") -Destination $evidenceRoot
        $results.Add([ordered]@{tag=$tag; source=$source; passed=$true; warnings=0;
            sha256=$before; arguments=$arguments})
    }
    Write-Host "Strict compile PASS mask=$($configuration.mask) assertions=$($configuration.assertions) optimization=$($configuration.optimization)"
}
$port = 'RTOS/Port/GCC/ARM_CM4F_CM7F/port.c'
$arguments = @('-mcpu=cortex-m7','-mthumb','-mfpu=fpv5-d16','-mfloat-abi=hard',
    '-std=gnu11','-Wall','-Wextra','-Werror','-O2','-DSTM32F446xx','-DUSE_HAL_DRIVER') +
    $includeArguments + @('-c',(Join-Path $projectRoot $port),
        '-o',(Join-Path $objectRoot 'cm7_port_compile_only.o'))
$output = & $compiler @arguments 2>&1
$code = $LASTEXITCODE
$output | Out-File -LiteralPath (Join-Path $evidenceRoot 'compile_cm7_port_only.txt') -Encoding utf8
if ($code -ne 0) { throw "Cortex-M7 compile failed: $output" }
$results.Add([ordered]@{tag='cm7_port_compile_only'; passed=$true; warnings=0; hardware_test=$false})
ConvertTo-Json -InputObject @($results.ToArray()) -Depth 5 |
    Set-Content -LiteralPath (Join-Path $evidenceRoot 'compile_results.json') -Encoding utf8
Write-Host "Verified $($results.Count) strict compilations; Cortex-M7 is compile-only"
