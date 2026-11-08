# Local materials and reproduction

On this workstation, `local/original-downloads/` is a private, Git-ignored archive of the project files previously in Downloads. `local/original-implementation-plan.md` is the original plan. The archive preserves the working files as well as old experiments and downloads; most are not needed for ordinary development. Windows permissions restrict `local/` to the current user, Administrators, and SYSTEM. Git LFS is not used: the working source is small, the exact GPL BusyBox input has an upstream download, and the large device-specific vendor images, credentials and backups must not be published as a side effect of a push. An ignored directory is not a backup and is absent from clones.

`local/n20-radio-runtime-archive.tar.gz` is a checksum-verified copy from the running phone of `/usr/local/lib/n20-radio`, the IPA firmware files, and the two WLAN configuration files. It includes the complete installed Android compatibility library set and the current built radio helpers. It does not include the device's private modem calibration partitions or Wi-Fi credentials.

Important paths, relative to `local/original-downloads/`:

| Path | Purpose |
| --- | --- |
| `experiment/n20_usb_key` and `.pub` | Existing key for phone SSH. Protect the private key and copy it separately if access from another PC is needed. |
| `recovery/n20-backup.pass` | Password for the phone's encrypted Restic repository. This cannot be regenerated for the existing repository. |
| `recovery/world-history-2026-11-08.tar.gz` | An older off-phone copy of the local backup history, not a current or automatic cloud backup. |
| `recovery/n20-gpt-primary.bin` and `n20-gpt-backup.bin` | This device's original partition-table sectors. These are device-specific and cannot be recreated from an OTA. |
| `minecraft-source-snapshot/` | An older copy of the original PC Minecraft server's world and settings; it was not installed on the phone. Treat it as private user data. |
| `experiment/n20-persistent-UNTESTED.img` | The previously used custom boot image; the filename predates its successful use. Do not flash it as part of a routine build. |
| `busybox-static-1.37.0-r20.apk`, `c24-ota-research/unpacked/kernel`, `c24-ota-research/vendor-unpacked/vendor_ramdisk` | Exact inputs for the tested boot-image rebuild. |
| `c24-ota-research/` and `experiment/cnss-compat.tar.gz` | C.24 partition images, extracted Wi-Fi/firmware components and the vendor Android compatibility libraries used in research. Their redistribution rights have not been established. |
| `platform-tools/`, `google-usb-driver/`, `mkbootimg-src/`, `payload-dumper-src/` | Previously used tools and pinned sources. Current development uses the checked-in `third_party/mkbootimg/` files. |

The archived `ota-tools-venv/` was created at its old absolute path and is retained for reference; virtual environments are not generally portable after moving. Use a separately installed Python or create a fresh environment instead.

Make a separate protected backup of the SSH key, backup password, backup repository, and device-specific GPT sectors. The historical world copy is not a substitute for a current backup. A new SSH key can be authorized only while other phone access remains; an existing Restic password, GPT sectors, or personal world cannot be reproduced from source downloads.

## Rebuild the existing boot image

From Windows PowerShell at the repository root, with a Python 3 executable available:

```powershell
.\tools\rebuild-boot.ps1 -PythonPath 'C:\path\to\python.exe'
```

The script reads the ignored local archive, writes to ignored `build/`, and checks the resulting image against the known SHA-256. It does not flash the device. `-Archive` may specify a different copy of the archived input directory. The equivalent manual commands are in [device.md](device.md).

