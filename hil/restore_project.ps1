# hil\restore_project.ps1
# Put the f446_rtos project back to its pre-HIL state.
#
#   * Debug\Core\Src\subdir.mk  <- pristine copy captured by build_fw.ps1
#   * Debug\objects.list        <- rebuilt from that same pristine source list
#   * Debug\hil\                <- removed (variant object files)
#   * hil\fwsrc\                <- removed (copied variant sources)
#   * Audit-only compile/link hooks are removed from Debug\makefile and RTOS rules
#   * Debug\sources.mk and main.c are not touched by restoration
#
# Usage: powershell -ExecutionPolicy Bypass -File hil\restore_project.ps1
param([switch] $Rebuild)
$ErrorActionPreference = 'Stop'
$ROOT  = 'E:\STM32_Project\CubeIDE\f446_rtos'
$DBG   = Join-Path $ROOT 'Debug'
$LOCAL = Join-Path $ROOT 'hil\fwsrc'
$tpl   = Join-Path $ROOT 'hil\templates\subdir.mk.template'
$TOOLS = 'E:\ST\STM32CubeIDE_1.13.2\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.11.3.rel1.win32_1.1.100.202309141235\tools\bin'
$MAKE  = 'E:\ST\STM32CubeIDE_1.13.2\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.make.win32_2.1.100.202310302056\tools\bin\make.exe'
$env:PATH = "$TOOLS;$env:PATH"

if (-not (Test-Path $tpl)) { throw "missing pristine template: $tpl" }

# 1. subdir.mk back to the pristine CubeIDE content
$mk = Join-Path $DBG 'Core\Src\subdir.mk'
if (Test-Path $mk) { Copy-Item $mk (Join-Path $ROOT 'hil\templates\subdir.mk.last_variant') -Force }
Copy-Item $tpl $mk -Force
Write-Host "restored $mk"

foreach ($rel in @('RTOS/Core/Src','RTOS/Port/GCC/ARM_CM7','RTOS/lib/third/CherryUSB/class/msc','RTOS/lib/third/CherryUSB/core','RTOS/lib/third/CherryUSB/port/dwc2')) {
    $ruleTemplate = Join-Path $ROOT ('hil/templates/' + ($rel -replace '/', '_') + '.mk')
    $rulePath = Join-Path $DBG "$rel/subdir.mk"
    if (Test-Path -LiteralPath $ruleTemplate) {
        Copy-Item -LiteralPath $ruleTemplate -Destination $rulePath -Force
        Get-ChildItem -LiteralPath (Join-Path $DBG $rel) -File | Where-Object { $_.Extension -in '.o','.d' } | Remove-Item -Force
    }
}
$linkTemplate = Join-Path $ROOT 'hil/templates/makefile.template'
if (Test-Path -LiteralPath $linkTemplate) { Copy-Item -LiteralPath $linkTemplate -Destination (Join-Path $DBG 'makefile') -Force }

