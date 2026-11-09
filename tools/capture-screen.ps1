param(
    [Parameter(Mandatory=$true)][string]$Phone,
    [Parameter(Mandatory=$true)][string]$IdentityFile,
    [string]$OutputPath = 'n20-screen.ppm.gz'
)
$ErrorActionPreference = 'Stop'
# A fresh private directory keeps screen contents out of shared temporary paths.
$remoteDir = '/run/n20/n20-capture-' + [Guid]::NewGuid().ToString('N')
try {
    & ssh -o BatchMode=yes -o ConnectTimeout=6 -i $IdentityFile "root@$Phone" "umask 077; mkdir -m 700 '$remoteDir' && /usr/local/bin/n20-capture-screen '$remoteDir/frame.ppm' && gzip -c '$remoteDir/frame.ppm' > '$remoteDir/frame.ppm.gz'"
    if ($LASTEXITCODE -ne 0) { throw 'Phone capture failed; build and install the current dashboard/capture.c first.' }
    & scp -q -o BatchMode=yes -i $IdentityFile "root@${Phone}:$remoteDir/frame.ppm.gz" $OutputPath
    if ($LASTEXITCODE -ne 0) { throw 'Copying the screen capture failed.' }
    Write-Output "Saved $OutputPath"
} finally {
    & ssh -o BatchMode=yes -o ConnectTimeout=6 -i $IdentityFile "root@$Phone" "rm -f '$remoteDir/frame.ppm' '$remoteDir/frame.ppm.gz'; rmdir '$remoteDir' 2>/dev/null || :" | Out-Null
}
