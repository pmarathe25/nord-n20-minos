param(
    [Parameter(Mandatory = $true)][string]$PythonPath,
    [string]$FirmwareDir = (Join-Path $PSScriptRoot '..\local\firmware\c24'),
    [string]$BusyBoxApk = (Join-Path $PSScriptRoot '..\third_party\busybox\busybox-static-1.37.0-r20.apk')
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'build'
$vendor = Join-Path $FirmwareDir 'vendor_ramdisk'
$kernel = Join-Path $FirmwareDir 'kernel'
$ramdisk = Join-Path $build 'persistent-initramfs.cpio.gz'
$image = Join-Path $build 'n20-boot.img'
New-Item -ItemType Directory -Force -Path $build | Out-Null
& $PythonPath (Join-Path $root 'boot\build_initramfs.py') --busybox-apk $BusyBoxApk --vendor-ramdisk $vendor --output $ramdisk
if ($LASTEXITCODE -ne 0) { throw 'Initramfs build failed.' }
& $PythonPath (Join-Path $root 'third_party\mkbootimg\mkbootimg.py') --header_version 3 --os_version 12.0.0 --os_patch_level 2024-10 --kernel $kernel --ramdisk $ramdisk --output $image
if ($LASTEXITCODE -ne 0) { throw 'Boot image build failed.' }
$expected = '7f1791ec2faea02d2193657432f4c92d20dbd65477ba80b87032a28abd11c64f'
$actual = (Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actual -ne $expected) { throw "Boot image differs from the known build: $actual" }
Write-Output "Verified boot image: $image"
