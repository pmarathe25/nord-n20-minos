# Local materials and reproduction

`local/` is Git-ignored and restricted on the original workstation to the current user, Administrators, and SYSTEM. It is not included in GitHub clones. It contains only device-specific or private materials needed for this setup; Minecraft world data and backup repositories remain on the phone.

| Path under `local/` | Purpose |
| --- | --- |
| `secrets/n20_usb_key` and `.pub` | Existing SSH key pair. The private key must remain private. |
| `recovery/n20-backup.pass` | Password for the phone's encrypted Restic repository; it cannot be regenerated for that repository. |
| `recovery/n20-gpt-primary.bin` and `n20-gpt-backup.bin` | This phone's original GPT sectors; unique device data, not reproducible from an OTA. |
| `firmware/c24/kernel` and `vendor_ramdisk` | Exact vendor inputs needed by the existing boot-image build. |
| `firmware/c24/{vendor_boot,dtbo,vbmeta,modem}.img.gz` | Compressed copies of the C.24 OTA partitions used by the Linux setup. Each was checked against the original image before compression; these are not a complete C.37 recovery package. |
| `firmware/stock-kernel-config.gz` | The original Android installation's kernel configuration, saved for reference. |
| `n20-radio-runtime-archive.tar.gz` | Checksum-verified copy of the phone's radio runtime and firmware configuration. |
| `history/` | Original plan and small historical research/diagnostic notes; earlier state descriptions may be stale. |

Keep separate protected copies of the SSH key, Restic password, and GPT sectors. A new SSH key can be authorized while another phone access method remains, but the existing Restic password and GPT sectors cannot be recreated from source. The phone's world and backup repository are not in Git.

## Redistributable inputs

Install Git LFS and run `git lfs pull` after cloning. The exact BusyBox APK, its BusyBox source tarball, and the matching Alpine packaging source and patches are included in `third_party/busybox/`; see its README for hashes and upstream locations. The pinned AOSP boot-image tools and qbootctl source, with their licenses, are in `third_party/`.

The C.24 kernel, vendor ramdisk and radio runtime are derived from OnePlus/Qualcomm software. Their redistribution rights have not been established, so they remain local even though they are not personal secrets. OnePlus's [user agreement](https://www.oneplus.com/us/legal/user-agreement) limits redistribution of materials made available from its site; it does not establish a license to republish these extracted OTA components. They can be re-extracted from the official OTA as below. Preserve a separate authorized copy if the vendor URL later disappears.

## Rebuild the existing boot image

From Windows PowerShell at the repository root, after fetching Git LFS objects:

```powershell
.\tools\rebuild-boot.ps1 -PythonPath 'C:\path\to\python.exe'
```

The script reads the tracked BusyBox APK and `local/firmware/c24/{kernel,vendor_ramdisk}`, writes to ignored `build/`, and checks the final image SHA-256. It does not flash. `-FirmwareDir` or `-BusyBoxApk` can override the defaults. The equivalent manual commands are in [device.md](device.md).

To reproduce the two vendor inputs, extract `boot` and `vendor_boot` from the [C.24 OTA package](https://android.googleapis.com/packages/ota-api/package/c9df99a58a0a6b3d447f6703c33d424872d3778e.zip) with [payload_dumper](https://github.com/manojanasuri16/payload_dumper) commit `a9eacbfe4e924f8afa09af6056e0c7d74b9c92b7`. This example writes to a new directory and does not flash:

```sh
git clone https://github.com/manojanasuri16/payload_dumper.git
git -C payload_dumper checkout a9eacbfe4e924f8afa09af6056e0c7d74b9c92b7
python3 -m pip install ./payload_dumper
payload-dumper 'https://android.googleapis.com/packages/ota-api/package/c9df99a58a0a6b3d447f6703c33d424872d3778e.zip' -p boot vendor_boot -o extracted-c24
python3 third_party/mkbootimg/unpack_bootimg.py --boot_img extracted-c24/boot.img --out extracted-c24/unpacked
python3 third_party/mkbootimg/unpack_bootimg.py --boot_img extracted-c24/vendor_boot.img --out extracted-c24/vendor-unpacked
```

Check SHA-256 before copying the extracted files to `local/firmware/c24/`:

- `kernel`: `5f8108872f15e7fd5fd6f30b37d8b208845e2f3d726bfd2912a136ae928f8ee4`
- `vendor_ramdisk`: `8dd5faeefde663f7037540c07c3bce6a6e0452afdb3b995a08c2bc5b98c3eea9`

Payload-manifest hash checks do not independently verify the OTA's signature. This C.24 package is not a complete recovery image for the phone's original C.37 Android build.

The radio archive includes `/usr/local/lib/n20-radio`, IPA firmware, and the two WLAN configuration files. It does not include private modem calibration partitions or Wi-Fi credentials. While the configured phone is reachable, it can be recreated using PowerShell with `$key` set to the private SSH key and `$phone` to its current address:

```powershell
ssh -i $key root@$phone 'tar -C / -czf /tmp/n20-radio-runtime-archive.tar.gz usr/local/lib/n20-radio usr/lib/firmware/ipa_fws.mdt usr/lib/firmware/ipa_fws.b00 usr/lib/firmware/ipa_fws.b01 usr/lib/firmware/ipa_fws.b02 usr/lib/firmware/ipa_fws.b03 usr/lib/firmware/ipa_fws.b04 usr/lib/firmware/wlan/WCNSS_qcom_cfg.ini usr/lib/firmware/wlan/qca_cld/WCNSS_qcom_cfg.ini'
scp -i $key "root@${phone}:/tmp/n20-radio-runtime-archive.tar.gz" local/n20-radio-runtime-archive.tar.gz
ssh -i $key root@$phone 'sha256sum /tmp/n20-radio-runtime-archive.tar.gz'
Get-FileHash local/n20-radio-runtime-archive.tar.gz -Algorithm SHA256
```

Compare the hashes. This records the running installation and does not establish each vendor binary's provenance. Do not extract it over a working phone without reviewing the target files and permissions. No procedure for reconstructing unique calibration data or recovering a fully bricked phone has been verified.

## Windows tools and USB helpers

ADB and Fastboot can be downloaded from [Google's SDK Platform-Tools](https://developer.android.com/tools/releases/platform-tools); the [Google USB driver](https://developer.android.com/studio/run/win-usb) is available separately. The previously used Platform-Tools ZIP had SHA-256 `45f4d63113e895ebde0c90f194099a4676b6ac653bd28d54314a9e022bbc1a99`; the latest download can differ.

The optional PC USB time service and Minecraft TCP bridge are tracked as `tools/usb-auth-proxy.py`, `tools/lan-minecraft-bridge.py`, and `tools/start-pc-helpers.ps1`. The Windows Startup entry points to the latter and passes the installed Python executable. If the checkout or Python moves, update the entry. These helpers are not Google Drive backup tools.
