$ErrorActionPreference = 'Stop'
$projectRoot = 'E:\STM32_Project\CubeIDE\f446_rtos'
$outputRoot = Join-Path $projectRoot 'hil\lowlevel_20261012\fixture_compile'
$summaryPath = Join-Path $outputRoot 'results.json'
if (Test-Path -LiteralPath $summaryPath) { throw 'Do not overwrite fixture compiler evidence' }
$matrix = Get-Content -LiteralPath (Join-Path $projectRoot `
    'hil\lowlevel_20261012\config_matrix_final\matrix.json') -Raw | ConvertFrom-Json
$reference = ($matrix.configurations | Where-Object name -eq 'default_o2').units[0]
$includeArguments = @($reference.arguments | Where-Object { $_ -like '-I*' })
$includeArguments += '-I' + (Join-Path $projectRoot 'hil\fw\Inc')
$objectRoot = Join-Path $outputRoot 'Obj'
New-Item -ItemType Directory -Path $objectRoot -Force | Out-Null
$entries = @(
    @{name='lowlevel_checks'; path='hil/fw/Src/lowlevel_checks.c'; defines=@('FIRMWARE_LOWLEVEL_CHECKS=1')},
    @{name='lowlevel_fpu_probe'; path='hil/fw/Port/ARM/lowlevel_fpu_probe.c'; defines=@('FIRMWARE_LOWLEVEL_CHECKS=1')},
    @{name='flash_diag'; path='hil/fw/Src/flash_diag.c'; defines=@('FIRMWARE_FLASH_DIAG=1')},
    @{name='benchmark_lowlevel'; path='Core/Src/benchmark.c'; defines=@('FIRMWARE_PERF=1','FIRMWARE_LOWLEVEL_CHECKS=1')},
    @{name='benchmark_flash'; path='Core/Src/benchmark.c'; defines=@('FIRMWARE_FLASH_DIAG=1')}
)
$results = @()
foreach ($entry in $entries) {
    $source = Join-Path $projectRoot $entry.path
    $arguments = @('-mcpu=cortex-m4','-mthumb','-mfpu=fpv4-sp-d16','-mfloat-abi=hard',
        '-std=gnu11','-O2','-Wall','-Wextra','-Werror','-DSTM32F446xx','-DUSE_HAL_DRIVER')
    $arguments += @($entry.defines | ForEach-Object { '-D' + $_ })
    $arguments += $includeArguments
    $arguments += @('-c',$source,'-o',(Join-Path $objectRoot ($entry.name + '.o')))
    $ErrorActionPreference = 'Continue'
    $compilerOutput = & $reference.executable @arguments 2>&1
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    $compilerOutput | Out-File -LiteralPath (Join-Path $outputRoot ($entry.name + '.txt')) `
        -Encoding utf8
    if ($code -ne 0) {
        $compilerOutput | Write-Host
        throw "Fixture compilation failed: $($entry.name)"
    }
    $results += [ordered]@{name=$entry.name;source=$entry.path;
        sha256=(Get-FileHash -LiteralPath $source).Hash;exit_code=$code;
        warnings=@($compilerOutput | Select-String 'warning:').Count;arguments=$arguments}
    Write-Host "$($entry.name): passed with -Wall -Wextra -Werror"
}
$results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $summaryPath -Encoding utf8
