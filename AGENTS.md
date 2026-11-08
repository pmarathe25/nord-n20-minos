# Agent instructions

- Read `README.md` and `docs/device.md` before changing device behavior. This is an experimental installation on a specific OnePlus Nord N20, not a generic Linux distribution.
- `runtime/` mirrors installed scripts and configuration; `dashboard/` contains the touchscreen and capture code; `boot/` builds the initramfs; `radio/` and `third_party/` contain radio helper sources. Preserve third-party license notices.
- Build with `sh tools/build-local.sh` and `sh tools/build-radio.sh` in an ARM64 Debian environment. Run the relevant `python3 tests/test-*.py` scripts individually; backup tests need Restic and rsync and operate in temporary directories. Check shell scripts with `sh -n` and Python files with `python3 -m py_compile` when changed.
- Firmware, signed modules, boot images, credentials, Wi-Fi settings, server configuration, world data, backups, and recovery material stay outside Git. Do not print or commit their contents. Keep existing device state and secrets intact when testing or deploying.
- The boot image, UUID, radio partition names, and firmware are device-specific. Do not flash images, modify partition metadata, or replace firmware as a routine build or test. The device lacks a verified full stock recovery path. Builds should not deploy automatically.
- Automatic world snapshots do not force saves and are not atomic filesystem snapshots. Preserve backup and restore safeguards, and test changed recovery behavior with isolated temporary data.
