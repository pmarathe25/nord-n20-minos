# Nord N20 MinOS

Device-specific Linux and Minecraft server for the OnePlus Nord N20 (CPH2459). It uses the vendor's C.24 kernel and boot firmware, an ARM64 Debian root in a chroot, a DRM touchscreen dashboard, Wi-Fi, and USB file access. This is a working development snapshot of one phone, **not a general installer**.

The phone uses boot slot B and a reformatted userdata partition. Its bootloader is unlocked, verified boot checks were disabled for this setup, and a complete stock C.37 recovery image and tested recovery procedure were not found. Flashing a mismatched image or changing partition metadata can make the phone unbootable. Work on the running root filesystem does not require rebuilding or reflashing the boot image.

## Repository

- `runtime/` contains the current installed service scripts and MTP configuration, with their paths relative to the phone's Debian root. `minecraft-supervisor` starts them; this system does not use systemd as PID 1.
- `dashboard/` contains the DRM/FreeType touchscreen UI and framebuffer capture source.
- `radio/` and `third_party/` contain the Android compatibility stub, Qualcomm service sources, boot tools, and the BusyBox build input and source. Their original license files are retained.
- `boot/` contains the persistent initramfs init and a deterministic builder. Its filesystem UUID and USB configuration are specific to this device.
- `tools/` has non-installing builds, screen capture, and a Minecraft status probe; `tests/` exercises administration and backup behavior in temporary directories.
- [Device and development notes](docs/device.md) describe dependencies, firmware, deployment, boot-image inputs, and known limitations.
- [Battery baseline](docs/power-baseline.md) records the short unplugged measurement and its limitations.
- The tested phone stops charging at 55% and resumes at 50%; see [device notes](docs/device.md) for details and recovery.
- `local/` holds Git-ignored device inputs and credentials on the original workstation. [Local materials](docs/local-materials.md) lists its contents and explains how to reproduce public build inputs.

The working phone at capture time had Wi-Fi `10.0.0.160`; its dashboard displays the current DHCP address. Minecraft uses port 25565 and SSH port 22. USB networking uses `192.168.137.2`. On the original workstation the private key is in ignored `local/secrets/n20_usb_key`; from Windows:

```powershell
ssh -i .\local\secrets\n20_usb_key root@<phone-ip>
```

The example server configuration enables the whitelist and enforces it; add trusted Minecraft usernames before enabling it on an existing server. The supervisor requires the server executable and bundled libraries in root-controlled `/opt/n20-minecraft/current`; see [device notes](docs/device.md) before upgrading an existing installation. The dashboard's Phone > Wi-Fi page scans and connects to open and WPA2-PSK networks. The power button turns the display off or on. The dashboard powers the panel down when idle and wakes it with the power button; touch wake remains available as a fallback if panel power-down fails. After wake, inputs are ignored for 150 ms. Wi-Fi power saving is enabled when Minecraft has no TCP clients and disabled shortly after a connection appears; the first connection can have extra latency. The repository configures USB MTP to expose `/srv/minecraft/world` read-only as Minecraft World, rather than exposing server settings and backup files. Existing installations need the updated MTP configuration and a service restart for that restriction to take effect. Stop Minecraft before copying the live world for a consistent copy. The local Restic history also requires its separate password; neither it nor that password is included here. Direct Google Drive backups are not yet implemented.

The dashboard has three tabs: **Server**, **World**, and **Phone**. Server
contains status, the join address, start/stop/restart, players, game settings,
and activity. World opens directly to local backups and restore. Phone groups
Wi-Fi, charging status, brightness, and screen sleep. See the
[UI inventory and navigation](docs/ui.md) for the design and headless previews.

## Develop

On the phone, or another ARM64 Debian environment with the dependencies in [the device notes](docs/device.md):

