param(
    [Parameter(Mandatory=$true)][string]$Phone,
    [Parameter(Mandatory=$true)][string]$IdentityFile,
    [switch]$Install
)
$ErrorActionPreference = 'Stop'
$source = Join-Path $PSScriptRoot '..\dashboard\n20-dashboard.c'
# Build under the root-owned runtime directory rather than shared /tmp paths.
$remoteDir = '/run/n20/dashboard-build-' + [Guid]::NewGuid().ToString('N')
$keepBuild = $false
try {
    & ssh -o BatchMode=yes -o ConnectTimeout=6 -i $IdentityFile "root@$Phone" "umask 077; mkdir -m 700 '$remoteDir'"
    if ($LASTEXITCODE -ne 0) { throw 'Could not create the private build directory on the phone.' }
    & scp -q -o BatchMode=yes -i $IdentityFile $source "root@${Phone}:$remoteDir/n20-dashboard.c"
    if ($LASTEXITCODE -ne 0) { throw 'Could not copy source to the phone.' }
    & ssh -o BatchMode=yes -o ConnectTimeout=6 -i $IdentityFile "root@$Phone" "gcc -std=c11 -O2 -Wall -Wextra -Werror -I/usr/include/libdrm -I/usr/include/freetype2 '$remoteDir/n20-dashboard.c' -o '$remoteDir/n20-dashboard' -ldrm -lfreetype -lm"
    if ($LASTEXITCODE -ne 0) { throw 'Dashboard compilation failed.' }
    if ($Install) {
        & ssh -o BatchMode=yes -o ConnectTimeout=6 -i $IdentityFile "root@$Phone" "install -m 755 '$remoteDir/n20-dashboard' /usr/local/sbin/n20-dashboard.new && mv /usr/local/sbin/n20-dashboard.new /usr/local/sbin/n20-dashboard"
        if ($LASTEXITCODE -ne 0) { throw 'Dashboard installation failed.' }
        Write-Output 'Installed; the new dashboard starts on the next phone boot.'
    } else {
        & ssh -o BatchMode=yes -o ConnectTimeout=6 -i $IdentityFile "root@$Phone" "rm -f '$remoteDir/n20-dashboard.c'"
        if ($LASTEXITCODE -ne 0) { throw 'Could not remove the temporary source copy.' }
        $keepBuild = $true
        Write-Output "Built $remoteDir/n20-dashboard on the phone; no files installed."
    }
} finally {
    if (-not $keepBuild) {
        & ssh -o BatchMode=yes -o ConnectTimeout=6 -i $IdentityFile "root@$Phone" "rm -f '$remoteDir/n20-dashboard.c' '$remoteDir/n20-dashboard'; rmdir '$remoteDir' 2>/dev/null || :" | Out-Null
    }
}
