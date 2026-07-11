[CmdletBinding()]
param(
    [ValidateSet('Start', 'Stop', 'Status')]
    [string]$Action = 'Start',
    [string]$Ssid = 'U585-IOT02A',
    [string]$Passphrase = 'u585iot02a'
)

$ErrorActionPreference = 'Stop'

function Wait-WinRtOperation {
    param(
        [Parameter(Mandatory)] $Operation,
        [Type]$ResultType
    )

    $methods = [System.WindowsRuntimeSystemExtensions].GetMethods() |
        Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 }
    if ($null -ne $ResultType) {
        $method = $methods | Where-Object {
            $_.IsGenericMethod -and
            $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
        } | Select-Object -First 1
        $task = $method.MakeGenericMethod($ResultType).Invoke($null, @($Operation))
    }
    else {
        $method = $methods | Where-Object {
            -not $_.IsGenericMethod -and
            $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncAction'
        } | Select-Object -First 1
        $task = $method.Invoke($null, @($Operation))
    }
    $task.GetAwaiter().GetResult()
}

Add-Type -AssemblyName System.Runtime.WindowsRuntime
[void][Windows.Networking.Connectivity.NetworkInformation, Windows.Networking.Connectivity, ContentType=WindowsRuntime]
[void][Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager, Windows.Networking.NetworkOperators, ContentType=WindowsRuntime]
[void][Windows.Networking.NetworkOperators.NetworkOperatorTetheringAccessPointConfiguration, Windows.Networking.NetworkOperators, ContentType=WindowsRuntime]
[void][Windows.Networking.NetworkOperators.TetheringWiFiBand, Windows.Networking.NetworkOperators, ContentType=WindowsRuntime]

$profile = [Windows.Networking.Connectivity.NetworkInformation]::GetInternetConnectionProfile()
if ($null -eq $profile) {
    throw 'No Internet connection profile is available to share.'
}

$manager = [Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager]::CreateFromConnectionProfile($profile)

switch ($Action) {
    'Start' {
        if ($Passphrase.Length -lt 8) {
            throw 'The hotspot passphrase must contain at least 8 characters.'
        }
        $config = [Windows.Networking.NetworkOperators.NetworkOperatorTetheringAccessPointConfiguration]::new()
        $config.Ssid = $Ssid
        $config.Passphrase = $Passphrase
        $config.Band = [Windows.Networking.NetworkOperators.TetheringWiFiBand]::TwoPointFourGigahertz
        Wait-WinRtOperation ($manager.ConfigureAccessPointAsync($config)) | Out-Null
        $result = Wait-WinRtOperation ($manager.StartTetheringAsync()) ([Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult])
        if ($result.Status -ne 'Success') {
            throw "Hotspot start failed: $($result.Status) $($result.AdditionalErrorMessage)"
        }
    }
    'Stop' {
        $result = Wait-WinRtOperation ($manager.StopTetheringAsync()) ([Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult])
        if ($result.Status -ne 'Success') {
            throw "Hotspot stop failed: $($result.Status) $($result.AdditionalErrorMessage)"
        }
    }
}

$current = $manager.GetCurrentAccessPointConfiguration()
[pscustomobject]@{
    State       = $manager.TetheringOperationalState
    Clients     = $manager.ClientCount
    SSID        = $current.Ssid
    Band        = $current.Band
    SharedFrom  = $profile.ProfileName
    BoardSubnet = '192.168.137.0/24'
    TcpPort     = 5000
}