If the original BusyBox APK is lost, the exact aarch64 Alpine v3.22 package is [busybox-static-1.37.0-r20.apk](https://dl-cdn.alpinelinux.org/alpine/v3.22/main/aarch64/busybox-static-1.37.0-r20.apk), SHA-256 `ee469aee2958feffd7f64dd96655704025ff60af614f77a1d7323dc237c34da2`. The package declares GPL-2.0-only; its [Alpine APKBUILD](https://raw.githubusercontent.com/alpinelinux/aports/2e97d754a30d558f15524b8e422303c8a96832df/main/busybox/APKBUILD) identifies its source and build patches. Obtain it independently, verify the hash, and place it at the path above. Upstream archives may change, so keep a copy of the verified input.

The matching vendor inputs can be re-extracted from the [C.24 OTA package](https://android.googleapis.com/packages/ota-api/package/c9df99a58a0a6b3d447f6703c33d424872d3778e.zip) using [payload_dumper](https://github.com/manojanasuri16/payload_dumper) commit `a9eacbfe4e924f8afa09af6056e0c7d74b9c92b7`. This example extracts to a new directory and does not flash anything:

```sh
git clone https://github.com/manojanasuri16/payload_dumper.git
git -C payload_dumper checkout a9eacbfe4e924f8afa09af6056e0c7d74b9c92b7
python3 -m pip install ./payload_dumper
payload-dumper 'https://android.googleapis.com/packages/ota-api/package/c9df99a58a0a6b3d447f6703c33d424872d3778e.zip' -p boot vendor_boot -o extracted-c24
python3 third_party/mkbootimg/unpack_bootimg.py --boot_img extracted-c24/boot.img --out extracted-c24/unpacked
python3 third_party/mkbootimg/unpack_bootimg.py --boot_img extracted-c24/vendor_boot.img --out extracted-c24/vendor-unpacked
```

Copy the extracted `kernel` and `vendor_ramdisk` into the corresponding local archive paths only after comparing their SHA-256 hashes with the previously verified files. The initramfs builder verifies the vendor-ramdisk and BusyBox hashes; the final-image hash also checks the kernel. Extraction checks against the OTA payload manifest do not independently verify the package signature. This older C.24 package is not a complete recovery image for the phone's original C.37 Android build.

The `system`, `vendor`, `modem`, `dtbo`, and `vbmeta` partitions may also be extracted from that OTA for research. The exact `cnss-daemon`, Android libraries, WLAN module and firmware used by this setup are preserved in the ignored archive and on the phone. Their full extraction from the partition images is not automated; see [device.md](device.md). No procedure for reconstructing unique calibration data or restoring a fully bricked phone has been verified.

While the configured phone is reachable, the installed runtime archive can be recreated without reading calibration partitions or Wi-Fi credentials. In PowerShell at the repository root, set `$key` to the private SSH key and `$phone` to its current address:

```powershell
ssh -i $key root@$phone 'tar -C / -czf /tmp/n20-radio-runtime-archive.tar.gz usr/local/lib/n20-radio usr/lib/firmware/ipa_fws.mdt usr/lib/firmware/ipa_fws.b00 usr/lib/firmware/ipa_fws.b01 usr/lib/firmware/ipa_fws.b02 usr/lib/firmware/ipa_fws.b03 usr/lib/firmware/ipa_fws.b04 usr/lib/firmware/wlan/WCNSS_qcom_cfg.ini usr/lib/firmware/wlan/qca_cld/WCNSS_qcom_cfg.ini'
scp -i $key "root@${phone}:/tmp/n20-radio-runtime-archive.tar.gz" local/n20-radio-runtime-archive.tar.gz
ssh -i $key root@$phone 'sha256sum /tmp/n20-radio-runtime-archive.tar.gz'
Get-FileHash local/n20-radio-runtime-archive.tar.gz -Algorithm SHA256
```

Compare the hashes. This records the running installation rather than establishing the provenance of each vendor binary, and cannot replace the separate unique backups above. Do not extract it over a working phone without reviewing the target files and permissions.

## Windows USB helpers

The optional PC USB time service and Minecraft TCP bridge are tracked as `tools/usb-auth-proxy.py`, `tools/lan-minecraft-bridge.py`, and `tools/start-pc-helpers.ps1`. The Windows Startup entry points to the latter and passes the installed Python executable. The phone uses Wi-Fi directly under normal operation; these helpers support the USB fallback. If the checkout moves, update the Startup entry. If Python moves, update its `-PythonPath` argument. They are not Google Drive backup tools.
