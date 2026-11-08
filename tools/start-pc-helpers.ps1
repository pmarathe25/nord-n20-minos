param(
    [Parameter(Mandatory = $true)][string]$PythonPath,
    [string]$WifiInterface = 'Wi-Fi'
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $PythonPath -PathType Leaf)) {
    throw "Python executable not found: $PythonPath"
}
$timeScript = Join-Path $PSScriptRoot 'usb-auth-proxy.py'
$bridgeScript = Join-Path $PSScriptRoot 'lan-minecraft-bridge.py'
$timeProcess = $null
$bridgeProcess = $null
$address = ''
while ($true) {
    if ($null -eq $timeProcess -or $timeProcess.HasExited) {
        $timeProcess = Start-Process -FilePath $PythonPath -ArgumentList ('"' + $timeScript + '"'), '0.0.0.0' -WindowStyle Hidden -PassThru
    }
    $wifi = Get-NetIPAddress -InterfaceAlias $WifiInterface -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.IPAddress -notlike '169.254.*' } | Select-Object -First 1
    if ($null -ne $wifi) {
        $newAddress = $wifi.IPAddress + '/' + $wifi.PrefixLength
        if ($null -eq $bridgeProcess -or $bridgeProcess.HasExited -or $address -ne $newAddress) {
            if ($null -ne $bridgeProcess -and -not $bridgeProcess.HasExited) { Stop-Process -Id $bridgeProcess.Id }
            $bridgeProcess = Start-Process -FilePath $PythonPath -ArgumentList ('"' + $bridgeScript + '"'), $wifi.IPAddress, $newAddress -WindowStyle Hidden -PassThru
            $address = $newAddress
        }
    }
    Start-Sleep -Seconds 15
}
