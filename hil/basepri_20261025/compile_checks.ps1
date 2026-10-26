$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$compiler = 'E:\ST\STM32CubeIDE_1.13.2\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.11.3.rel1.win32_1.1.100.202309141235\tools\bin\arm-none-eabi-gcc.exe'
$outputRoot = Join-Path $projectRoot 'Debug/basepri_compile_checks'
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
$includeDirectories = @('RTOS/Core/Inc','Core/Inc','Drivers/STM32F4xx_HAL_Driver/Inc',
    'Drivers/STM32F4xx_HAL_Driver/Inc/Legacy','Drivers/CMSIS/Device/ST/STM32F4xx/Include',
    'Drivers/CMSIS/Include','RTOS/lib/third/CherryUSB/core','RTOS/lib/third/CherryUSB/common',
    'RTOS/lib/third/CherryUSB/class/cdc','RTOS/lib/third/CherryUSB/class/hid',
    'RTOS/lib/third/CherryUSB/class/msc','RTOS/lib/third/CherryUSB/port/dwc2')
$includeArguments = @($includeDirectories | ForEach-Object { '-I' + (Join-Path $projectRoot $_) })
$sources = @('RTOS/Core/Src/rtos_heap.c','RTOS/Core/Src/rtos_perf.c',
    'RTOS/Core/Src/rtos_usb.c','RTOS/Port/GCC/ARM_CM7/port.c')
$results = New-Object System.Collections.Generic.List[object]
foreach ($mask in @(0,1)) {
    foreach ($assertions in @(0,1)) {
        foreach ($optimization in @('O0','O2','O3')) {
            foreach ($source in $sources) {
                $tag = "m${mask}_a${assertions}_${optimization}_$([System.IO.Path]::GetFileNameWithoutExtension($source))"
                $arguments = @('-mcpu=cortex-m4','-mthumb','-mfpu=fpv4-sp-d16','-mfloat-abi=hard',
                    '-std=gnu11','-Wall','-Wextra','-Werror',"-$optimization",'-DSTM32F446xx',
                    '-DUSE_HAL_DRIVER',"-DRTOS_CONFIG_USE_BASEPRI=$mask",
                    "-DRTOS_CONFIG_ASSERT_ENABLED=$assertions") + $includeArguments +
                    @('-c',(Join-Path $projectRoot $source),'-o',(Join-Path $outputRoot "$tag.o"))
                $output = & $compiler @arguments 2>&1
                $code = $LASTEXITCODE
                $output | Out-File -LiteralPath (Join-Path $PSScriptRoot "compile_$tag.txt") -Encoding utf8
                if ($code -ne 0) { throw "Strict compilation failed: $tag : $output" }
                $results.Add([ordered]@{tag=$tag; source=$source; passed=$true; warnings=0})
            }
            Write-Host "Strict compile PASS mask=$mask assertions=$assertions optimization=$optimization"
        }
    }
}
$arguments = @('-mcpu=cortex-m7','-mthumb','-mfpu=fpv5-d16','-mfloat-abi=hard','-std=gnu11',
    '-Wall','-Wextra','-Werror','-O2','-DSTM32F446xx','-DUSE_HAL_DRIVER') + $includeArguments +
    @('-c',(Join-Path $projectRoot $sources[3]),'-o',(Join-Path $outputRoot 'cm7_port_compile_only.o'))
$output = & $compiler @arguments 2>&1
$code = $LASTEXITCODE
$output | Out-File -LiteralPath (Join-Path $PSScriptRoot 'compile_cm7_port_only.txt') -Encoding utf8
if ($code -ne 0) { throw "Cortex-M7 port compilation failed: $output" }
$results.Add([ordered]@{tag='cm7_port_compile_only'; passed=$true; warnings=0; hardware_test=$false})
$results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'compile_results.json') -Encoding utf8
Write-Host "Verified $($results.Count) strict compilations; Cortex-M7 is compile-only"