```sh
sh tools/build-local.sh             # builds dashboard, capture, and MTP sandbox
sh tools/build-radio.sh             # produces open-source radio helpers; see prerequisites
sh tests/test-security-sysctls.sh   # isolated fake sysctl files
sudo python3 tests/test-live-private-paths.py --check-live  # read-only on the configured phone
cc -std=c11 -O2 -Wall -Wextra -Werror -I/usr/include/libdrm -I/usr/include/freetype2 \
  tests/test-dashboard.c -o /tmp/n20-dashboard-test -ldrm -lfreetype -lm
/tmp/n20-dashboard-test
cc -D_GNU_SOURCE -std=c11 -O2 -Wall -Wextra -Werror -I/usr/include/libdrm \
  tests/test-capture.c -o /tmp/n20-capture-test -ldrm
/tmp/n20-capture-test
python3 tests/test-mtp-sandbox.py build/n20-mtp-sandbox  # root; isolated mount namespace
python3 tests/test-save-events.py
python3 tests/test-ssh-policy.py  # requires OpenSSH server
python3 tests/test-wifi-security.py
python3 tests/test-wifi-events.py
python3 tests/test-wifi-integration.py
python3 tests/test-wifi-power-policy.py
python3 tests/test-wifi-power-integration.py
python3 tests/test-timekeeper.py
sh tests/test-supervisor-server-only.sh  # root; isolated mount namespace
sh tests/test-supervisor-restore-hold.sh  # root; isolated mount namespace
sh tests/test-supervisor-runtime-directory.sh  # root; isolated mount namespace
python3 tests/test-admin-settings.py
python3 tests/test-powerguard.py
python3 tests/test-autosave-scheduler.py
python3 tests/test-backup-world-guard.py  # root; isolated mount namespace
python3 tests/test-backup-restore.py
python3 tests/test-restore-area.py  # root
```

The backup tests use temporary directories and an isolated Restic repository; they require `restic`, `rsync`, `nice`, and `ionice`. They do not test real gameplay or guarantee atomic snapshots of a changing world. Run `/tmp/n20-dashboard-test --benchmark` for an isolated renderer comparison. For an optional disposable server heap comparison, run `python3 tools/benchmark-server-memory.py --jar /opt/n20-minecraft/current/server.jar --eula /srv/minecraft/eula.txt` as root; it temporarily increases device load. Set `N20_UI_METRICS=1` when launching the dashboard to log aggregate rendering time, event-to-frame-submission time, and peak RSS; neither measures whole-phone power.

For a read-only battery log on the phone, run `python3 tools/measure-battery.py --duration 600 --interval 10 > /tmp/n20-battery.csv`. Compare unplugged runs with similar workloads and display states; the vendor current counter is not calibrated. See [the power baseline](docs/power-baseline.md) for measurements, limitations, and the current display-wake constraint.

For a synthetic active-world heap comparison, run `python3 tools/benchmark-server-workload.py --jar /opt/n20-minecraft/current/server.jar --eula /srv/minecraft/eula.txt` as root. It creates disposable loopback-only worlds and temporarily adds substantial load; `--reverse` swaps the heap test order. Its results do not establish real-play latency or battery life.

From Windows, build the dashboard on the existing phone without installing it:

```powershell
.\tools\build-dashboard.ps1 -Phone <phone-ip> -IdentityFile .\local\secrets\n20_usb_key
```

Add `-Install` to replace the installed dashboard binary; it starts on the next phone boot. Capture the current framebuffer with `tools/capture-screen.ps1` and the same arguments. Review the changes and avoid overwriting the phone's credentials, configuration, or world data when deploying other components.

Git excludes vendor firmware and binaries, boot images, credentials, world data, and backup keys. The original workstation keeps necessary vendor inputs and private device files in restricted `local/`; the world and backup repository stay on the phone. Git LFS stores the redistributable BusyBox build input and source; run `git lfs pull` after cloning. See [local materials](docs/local-materials.md) and [device notes](docs/device.md) before attempting a rebuild or deployment.
