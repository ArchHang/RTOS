# hil\restore_project.ps1
# Restore the complete canonical daily build without changing main.c.
#
#   * Debug rules and inventories <- committed hil/templates/
#   * Debug\objects.list        <- complete canonical object list
#   * Debug\hil\                <- removed (variant object files)
#   * hil\fwsrc\                <- removed (copied variant sources)
#   * Audit-only compile/link hooks are removed from Debug\makefile and RTOS rules
#   * All generated make rules and directory inventory are restored; main.c is untouched
#
# Usage: powershell -ExecutionPolicy Bypass -File hil\restore_project.ps1
param([switch] $Rebuild)
$ErrorActionPreference = 'Stop'
$ROOT  = Split-Path $PSScriptRoot -Parent
$DBG   = Join-Path $ROOT 'Debug'
$LOCAL = Join-Path $ROOT 'hil\fwsrc'
. (Join-Path $PSScriptRoot 'build_layout.ps1')
$tpl   = Join-Path $ROOT 'hil\templates\subdir.mk.template'
$TOOLS = 'E:\ST\STM32CubeIDE_1.13.2\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.11.3.rel1.win32_1.1.100.202309141235\tools\bin'
$MAKE  = 'E:\ST\STM32CubeIDE_1.13.2\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.make.win32_2.1.100.202310302056\tools\bin\make.exe'
$env:PATH = "$TOOLS;$env:PATH"

if (-not (Test-Path $tpl)) { throw "missing pristine template: $tpl" }

# 1. Restore the complete generated tree, including after an IDE regeneration
# or a removed Debug directory. Keep the last variant rule as a local diagnostic.
$mk = Join-Path $DBG 'Core/Src/subdir.mk'
if (Test-Path -LiteralPath $mk) {
    Copy-Item -LiteralPath $mk -Destination (Join-Path $ROOT 'hil/templates/subdir.mk.last_variant') -Force
}
Initialize-HilBuildTree -ProjectRoot $ROOT -DebugRoot $DBG -RestoreRules
Write-Host 'restored all canonical make rules, source inventory, and object list'

# 2. drop variant build artifacts. NOTE: hil\fw holds the variant SOURCES and is
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

# 3. optional: rebuild the daily firmware
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
