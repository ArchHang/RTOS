param([string] $Tag = 'audit_20261012_usb_timer_full_before')
$ErrorActionPreference = 'Stop'
$projectRoot = 'E:\STM32_Project\CubeIDE\f446_rtos'
$auditDir = Join-Path $projectRoot 'hil\audit_20261012'
$utf8 = New-Object System.Text.UTF8Encoding($false)

# Called explicitly by the hardware-test coordinator, with other writers idle.
# Reproduce only the original two USB timer-delete functions; keep all other
# fixes and the queue-full fixture identical to the final audit firmware.
function Get-FunctionSpan([string] $Source, [string] $Name) {
    $pattern = '(?m)^void\s+' + [regex]::Escape($Name) + '\s*\([^\r\n]*\)\s*\r?\n\{'
    $match = [regex]::Match($Source, $pattern)
    if (-not $match.Success) { throw "function not found: $Name" }
    $depth = 1
    $cursor = $match.Index + $match.Length
    while ($cursor -lt $Source.Length -and $depth -gt 0) {
        if ($Source[$cursor] -eq '{') { $depth++ }
        if ($Source[$cursor] -eq '}') { $depth-- }
        $cursor++
    }
    if ($depth -ne 0) { throw "unbalanced function: $Name" }
    return @{ Start = $match.Index; Length = $cursor - $match.Index }
}

$targets = @(
    @{ Relative = 'Core\Src\rtos_usb.c'; Function = 'rtos_usb_timer_delete' },
    @{ Relative = 'Core\Src\rtos_usb_osal.c'; Function = 'usb_osal_timer_delete' }
)
$saved = @{}
try {
    foreach ($target in $targets) {
        $currentPath = Join-Path (Join-Path $projectRoot 'RTOS') $target.Relative
        $baselinePath = Join-Path (Join-Path $auditDir 'baseline_RTOS') $target.Relative
        $saved[$currentPath] = [System.IO.File]::ReadAllBytes($currentPath)
        $backupName = $Tag + '_' + [System.IO.Path]::GetFileName($currentPath) + '.current'
        [System.IO.File]::WriteAllBytes((Join-Path $auditDir $backupName), $saved[$currentPath])
        $current = [System.IO.File]::ReadAllText($currentPath)
        $baseline = [System.IO.File]::ReadAllText($baselinePath)
        $oldSpan = Get-FunctionSpan $baseline $target.Function
        $newSpan = Get-FunctionSpan $current $target.Function
        $originalFunction = $baseline.Substring($oldSpan.Start, $oldSpan.Length)
        $modified = $current.Substring(0, $newSpan.Start) + $originalFunction +
                    $current.Substring($newSpan.Start + $newSpan.Length)
        [System.IO.File]::WriteAllText($currentPath, $modified, $utf8)
    }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $projectRoot 'hil\build_fw.ps1') -Variant audit
    if ($LASTEXITCODE -ne 0) { throw 'USB original-delete verification build failed' }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $projectRoot 'hil\run_hil.ps1') `
        -Elf (Join-Path $projectRoot 'Debug\f446_rtos.elf') -Tag $Tag -Seconds 60 `
        -Terminator 'AT\|DONE' -OutDir $auditDir -AllowFailures
    if ($LASTEXITCODE -ne 0) { throw 'USB original-delete verification capture incomplete' }
} finally {
    foreach ($currentPath in $saved.Keys) {
        [System.IO.File]::WriteAllBytes($currentPath, $saved[$currentPath])
        $restored = [System.IO.File]::ReadAllBytes($currentPath)
        if ([System.Convert]::ToBase64String($restored) -ne
            [System.Convert]::ToBase64String($saved[$currentPath])) {
            throw "source restore verification failed: $currentPath"
        }
    }
    Write-Host 'Current USB sources restored byte-for-byte. Rebuild final audit before its next flash.'
}
