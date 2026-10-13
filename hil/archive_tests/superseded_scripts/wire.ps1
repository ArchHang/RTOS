# Wire the system-level scenario suite into a project's Debug build.
# Usage: powershell -ExecutionPolicy Bypass -File wire.ps1 -Project rtos|freertos [-Scenario N]
param(
    [Parameter(Mandatory = $true)][ValidateSet('rtos', 'freertos')][string] $Project,
    [int] $Scenario = 1
)
$ErrorActionPreference = 'Stop'

$ROOT = if ($Project -eq 'rtos') { 'E:\STM32_Project\CubeIDE\f446_rtos' } else { 'E:\STM32_Project\CubeIDE\FreeRTOS' }
$MK = Join-Path $ROOT 'Debug\Core\Src\subdir.mk'
$OL = Join-Path $ROOT 'Debug\objects.list'

# 本次要并入 Core/Src 的新源文件
if ($Project -eq 'rtos') {
    $newSources = @('kern_port_rtos.c', 'test_framework.c', 'test_scenarios.c')
} else {
    $newSources = @('kern_port_freertos.c', 'test_framework.c', 'test_scenarios.c')
}

# ---- 1. subdir.mk: 在 selftest 之后插入新源与其对象/依赖 ----
$lines = [System.IO.File]::ReadAllLines($MK)
$out = New-Object System.Collections.Generic.List[string]
$inSrcs = $false
$inObjs = $false
$inDeps = $false
foreach ($line in $lines) {
    $out.Add($line)
    if ($line -match '^C_SRCS') { $inSrcs = $true; continue }
    if ($line -match '^OBJS') { $inSrcs = $false; $inObjs = $true; continue }
    if ($line -match '^C_DEPS') { $inObjs = $false; $inDeps = $true; continue }
    if ($line.Trim() -eq '') { $inSrcs = $false; $inObjs = $false; $inDeps = $false; continue }

    if ($inSrcs -and $line -match 'selftest\.c') {
        foreach ($s in $newSources) { $out.Add("../Core/Src/$s \") }
    }
    if ($inObjs -and $line -match 'selftest\.o') {
        foreach ($s in $newSources) { $out.Add("./Core/Src/$($s -replace '\.c$', '.o') \") }
    }
    if ($inDeps -and $line -match 'selftest\.d') {
        foreach ($s in $newSources) { $out.Add("./Core/Src/$($s -replace '\.c$', '.d') \") }
    }
}
$text = ($out -join "`r`n") + "`r`n"

# ---- 2. 注入场景宏(替换既有值或首次插入) ----
$text = [regex]::Replace($text, '-DTEST_SCENARIO_SELECT=\d+', "-DTEST_SCENARIO_SELECT=$Scenario")
if ($text -notmatch 'TEST_SCENARIO_SELECT') {
    $text = $text -replace '(-c -DDEBUG -DUSE_HAL_DRIVER -DSTM32F446xx )', "`$1-DTEST_SCENARIO_SELECT=$Scenario "
}
[System.IO.File]::WriteAllText($MK, $text)

# ---- 3. objects.list: 在 selftest.o 之后插入新对象 ----
$olLines = Get-Content $OL
$olOut = New-Object System.Collections.Generic.List[string]
foreach ($l in $olLines) {
    $olOut.Add($l)
    if ($l -match 'selftest\.o') {
        foreach ($s in $newSources) { $olOut.Add("./Core/Src/$($s -replace '\.c$', '.o')") }
    }
}
$seen = @{}
$final = New-Object System.Collections.Generic.List[string]
foreach ($l in $olOut) { if (-not $seen.ContainsKey($l)) { $seen[$l] = $true; $final.Add($l) } }
Set-Content $OL -Value $final

"wired $Project (scenario=$Scenario)"
"-- subdir.mk 新增源 --"
(Get-Content $MK) | Select-String 'kern_port|test_framework|test_scenarios' | Select-Object -First 9 | ForEach-Object { "   $($_.Line.Trim())" }
"-- objects.list 新增对象 --"
(Get-Content $OL) | Select-String 'kern_port|test_framework|test_scenarios' | ForEach-Object { "   $_" }
"-- 宏 --"
if ((Get-Content $MK) -match 'TEST_SCENARIO_SELECT=\d+') { "   OK: $($Matches[0])" } else { "   MISSING" }
