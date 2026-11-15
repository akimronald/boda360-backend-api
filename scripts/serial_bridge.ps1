param(
    [string]$Port = "COM7",
    [int]$Baud = 115200
)

$ErrorActionPreference = 'Continue'
$sp = $null

try {
    $sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
    $sp.DtrEnable = $false
    $sp.RtsEnable = $false
    $sp.Open()
    [Console]::WriteLine("__SERIAL_CONNECTED__")

    # Command file watcher path
    $cmdFile = Join-Path $PSScriptRoot "pending_command.txt"

    # Continuous Read Loop from ESP32
    while ($sp.IsOpen) {
        # Check if there is data to read from ESP32
        if ($sp.BytesToRead -gt 0) {
            try {
                $line = $sp.ReadLine()
                if ($line) {
                    [Console]::WriteLine($line)
                }
            } catch {}
        } else {
            [System.Threading.Thread]::Sleep(35)
        }

        # Check for outgoing commands from Node.js via file trigger
        if (Test-Path $cmdFile) {
            try {
                $action = Get-Content $cmdFile -Raw
                Remove-Item $cmdFile -Force -ErrorAction SilentlyContinue
                if ($action -and $sp.IsOpen) {
                    $action = $action.Trim()
                    $sp.WriteLine($action)
                    [Console]::WriteLine("[COMMAND DISPATCHED TO ESP32]: $action")
                }
            } catch {}
        }
    }
} catch {
    [Console]::WriteLine("__SERIAL_ERROR__: " + $_.Exception.Message)
    exit 1
} finally {
    if ($sp -and $sp.IsOpen) {
        $sp.Close()
        $sp.Dispose()
    }
}
