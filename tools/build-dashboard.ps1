param(
    [Parameter(Mandatory=$true)][string]$Phone,
    [Parameter(Mandatory=$true)][string]$IdentityFile,
    [switch]$Install
)
$ErrorActionPreference = 'Stop'
$source = Join-Path $PSScriptRoot '..\dashboard\n20-dashboard.c'
& scp -q -o BatchMode=yes -i $IdentityFile $source "root@${Phone}:/tmp/n20-dashboard.c"
if ($LASTEXITCODE -ne 0) { throw 'Could not copy source to the phone.' }
& ssh -o BatchMode=yes -o ConnectTimeout=6 -i $IdentityFile "root@$Phone" 'gcc -std=c11 -O2 -Wall -Wextra -Werror -I/usr/include/libdrm -I/usr/include/freetype2 /tmp/n20-dashboard.c -o /tmp/n20-dashboard.build -ldrm -lfreetype -lm'
if ($LASTEXITCODE -ne 0) { throw 'Dashboard compilation failed.' }
if ($Install) {
    & ssh -o BatchMode=yes -o ConnectTimeout=6 -i $IdentityFile "root@$Phone" 'install -m 755 /tmp/n20-dashboard.build /usr/local/sbin/n20-dashboard.new && mv /usr/local/sbin/n20-dashboard.new /usr/local/sbin/n20-dashboard'
    if ($LASTEXITCODE -ne 0) { throw 'Dashboard installation failed.' }
    Write-Output 'Installed; the new dashboard starts on the next boot.'
} else {
    Write-Output 'Built /tmp/n20-dashboard.build on the phone; no files installed.'
}
