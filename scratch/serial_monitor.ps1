param(
    [string]$PortName = "COM7",
    [int]$BaudRate = 115200
)

$host.UI.RawUI.WindowTitle = "ESP32 Live Serial Monitor [$PortName @ $BaudRate]"
Clear-Host
Write-Host "=======================================================================" -ForegroundColor Cyan
Write-Host "  🏍️  ESP32 TELEMATICS COMMAND CENTER - LIVE SERIAL MONITOR" -ForegroundColor Green
Write-Host "  Port: $PortName | Baud Rate: $BaudRate" -ForegroundColor White
Write-Host "  Interactive Commands: ENGINE_CUT  |  ENGINE_RESTORE" -ForegroundColor Yellow
Write-Host "  Type 'quit' or press Ctrl+C to close monitor." -ForegroundColor Gray
Write-Host "=======================================================================`n" -ForegroundColor Cyan

$port = New-Object System.IO.Ports.SerialPort $PortName, $BaudRate, None, 8, One
$port.DtrEnable = $false
$port.RtsEnable = $false

try {
    $port.Open()
    Write-Host "[+] Connected to $PortName. Streaming live output...`n" -ForegroundColor Green
} catch {
    Write-Host "[-] Error: Could not open $PortName ($_.Exception.Message)" -ForegroundColor Red
    Write-Host "Make sure no other program (like Arduino IDE) has the port open." -ForegroundColor Yellow
    Read-Host "Press Enter to exit..."
    exit 1
}

$lineBuffer = ""

try {
    while ($true) {
        if ($port.BytesToRead -gt 0) {
            $incoming = $port.ReadExisting()
            [Console]::ForegroundColor = [ConsoleColor]::Green
            [Console]::Write($incoming)
            [Console]::ResetColor()
        }

        if ([Console]::KeyAvailable) {
            $keyInfo = [Console]::ReadKey($true)

            if ($keyInfo.Key -eq [ConsoleKey]::Enter) {
                [Console]::WriteLine()
                if ($lineBuffer.Trim().ToLower() -eq "quit" -or $lineBuffer.Trim().ToLower() -eq "exit") {
                    break
                }
                if ($lineBuffer.Length -gt 0) {
                    $port.WriteLine($lineBuffer)
                    $lineBuffer = ""
                }
            } elseif ($keyInfo.Key -eq [ConsoleKey]::Backspace) {
                if ($lineBuffer.Length -gt 0) {
                    $lineBuffer = $lineBuffer.Substring(0, $lineBuffer.Length - 1)
                    [Console]::Write("`b `b")
                }
            } else {
                $c = $keyInfo.KeyChar
                if ($c -ge 32 -and $c -le 126) {
                    $lineBuffer += $c
                    [Console]::ForegroundColor = [ConsoleColor]::Yellow
                    [Console]::Write($c)
                    [Console]::ResetColor()
                }
            }
        }

        Start-Sleep -Milliseconds 15
    }
} finally {
    if ($port.IsOpen) {
        $port.Close()
    }
    Write-Host "`n[+] Serial port $PortName closed safely." -ForegroundColor Yellow
}
