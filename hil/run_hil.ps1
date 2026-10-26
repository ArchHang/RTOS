# hil\run_hil.ps1
# Flash an ELF over SWD and capture the serial output on COM3.
# Flash and capture are started in the SAME invocation so that no boot output
# is lost (lesson recorded in docs\rtos_testing_report.md §5.1).
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File hil\run_hil.ps1 -Elf <path> -Tag <name> `
#       -Seconds 60 -Terminator 'BM\|DONE' [-ReplyOn 'ST\|ISR\|ARM' -Reply 'X']
param(
    [Parameter(Mandatory = $true)][string] $Elf,
    [Parameter(Mandatory = $true)][string] $Tag,
    [int]    $Seconds = 60,
    [string] $Terminator = '',
    [string] $ReplyOn = '',
    [string] $Reply = 'X',
    [string] $Port = 'COM3',
    [int]    $Baud = 115200,
    [string] $OutDir = '',
    [switch] $NoFlash,
    [switch] $AllowFailures,
    [switch] $LiveLog
)
$ErrorActionPreference = 'Stop'

$CLI = 'E:\ST\STM32CubeIDE_1.13.2\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.1.100.202311100844\tools\bin\STM32_Programmer_CLI.exe'
if ($OutDir -eq '') { $OutDir = Join-Path $PSScriptRoot 'logs' }
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }
$log = Join-Path $OutDir ("log_" + $Tag + ".txt")

if (-not (Test-Path $Elf)) { throw "missing elf: $Elf" }
$elfPath = (Resolve-Path -LiteralPath $Elf).Path
$started = (Get-Date).ToString('o')
$firmwareHash = (Get-FileHash -LiteralPath $elfPath -Algorithm SHA256).Hash
Copy-Item -LiteralPath $elfPath -Destination (Join-Path $OutDir ("firmware_" + $Tag + '.elf')) -Force

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$sp.ReadTimeout = 200
$sp.Open()
$sp.DiscardInBuffer()
try {
if (-not $NoFlash) {
    $flash = & $CLI -c port=SWD mode=UR -w $Elf -v -rst 2>&1
    $flash | Out-File -LiteralPath (Join-Path $OutDir ("flash_" + $Tag + '.txt')) -Encoding utf8
    if ($LASTEXITCODE -ne 0) {
        $sp.Close()
        $flash | Select-Object -Last 25
        throw 'flash failed'
    }
}

$sb = New-Object System.Text.StringBuilder
$nextLiveWrite = [DateTime]::MinValue
$deadline = (Get-Date).AddSeconds($Seconds)
$replied = $false

while ((Get-Date) -lt $deadline) {
    try { $null = $sb.Append($sp.ReadExisting()) } catch { }
    $text = $sb.ToString()
    if ($LiveLog -and (Get-Date) -ge $nextLiveWrite) {
        # A partial snapshot permits read-only monitoring without opening COM
        # twice. The final log and acceptance checks still use the full buffer.
        [System.IO.File]::WriteAllText(($log + '.partial'), $text)
        $nextLiveWrite = (Get-Date).AddSeconds(2)
    }
    if ((-not $replied) -and ($ReplyOn -ne '') -and ($text -match $ReplyOn)) {
        Start-Sleep -Milliseconds 150
        $sp.Write($Reply)
        $replied = $true
    }
    if (($Terminator -ne '') -and ($text -match $Terminator)) { break }
    Start-Sleep -Milliseconds 30
}
Start-Sleep -Milliseconds 300
try { $null = $sb.Append($sp.ReadExisting()) } catch { }
} finally { $sp.Close(); $sp.Dispose() }

$text = $sb.ToString()
Set-Content -Path $log -Value $text -NoNewline
if ($LiveLog -and (Test-Path -LiteralPath ($log + '.partial'))) {
    Remove-Item -LiteralPath ($log + '.partial') -Force
}
Write-Host ("===== " + $Tag + " (" + $text.Length + " chars) -> " + $log + " =====")
Write-Host $text
[ordered]@{ tag=$Tag; started=$started; finished=(Get-Date).ToString('o'); elf=$elfPath;
    sha256=$firmwareHash; port=$Port; baud=$Baud; flashed=(-not $NoFlash);
    terminator=$Terminator; terminator_seen=($Terminator -eq '' -or $text -match $Terminator);
    failure_lines=@($text -split "`r?`n" | Where-Object { $_ -match '\|FAIL(?:\||$)' })
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutDir ("run_" + $Tag + '.json')) -Encoding utf8
if ($Terminator -ne '' -and $text -notmatch $Terminator) { throw "capture incomplete: missing terminator '$Terminator' (see $log)" }
if (-not $AllowFailures -and $text -match '\|FAIL(?:\||\r?\n)') { throw "firmware reported a failed assertion (see $log)" }
