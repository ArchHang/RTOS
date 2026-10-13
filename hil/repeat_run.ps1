# hil\repeat_run.ps1
# Flash the same ELF N times and report whether the output lines are stable.
# The reference reports claim byte-identical repeats, so this is the determinism check.
param(
    [string] $Elf = 'E:\STM32_Project\CubeIDE\f446_rtos\Debug\f446_rtos.elf',
    [string] $Tag = 'bm',
    [int]    $Runs = 3,
    [int]    $Seconds = 40,
    [string] $Terminator = 'BM\|DONE',
    [string] $Pattern = 'BM\|',
    [string] $ReplyOn = ''
)
$ErrorActionPreference = 'Stop'
$hil = 'E:\STM32_Project\CubeIDE\f446_rtos\hil'
$out = Join-Path $hil 'logs'
if (-not (Test-Path $out)) { New-Item -ItemType Directory -Path $out | Out-Null }
$Elf = [System.IO.Path]::GetFullPath($Elf)

$sets = @()
for ($i = 1; $i -le $Runs; $i++) {
    $lineArgs = @('-NoProfile','-ExecutionPolicy','Bypass','-File', "$hil\run_hil.ps1",
                  '-Elf', $Elf, '-Tag', "${Tag}_r$i", '-Seconds', "$Seconds", '-Terminator', $Terminator)
    if ($ReplyOn -ne '') { $lineArgs += @('-ReplyOn', $ReplyOn, '-Reply', 'X') }
    $o = & powershell.exe @lineArgs 2>&1
    $log = Join-Path $out "log_${Tag}_r$i.txt"
    if (-not (Test-Path $log)) { Write-Host "!! run $i produced no log:"; $o | Select-Object -Last 15 | ForEach-Object { Write-Host $_ }; continue }
    $lines = @(Get-Content $log | Select-String -Pattern $Pattern | ForEach-Object { $_.Line.Trim() })
    $sets += ,$lines
    Write-Host "--- run $i ($($lines.Count) lines) ---"
    $lines | ForEach-Object { Write-Host $_ }
}

Write-Host ""
Write-Host "=== stability across $($sets.Count) captured runs ==="
if ($sets.Count -lt 2) { Write-Host "not enough runs captured"; exit 1 }
$base = $sets[0]
$stable = $true
for ($i = 1; $i -lt $sets.Count; $i++) {
    if (($sets[$i] -join "`n") -ne ($base -join "`n")) { $stable = $false }
}
if ($stable) { Write-Host "IDENTICAL across all $($sets.Count) runs" }
else {
    Write-Host "DIFFERS across runs"
    for ($i = 0; $i -lt $sets.Count; $i++) { Write-Host "run$($i+1):"; $sets[$i] | ForEach-Object { Write-Host "   $_" } }
}
