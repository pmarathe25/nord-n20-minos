# Device and development notes

## Current architecture

The tested phone is CPH2459 with an unlocked bootloader. Slot B combines C.24 `vendor_boot`, `dtbo`, `vbmeta`, and a custom boot image containing kernel `5.4.254-qgki-g93bb89e5a062`. The kernel starts BusyBox `/init`, mounts the ext4 userdata volume and bind-mounts `/dev`, `/proc`, and `/sys` into the Debian root. The UUID in `boot/persistent-init` is the existing phone's UUID. Do not substitute another device or assume this image is portable. `fastboot boot` was not supported by this phone.

`minecraft-supervisor` starts the radio, Wi-Fi manager, MTP, timekeeper, SSH, dashboard support, backup scheduler, and Minecraft. Java runs as `minecraft` using `/opt/java`; the tested installation had Java 25 and Minecraft Java 26.3. Wi-Fi is normally the preferred route. USB serial provides a root shell and is not password-protected. SSH accepts key authentication and was configured to reject passwords. Protect the private key and physical access.

`config/server.properties.example` records the relevant non-secret Minecraft settings only. The management API secret must be a separately generated 40-character alphanumeric value, and the live `server.properties` must remain protected and uncommitted. Management traffic is unencrypted and must remain bound to loopback. The installed key-only SSH configuration is in `runtime/etc/ssh/sshd_config_n20`; authorized keys are device-specific and excluded.

The screen keeps the touch controller and display panel powered at zero brightness for tap-to-wake. Its wake guard consumes the initial touch. Power usage, OLED wear, multiplayer load, and fully depleted-battery startup have not been characterized. A normal reboot restored Wi-Fi, the server and USB MTP. Wi-Fi internet, DNS and NTP worked with USB networking temporarily disabled; a physical unplugged reboot was not tested.

## Development dependencies

The phone uses Debian 13 arm64. Build the dashboard with `gcc`, `libdrm-dev`, and `libfreetype-dev`. The radio helpers require `gcc`, `libqrtr-dev`, `libudev-dev`, and `libzstd-dev`. `QRTR_PREFIX` can point `tools/build-radio.sh` at an extracted development sysroot, for example `QRTR_PREFIX=/tmp/n20-qcom-tools/usr sh tools/build-radio.sh /tmp/n20-radio-build`; the script also requires the system `libudev.so.1`. `restic`, `rsync`, `nice` and `ionice` are needed for the isolated backup tests. The runtime also needs `wpasupplicant`, `iw`, `umtp-responder`, OpenSSH, BusyBox, Python 3, and the Java runtime.

The preserved `pd-mapper` source currently emits a `-Wtype-limits` warning for an unsigned comparison; the build does not treat third-party warnings as errors. Investigate it before modifying that code.

The tested installation has a diverted `/usr/bin/kmscon`; `runtime/usr/bin/kmscon` launches the dashboard and falls back to `/usr/bin/kmscon.distrib`. Copying a launcher into a new installation is not enough: the diversion, display permissions, device nodes, firmware, server user, Java, SSH configuration, keys, and actual server setup also need to exist. This repository does not automate a fresh root filesystem or image installation.

The admin tests change their module's paths to disposable directories under `/tmp`; snapshot and restore tests create their own temporary Restic repositories. The tests do not alter the live world. Run them from a Unix-like environment: `fcntl`, Linux paths, and the external tools above are required.

## Wi-Fi implementation

The C.24 kernel enforces module signatures. Its matching signed `qca_cld3_wlan.ko` has SHA-256 `f52a095b045c6163c2b682973b31a473f0152d04fe8f7366b1becb244faaae00`. `n20-radio-start` starts patched `rmtfs`, `pd-mapper`, and `tqftpserv`, holds the modem open, starts the C.24 `cnss-daemon` with an Android compatibility library set and the included `cnss-pm-stub.c`, waits for firmware, then loads the signed WLAN module. The EFS and calibration partitions are copied to private temporary memory and served through read-only/shadow rmtfs; the original partitions are not intentionally written. The hard-coded partition names and sysfs paths are device-specific.

