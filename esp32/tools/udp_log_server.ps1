param(
    [int]$Port = 4210,
    [string]$LogFile = "tools\udp_log.txt"
)

$fullLog = Join-Path (Get-Location) $LogFile
New-Item -ItemType Directory -Force -Path (Split-Path $fullLog) | Out-Null
"==== UDP log receiver started $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') port=$Port ====" | Out-File -FilePath $fullLog -Encoding utf8 -Append

$udp = [System.Net.Sockets.UdpClient]::new($Port)
$remote = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)
Write-Host "Listening for ESP32 UDP logs on port $Port ..."
Write-Host "Also writing to $fullLog"
Write-Host "Press Ctrl+C to stop."

try {
    while ($true) {
        $bytes = $udp.Receive([ref]$remote)
        $text = [System.Text.Encoding]::UTF8.GetString($bytes)
        $line = "[$($remote.Address)] $text"
        Write-Host -NoNewline $line
        Add-Content -Path $fullLog -Value $line -Encoding utf8
    }
}
finally {
    $udp.Close()
}
