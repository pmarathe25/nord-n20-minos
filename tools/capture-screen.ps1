param(
    [Parameter(Mandatory=$true)][string]$Phone,
    [Parameter(Mandatory=$true)][string]$IdentityFile,
    [string]$OutputPath = 'n20-screen.ppm.gz'
)
$ErrorActionPreference = 'Stop'
& ssh -o BatchMode=yes -o ConnectTimeout=6 -i $IdentityFile "root@$Phone" '/usr/local/bin/n20-capture-screen && gzip -c /tmp/drm-frame.ppm > /tmp/n20-screen.ppm.gz'
if ($LASTEXITCODE -ne 0) { throw 'Phone capture failed; build and install dashboard/capture.c first.' }
& scp -q -o BatchMode=yes -i $IdentityFile "root@${Phone}:/tmp/n20-screen.ppm.gz" $OutputPath
if ($LASTEXITCODE -ne 0) { throw 'Copying the screen capture failed.' }
Write-Output "Saved $OutputPath"