The existing installation stores the built helpers and extracted vendor files in `/usr/local/lib/n20-radio`, including the signed module, a `qrtr/` library directory, and `android/` with `cnss-daemon`, `linker64`, Android shared libraries and the built PM stub. It mounts the stock `modem_b` partition read-only at `/mnt/n20-modem`; `/vendor/firmware_mnt/image` points to `/mnt/n20-modem/image`, `/data/vendor/wifi` to `/run/n20-radio-test/wifi`, and `/mnt/vendor/persist` to `/run/n20-radio-test/persist`. The vendor C.24 `ipa_fws.mdt` and `.b00`–`.b04` are in `/usr/lib/firmware`; its `WCNSS_qcom_cfg.ini` is present both under `wlan/` and `wlan/qca_cld/`. These vendor files are excluded from Git. Preserve them from verified matching C.24 sources or the existing installation; arbitrary firmware will not work. Re-extraction of the Android library set is not automated by this repository.

`n20-wifi-manager` uses `wpa_supplicant`, writes only a derived WPA2 key to root-only `/etc/n20-wpa.conf`, and supplies the dashboard via `/run/n20/wifi-state`. Its touchscreen UI supports open and WPA2-PSK networks, but not hidden SSIDs, WPA3-only, or enterprise authentication. `n20-wifi-dhcp` configures the lease and DNS. The timekeeper uses the PC USB helper if reachable, otherwise attempts NTP over Wi-Fi; the server waits for a credible date after clock loss.

## Backups and USB

`n20-save-events` watches the local Minecraft management API. `n20-admin` schedules low-priority Restic snapshots after ordinary save events and checks for quiet files; it does not force a server save for automatic backups. This reduces, but does not eliminate, inconsistent online copies. Restic history is local to `/srv/backups/world-history` and uses the separate `/etc/n20-backup.pass`; keep both off-device for disaster recovery. Automatic Google Drive replication is not configured. Previous preferences were direct phone-to-Drive incremental backups using the owner's Google Cloud project, without an additional encrypted cloud archive.

`n20-mtp-start` adds an MTP function to the existing USB ACM/NCM gadget and exports `/srv` read-only. File Explorer enumeration and a checksum-matched file copy were tested. Raw files copied while the server is changing the world may not form a consistent backup. The gadget setup may briefly reset USB networking; its fallback attempts to restore it.

## Rebuild the initramfs and boot image

The builder expects the exact Alpine aarch64 `busybox-static-1.37.0-r20.apk` SHA-256 `ee469aee2958feffd7f64dd96655704025ff60af614f77a1d7323dc237c34da2` and a C.24 `vendor_ramdisk` SHA-256 `8dd5faeefde663f7037540c07c3bce6a6e0452afdb3b995a08c2bc5b98c3eea9`. They are not in Git. With local copies:

```sh
python3 boot/build_initramfs.py --busybox-apk /path/to/busybox-static.apk \
  --vendor-ramdisk /path/to/vendor_ramdisk --output build/persistent-initramfs.cpio.gz
```

With the originally used inputs, the generated initramfs SHA-256 was `4b34faebfa1a4711e2766a16774a8a392e4a2c28ffb9b2f8dfb7b7d5db4745f0`. To reproduce the original boot image, use the matching C.24 kernel and AOSP `mkbootimg.py` at commit `d2bb0af5ba6d3198a3e99529c97eda1be0b5a093`:

```sh
python3 third_party/mkbootimg/mkbootimg.py --header_version 3 --os_version 12.0.0 \
  --os_patch_level 2024-10 --kernel /path/to/c24/kernel \
  --ramdisk build/persistent-initramfs.cpio.gz --output build/n20-boot.img
```

The previously installed image SHA-256 was `7f1791ec2faea02d2193657432f4c92d20dbd65477ba80b87032a28abd11c64f`. This describes the existing device, not an instruction to flash. A recovery path and full reinstall process are not verified.

The original workstation's exact inputs and a verified Windows rebuild command are described in [local materials](local-materials.md). The matching AOSP `mkbootimg.py` and `unpack_bootimg.py` are included under `third_party/mkbootimg/`.

The C.24 OTA used for research was [this OnePlus package](https://android.googleapis.com/packages/ota-api/package/c9df99a58a0a6b3d447f6703c33d424872d3778e.zip). Selected extracted components were checked against its payload manifest, which is a corruption check rather than independent package-signature verification. Third-party helper bases were Debian `rmtfs` 1.1-4, `protection-domain-mapper` 1.0-7, and `tqftpserv` 1.1-4 with device-specific modifications; their licenses are in their source directories.
