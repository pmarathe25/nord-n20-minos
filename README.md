# Nord N20 MinOS

Device-specific Linux and Minecraft server for the OnePlus Nord N20 (CPH2459). It uses the vendor's C.24 kernel and boot firmware, an ARM64 Debian root in a chroot, a DRM touchscreen dashboard, Wi-Fi, and USB file access. This is a working development snapshot of one phone, **not a general installer**.

The phone uses boot slot B and a reformatted userdata partition. Its bootloader is unlocked, verified boot checks were disabled for this setup, and a complete stock C.37 recovery image and tested recovery procedure were not found. Flashing a mismatched image or changing partition metadata can make the phone unbootable. Work on the running root filesystem does not require rebuilding or reflashing the boot image.

## Repository

- `runtime/` contains the current installed service scripts and MTP configuration, with their paths relative to the phone's Debian root. `minecraft-supervisor` starts them; this system does not use systemd as PID 1.
- `dashboard/` contains the DRM/FreeType touchscreen UI and framebuffer capture source.
- `radio/` and `third_party/` contain the custom Android compatibility stub and modified Qualcomm service sources. Their original license files are retained.
- `boot/` contains the persistent initramfs init and a deterministic builder. Its filesystem UUID and USB configuration are specific to this device.
- `tools/` has non-installing builds, screen capture, and a Minecraft status probe; `tests/` exercises administration and backup behavior in temporary directories.
- [Device and development notes](docs/device.md) describe dependencies, firmware, deployment, boot-image inputs, and known limitations.

The working phone at capture time had Wi-Fi `10.0.0.160`; its dashboard displays the current DHCP address. Minecraft uses port 25565 and SSH port 22. USB networking uses `192.168.137.2`. From Windows with the existing private key kept outside this repository:

```powershell
ssh -i <path-to-private-key> root@<phone-ip>
```

The dashboard's Settings > Wi-Fi page scans and connects to open and WPA2-PSK networks. The screen wakes on touch. With USB connected, File Explorer > This PC > Minecraft Server > Minecraft Server Files exposes `/srv` read-only. Stop Minecraft before copying the live world for a consistent copy. The local Restic history also requires its separate password; neither it nor that password is included here. Direct Google Drive backups are not yet implemented.

## Develop

On the phone, or another ARM64 Debian environment with the dependencies in [the device notes](docs/device.md):

```sh
sh tools/build-local.sh             # produces build/n20-dashboard and build/n20-capture-screen
sh tools/build-radio.sh             # produces open-source radio helpers; see prerequisites
python3 tests/test-admin-settings.py
python3 tests/test-powerguard.py
python3 tests/test-autosave-scheduler.py
python3 tests/test-backup-restore.py
```

The tests use temporary directories and an isolated Restic repository; they require `restic`, `rsync`, `nice`, and `ionice`. They do not test real gameplay or guarantee atomic snapshots of a changing world.

From Windows, build the dashboard on the existing phone without installing it:

```powershell
.\tools\build-dashboard.ps1 -Phone <phone-ip> -IdentityFile <path-to-private-key>
```

Add `-Install` to replace the installed dashboard binary; it starts on the next phone boot. Capture the current framebuffer with `tools/capture-screen.ps1` and the same arguments. Review the changes and avoid overwriting the phone's credentials, configuration, or world data when deploying other components.

This repository excludes firmware and vendor binaries, boot images, Wi-Fi and SSH credentials, management secrets, world data, and backup keys. Keep them separately and do not commit them. See [device notes](docs/device.md) before attempting a rebuild or deployment.
