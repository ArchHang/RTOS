# Flash an ELF and capture serial output until a terminator appears.
# Auto-replies 'X' once when the firmware arms the ISR-matrix test.
param(
    [Parameter(Mandatory = $true)][string] $Elf,
    [Parameter(Mandatory = $true)][string] $Tag,
    [int] $Seconds = 120,
    [string] $Terminator = 'SC\|DONE',
    [string] $Port = 'COM3',
    [int] $Baud = 115200
)
$ErrorActionPreference = 'Stop'

$CPCLI = 'E:\ST\STM32CubeIDE_1.13.2\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.1.100.202311100844\tools\bin\STM32_Programmer_CLI.exe'
$OUT = 'E:\STM32_Project\CubeIDE\f446_rtos\.trae\scenario_test'

if (-not (Test-Path $Elf)) { throw "missing elf: $Elf" }

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$sp.ReadTimeout = 200
$sp.Open()
$sp.DiscardInBuffer()

$flash = & $CPCLI -c port=SWD mode=UR -w $Elf -v -rst 2>&1
if ($LASTEXITCODE -ne 0) { $sp.Close(); $flash | Select-Object -Last 15; throw 'flash failed' }

$sb = New-Object System.Text.StringBuilder
$deadline = (Get-Date).AddSeconds($Seconds)
$isrArmed = $false

while ((Get-Date) -lt $deadline) {
    try { $null = $sb.Append($sp.ReadExisting()) } catch { }
    $text = $sb.ToString()
    if ((-not $isrArmed) -and ($text -match 'ST\|ISR\|ARM')) {
        Start-Sleep -Milliseconds 150
        $sp.Write('X')
        $isrArmed = $true
    }
    Start-Sleep -Milliseconds 40
    if ($text -match $Terminator) { break }
}
$sp.Close()

$text = $sb.ToString()
$log = Join-Path $OUT ("log_" + $Tag + ".txt")
Set-Content -Path $log -Value $text
Write-Host ("===== " + $Tag + " (" + $text.Length + " chars) -> " + $log + " =====")
Write-Host $text