# 2. objects.list back to the full canonical list, independent of existing .o files
$objectTemplate = Join-Path $ROOT 'hil/templates/objects.list.template'
$oldObjectTemplate = Join-Path $LOCAL 'objects_list.template'
if (-not (Test-Path -LiteralPath $objectTemplate) -and (Test-Path -LiteralPath $oldObjectTemplate)) {
    Copy-Item -LiteralPath $oldObjectTemplate -Destination $objectTemplate
}
if (Test-Path -LiteralPath $objectTemplate) {
Copy-Item -LiteralPath $objectTemplate -Destination (Join-Path $DBG 'objects.list') -Force
Write-Host 'restored canonical objects.list'
} else {
$srcs = @(); $mode = ''
foreach ($l in [System.IO.File]::ReadAllLines($tpl)) {
    if ($l -match '^C_SRCS') { $mode = 'src'; continue }
    if ($l -match '^OBJS')   { $mode = 'obj'; continue }
    if ($l -match '^C_DEPS') { $mode = 'dep'; continue }
    if ($l.Trim() -eq '')    { $mode = ''; continue }
    if ($mode -eq 'src') { $srcs += ($l.Trim() -replace ' \\$','') }
}
$list = New-Object System.Collections.Generic.List[string]
$list.Add('"./AI/output/int8/ai_app.o"')
$list.Add('"./AI/output/int8/ai_param.o"')
$list.Add('"./AI/output/int8/conv.o"')
foreach ($s in $srcs) { $list.Add('"' + ($s -replace '^\.\./','./' -replace '\.c$','.o') + '"') }
$list.Add('"./Core/Startup/startup_stm32f446retx.o"')
Get-ChildItem (Join-Path $DBG 'Drivers\STM32F4xx_HAL_Driver\Src') -Filter *.o -ErrorAction SilentlyContinue |
    Sort-Object Name | ForEach-Object { $list.Add('"./Drivers/STM32F4xx_HAL_Driver/Src/' + $_.BaseName + '.o"') }
foreach ($d in @('RTOS/Core/Src','RTOS/Port/GCC/ARM_CM7','RTOS/lib/third/CherryUSB/class/cdc','RTOS/lib/third/CherryUSB/class/hid','RTOS/lib/third/CherryUSB/class/msc','RTOS/lib/third/CherryUSB/core','RTOS/lib/third/CherryUSB/port/dwc2')) {
    $dir = Join-Path $DBG ($d -replace '/','\')
    Get-ChildItem $dir -Filter *.o -ErrorAction SilentlyContinue | Sort-Object Name |
        ForEach-Object { $list.Add('"./' + $d + '/' + $_.BaseName + '.o"') }
}
Set-Content (Join-Path $DBG 'objects.list') -Value $list
Write-Host "restored objects.list ($($list.Count) entries)"
}

# 3. drop variant build artifacts. NOTE: hil\fw holds the variant SOURCES and is
#    never deleted -- only the copied sources and compiled objects are removed.
foreach ($p in @((Join-Path $DBG 'hil'), $LOCAL, (Join-Path $DBG '.hil_build_stamp'))) {
    $resolvedTarget = [System.IO.Path]::GetFullPath($p)
    $resolvedRoot = [System.IO.Path]::GetFullPath($ROOT).TrimEnd('\') + '\'
    if (-not $resolvedTarget.StartsWith($resolvedRoot, [System.StringComparison]::OrdinalIgnoreCase)) { throw "unsafe cleanup target: $resolvedTarget" }
    if (Test-Path -LiteralPath $resolvedTarget) { Remove-Item -LiteralPath $resolvedTarget -Recurse -Force; Write-Host "removed $resolvedTarget" }
}
# main.o may have been compiled with the last variant's -DFIRMWARE_* set; make
# cannot see macro changes, so it must be dropped explicitly.
foreach ($f in @('Core\Src\main.o','Core\Src\main.d')) {
    $fp = Join-Path $DBG $f
    if (Test-Path $fp) { Remove-Item $fp -Force; Write-Host "removed $f" }
}

# 4. optional: rebuild the daily firmware
if ($Rebuild) {
    $ErrorActionPreference = 'Continue'
    $b = & $MAKE -C $DBG all -j8 2>&1
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    $buildLogDirectory = Join-Path $ROOT 'hil\logs'
    New-Item -ItemType Directory -Path $buildLogDirectory -Force | Out-Null
    $b | Out-File -LiteralPath (Join-Path $buildLogDirectory 'build_restored_daily.txt') -Encoding utf8
    if ($code -ne 0) { $b | Select-Object -Last 10 | ForEach-Object { Write-Host $_ }; throw 'rebuild failed' }
    & "$TOOLS\arm-none-eabi-size.exe" (Join-Path $DBG 'f446_rtos.elf')
}
Write-Host 'project restored'
