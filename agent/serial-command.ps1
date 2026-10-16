param(
    [Parameter(Mandatory = $true)][string]$Command,
    [int]$TimeoutSeconds = 12
)

$ErrorActionPreference = 'Stop'
$consolePort = New-Object System.IO.Ports.SerialPort('COM5',115200,'None',8,'One')
$consolePort.DtrEnable = $false
$consolePort.RtsEnable = $false
$consolePort.ReadTimeout = 1000
try {
    $consolePort.Open()
    $consolePort.DiscardInBuffer()
    $consolePort.Write($Command + "`r")
    $serialText = ''
    $serialTimer = [System.Diagnostics.Stopwatch]::StartNew()
    do {
        Start-Sleep -Milliseconds 100
        $serialText += $consolePort.ReadExisting()
        if ($serialText -match '(?m)root@[^\r\n]+[#$] $') { break }
    } while ($serialTimer.Elapsed.TotalSeconds -lt $TimeoutSeconds)
    # 证据中只保留业务状态，隐藏可能由系统日志携带的设备唯一标识。
    $serialText = $serialText -replace '(?i)([0-9a-f]{2}:){5}[0-9a-f]{2}', '[MAC]'
    $serialText = $serialText -replace '(?im)(serial\s*(?:number)?\s*[:=]\s*)[^\r\n]+', '$1[redacted]'
    $serialText = $serialText -replace '(?i)\b[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\b', '[UUID]'
    Write-Output $serialText
} finally {
    if ($consolePort.IsOpen) { $consolePort.Close() }
    $consolePort.Dispose()
}
