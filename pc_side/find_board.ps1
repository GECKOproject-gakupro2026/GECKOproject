[CmdletBinding()]
param([int]$Port = 5000)

$neighbors = Get-NetNeighbor -AddressFamily IPv4 -ErrorAction Stop |
    Where-Object {
        $_.IPAddress -like '192.168.137.*' -and
        $_.IPAddress -ne '192.168.137.1' -and
        $_.State -notin @('Unreachable', 'Incomplete')
    } |
    Sort-Object IPAddress -Unique

$found = foreach ($neighbor in $neighbors) {
    $client = [System.Net.Sockets.TcpClient]::new()
    try {
        $task = $client.ConnectAsync($neighbor.IPAddress, $Port)
        try {
            $connected = $task.Wait(300) -and $client.Connected
        }
        catch [System.AggregateException] {
            $connected = $false
        }
        if ($connected) {
            [pscustomobject]@{
                IPAddress = $neighbor.IPAddress
                Port      = $Port
                Target    = "$($neighbor.IPAddress):$Port"
                LinkLayer = $neighbor.LinkLayerAddress
            }
        }
    }
    finally {
        $client.Dispose()
    }
}

if (-not $found) {
    Write-Warning 'Board TCP server not found in the ARP/neighbor table. Check the UART DHCP log, or ping the board first.'
}
$found
