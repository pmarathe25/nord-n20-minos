# Device and development notes

## Current architecture

The tested phone is CPH2459 with an unlocked bootloader. Slot B combines C.24 `vendor_boot`, `dtbo`, `vbmeta`, and a custom boot image containing kernel `5.4.254-qgki-g93bb89e5a062`. The kernel starts BusyBox `/init`, mounts the ext4 userdata volume and bind-mounts `/dev`, `/proc`, and `/sys` into the Debian root. The UUID in `boot/persistent-init` is the existing phone's UUID. Do not substitute another device or assume this image is portable. `fastboot boot` was not supported by this phone.

While USB or AC power is reported present, `n20-admin powerguard` stops charging at 55% and
permits it again at 50%; between those thresholds it retains the current state.
It checks approximately every 15 seconds. Unplugging or an invalid capacity
reading causes it to permit charging. The supervisor restarts the guard after
an unexpected exit and permits charging during the restart. This uses the
device-specific `/sys/class/oplus_chg/battery/mmi_charging_enable` control,
not a generic power-supply interface. On this phone at 99% with USB connected,
disabling it changed the battery status from `Charging` to `Not charging`;
reenabling restored `Charging`. Guard restart and the active limit after a
reboot were verified. The 50% restart threshold and invalid-reading handling
were tested with temporary files, not by discharging the physical battery to
those levels. USB and AC flag combinations were tested with temporary files;
the physical test used a workstation USB connection, not a wall charger, whose
flag behavior when charging is paused remains unverified. A 15-second control interval can overshoot the target, and the
reported status alone does not prove true battery bypass or quantify aging.
If the guard and its supervisor both stop, the last switch state may persist;
restore charging with `printf '1\n' > /sys/class/oplus_chg/battery/mmi_charging_enable`
as root, or reboot. Do not substitute a different device's control path.

`minecraft-supervisor` starts the radio, Wi-Fi manager, MTP, timekeeper, SSH, dashboard support, backup scheduler, and Minecraft. Java runs as `minecraft` using `/opt/java`; the tested installation had Java 25 and Minecraft Java 26.3. Wi-Fi is normally the preferred route. USB serial provides a root shell and is not password-protected. SSH requires public-key authentication, permits only the root account, and rejects passwords and keyboard-interactive authentication. It limits authentication attempts and concurrent unauthenticated connections. The original workstation keeps the SSH key at the ignored `local/secrets/n20_usb_key` (with its public key beside it); clones do not include it. Protect the private key and physical access.

On 2026-11-09 UTC, Debian's security updates for `openssl` and
`ca-certificates` were installed without removing packages or replacing local
configuration. The resulting versions were `3.5.7-1~deb13u3` and
`20250419+deb13u1`; `libssl3t64` was already at `3.5.7-1~deb13u3`. Debian
identifies the previous source-package versions as vulnerable in
[DSA-6531-1](https://security-tracker.debian.org/tracker/DSA-6531-1) and
[DSA-6572-1](https://security-tracker.debian.org/tracker/DSA-6572-1).
After the update, a fresh NTS synchronization succeeded and Minecraft was
online with its whitelist enabled. This is a point-in-time package check, not
an automated patching policy or a complete audit of all installed software.

The separately installed Java runtime reported Eclipse Temurin 25.0.5+8,
which [Adoptium lists](https://adoptium.net/en-GB/news/2026/10/temurin-october-2026-emudhra)
as containing its October 2026 eMudhra trust-store fix. A `keytool` listing
did not contain the two affected roots named in that advisory. As an additional
check, OpenSSL using certificates exported from the Java trust store accepted
an ordinary Adoptium TLS connection and rejected four published compromised
TLS test chains with certificate-verification errors. This checks the exported
trust anchors and those chains. A subsequent direct test using the installed
Java runtime accepted an ordinary Adoptium TLS connection and rejected all 14
published TLS test chains with certificate-path validation errors. The test class
was compiled with a Debian compiler extracted under `/tmp`; no compiler package
or Java runtime files were installed or changed. The test does not prove
signed-code handling or establish protection against unrelated certificate issues.
The same Java trust store and PKIX path builder subsequently accepted a normal
certificate chain and rejected both offline key-rollover chains supplied in
the [incident test bundle](https://emudhra.we-have-a-problem.com/). Revocation
checking was disabled for that offline test; it tested trust-path construction,
not signed JAR or other signed-code verification.

The Minecraft account owns `/srv/minecraft`, so its entries remain untrusted
to root-run helpers. Administration reads `server.properties` and
`whitelist.json` only as bounded, single-link regular files without following
symlinks; invalid files are rejected or reported unavailable. The save-event
monitor and Wi-Fi power policy apply the same checks to `server.properties`.
The save-event monitor bounds handshake and partial-message reads while waiting
without a periodic socket timeout when idle. Management notifications and
server-status queries reject excessively nested JSON; status queries also use
an overall response deadline. The status probe reads the configured game port
and requires the recorded Java process to own a listener on that port before
querying it. If the port changes while the old server is still running, status
and idle-dependent actions remain unavailable until a matching listener is
present. The supervisor
launches `n20-security-sysctls` before services to enable the kernel's
`protected_hardlinks`, `protected_symlinks`, `protected_fifos`, and
`protected_regular` controls. On the tested phone a disposable unprivileged
hardlink to a root-only file succeeded before this change and was denied
afterward. Install the helper alongside the supervisor; without it, normal
service startup stops rather than proceeding without these settings. A normal
reboot on the tested phone restored all four settings before services started;
after the clock was corrected over SSH, NTS synchronized and Minecraft came
online with its whitelist enforced. The isolated supervisor test runs the real
helper against fake controls and verifies that services do not start if the
helper is missing or a protection check fails.

The live installation also had world-writable `/`, `/usr`, `/usr/local`,
`/usr/local/sbin`, the local CA directory, and several Android radio shared
libraries. Their group/world write bits were removed. The security helper now
checks critical root-owned parents and removes unsafe write bits from the
root-owned local executables, device-specific radio libraries, and local CA
files before starting services. It refuses startup if those protected trees
contain entries owned by another user. After a
normal reboot, those permissions and kernel protections remained active,
Wi-Fi returned, and Minecraft started after trusted clock recovery. A comparison
of installed scripts under `/usr/local/sbin` against the checkout found only a
blank-line difference in the Wi-Fi manager. This is not an integrity audit of
the vendor libraries or proof that the prior writable paths were never abused;
those binaries require separately trusted matching references for verification.

A later comparison of the complete `runtime/` file set against installed paths
again found only that blank-line difference; the MTP sandbox C source is a
build input and is not installed at its source path. The dashboard built from
the current sources matched both the installed and running executable. This
comparison establishes correspondence to the local checkout, not independent
integrity of the checkout, compiler, or vendor components.

On 2026-11-09 UTC, the installed Minecraft server JAR identified itself as
26.3 and its SHA-1 matched the server download hash in Mojang's
[version manifest](https://piston-meta.mojang.com/mc/game/version_manifest_v2.json)
and linked version metadata; the version metadata also matched its manifest
hash. The manifest listed 26.3 as the latest release at that time. Running as
the Minecraft account, a further check matched the bundled SHA-256 entries for
the extracted server JAR and all 39 listed libraries. These are point-in-time
checks, not proof of the code previously loaded by a running process. Those
original files remain game-owned but are no longer used by the supervisor.

The startup helper was later extended to reject links that escape the protected
local executable, radio-library, and certificate directories. The phone's
existing same-directory library aliases pass that check. Following a normal
reboot with the updated helper installed, all four filesystem protections were
active, Wi-Fi and the dashboard started, and Minecraft accepted a status query.
The clock again needed correction over authenticated SSH before NTS could
synchronize; idle Wi-Fi power saving and the charging limit resumed.
After that reboot, a listener inventory showed only SSH on port 22, Minecraft
on port 25565, and Minecraft management on the IPv4 loopback address at port
25585; it showed no UDP listeners. This is a point-in-time inventory, not a
firewall or a guarantee about future services.
The same startup checks now cover the Java `bin`, `lib`, and `conf` trees.
A live scan found no non-root-owned or group/world-writable entries in
`/opt/java` when the change was installed. The expanded check passed against the
running phone and in isolated startup tests. On a subsequent normal reboot the
dashboard and Minecraft started, the four filesystem protections were active,
and the charging limit and idle Wi-Fi power saving resumed. The clock again
needed correction over authenticated SSH before NTS synchronized.

A subsequent read-only scan found no group- or world-writable regular files or
directories and no Minecraft-owned entries under `/usr/lib` or `/usr/share`.
Key system-library and Python-module parent directories are now also checked at
service startup. This scan is a point-in-time permissions check, not an audit of
all packages or their contents.

A later point-in-time check found no group/world-writable regular files or
directories under `/var/lib/n20`, `/etc`, or `/run/n20`. Under `/var/log`, the
group-writable login-accounting files belonged to `root:utmp`; the Minecraft
account was not in that group and a direct access check could not write them.
`/tmp` was root-owned with sticky mode `1777`. These checks do not establish
the integrity of existing log contents.

An additional phone-side comparison found that the installed dashboard, MTP
sandbox, `pd-mapper-n20` and `rmtfs-n20` were byte-identical to recent builds
from the checkout, and their running executables matched the installed files.
The running `tqftpserv-n20` matched its installed file but not the recent build:
the default rebuild linked zstd statically, while the installed program uses a
shared library. A shared-zstd rebuild also differed, so its provenance remains
unconfirmed. A further shared-zstd C23 build using the source-file order
`translate.c`, `tqftpserv.c`, `zstd-decompress.c` produced identical read-only
data and differed in eight `.text` bytes, all in `main`; the differing
instructions use different stack slots and register assignments. This narrows
the discrepancy but does not prove semantic equivalence. These builds used the
phone's own compiler and libraries and are
not an independent integrity verification of the toolchain or vendor binaries.

The dashboard has four direct tabs: Server, Players, World, and Phone. Server
contains status, the join address, lifecycle controls, inline game settings,
and a Log button. Players opens the whitelist, with an Online view alongside.
World opens backup history and restore. Phone combines Wi-Fi, brightness and
screen sleep. A persistent top bar shows battery percentage, charging, Wi-Fi
connection state, and battery temperature on every page and form. Its reads
reuse the existing awake cadence and stop while the screen is off. Only changed
indicator rows repaint on otherwise static pages. Existing display power and
Wi-Fi power policy behavior is unchanged.
Confirmation buttons recheck current data and, for stop/restart, the server
process identity. On Wi-Fi send failure the password form stays open.
On 2026-11-09, the redesigned UI was built and tested on ARM64, then installed
by replacing and restarting only the dashboard. Minecraft's process identity,
display preferences, and Wi-Fi power-policy processes were unchanged. Injected
power-button and touch events exercised wake, the main tabs, and sleep;
framebuffer checks verified the selected tabs. The screen was returned to off.
A subsequent 15-second sample recorded no dashboard CPU ticks or context
switches, and Wi-Fi reported power saving on. This is a short activity check,
not a whole-device power measurement. Earlier live observations describe the
previous UI. See [UI design and validation](ui.md) for navigation and fixtures.

A live listener check found Minecraft's management port bound to the IPv4
loopback address, with RCON and query disabled and online authentication and
whitelist enforcement enabled. From the workstation, the game port was
reachable over Wi-Fi while the management port was unavailable. This is a
point-in-time check, not a firewall guarantee or a test from every network.

The supervisor launches Java with an empty capability bounding set and
`no_new_privs`, so the game cannot gain privilege via setuid executables or
file capabilities. An already-running supervisor continues using its old
launch command until that supervisor is restarted. To exercise the boundary
without touching the live world, run as root:

```sh
python3 tests/test-live-private-paths.py --check-live
python3 tests/test-server-privileges.py --jar /opt/n20-minecraft/current/server.jar \
  --eula /srv/minecraft/eula.txt
python3 tests/test-protected-server.py --jar /opt/n20-minecraft/current/server.jar \
  --eula /srv/minecraft/eula.txt
```

This starts a second, loopback-only server with a disposable world and can
temporarily increase CPU, memory and storage load. It requires an EULA the
operator has already accepted.

`tools/prepare-protected-server.py` prepares a new root-controlled server JAR
and bundler repository from a supplied JAR and expected SHA-256. It refuses to
overwrite an existing output and verifies each extracted bundle entry. Obtain
and verify the expected digest independently; a digest calculated solely from
an untrusted JAR does not establish publisher authenticity. The protected
server test checks permissions and starts a disposable server from the prepared
bundle. Preparation alone does not change the live supervisor or server.

For a vanilla Mojang release, `python3 tools/verify-mojang-server.py --jar
<path-to-jar> --version <version>` checks the version metadata and downloads
the complete server JAR over HTTPS from Mojang's pinned hostnames. It verifies
the publisher's size and SHA-1 metadata, then compares SHA-256 and size against
the local file and prints the SHA-256 for the preparation command. It does not
install the JAR. On the tested phone, this full-download comparison passed for
the protected 26.3 JAR. This relies on the system HTTPS trust store and Mojang's
metadata; it is not a publisher signature or a verification of third-party mods.

The live supervisor now requires `/opt/n20-minecraft/current/server.jar` and
`/opt/n20-minecraft/current/repo`, and launches Java with the latter as
`bundlerRepoDir`. On the tested phone, `current` is a root-owned relative
symlink to `bundle-26.3`. The bundle was prepared from the JAR after checking
its SHA-1 against Mojang's HTTPS version metadata, then checking its SHA-256
again during preparation. The normal reboot succeeded, the live Java command
used those protected paths, the Minecraft account could not write the bundle or
its parent, and the game remained online with its whitelist enabled. The
original JAR and extracted libraries under `/srv/minecraft` were retained.
The account still owns its world and configuration, which remain untrusted.
The startup security helper also checks ownership, write permissions and links
in the protected bundle tree. If the bundle is missing, the dashboard reports
that server files are unavailable instead of offering Start.
In a later live process snapshot, all 42 open JAR file descriptors in the
Minecraft JVM pointed into `/opt/n20-minecraft`; none pointed into the
game-owned server directory or elsewhere. This is a point-in-time descriptor
check, not proof of every class already loaded or of future behavior.
The live private-path test also denied the Minecraft account access to the
Wi-Fi configuration, supplicant control directory, SSH host private keys,
backup secrets, and administration directories, and denied write access to
the protected executable bundle. This is a point-in-time permission check.
The supervisor reads the game-owned EULA file as the Minecraft account with a
two-second timeout, rather than following it as root. Isolated tests cover a
root-only symlink target, a FIFO, the missing-bundle case, and recovery holds;
a subsequent normal reboot started the protected server successfully.

Before installing this supervisor on another existing installation, create a
root-owned mode-755 `/opt/n20-minecraft`, prepare a verified JAR into a new
versioned directory there, and create the root-owned relative `current` symlink
to that directory. Install the updated `n20-security-sysctls` alongside the
supervisor. Otherwise the supervisor deliberately leaves Minecraft stopped;
it does not fall back to the writable old JAR. For a future update, verify the
new publisher artifact, prepare a separate versioned directory, stop the
server cleanly, then replace `current` atomically with a root-owned relative
symlink to the new directory. Keep the old bundle for rollback and test the new
server before discarding it. Do not modify the live world or configuration as
part of bundle preparation.

The USB serial root shell and emergency shell are recovery paths, not authenticated interfaces; anyone with physical USB access can obtain root. The bootloader is unlocked and verified boot is disabled. Do not expose the phone or its USB port to untrusted physical access. Replacing those recovery paths requires a separately verified recovery plan and a device-specific boot-image change.

`config/server.properties.example` records the relevant non-secret Minecraft settings only. The management API secret must be a separately generated 40-character alphanumeric value, and the live `server.properties` must remain protected and uncommitted. Management traffic is unencrypted and must remain bound to loopback. The installed key-only SSH configuration is in `runtime/etc/ssh/sshd_config_n20`; authorized keys are device-specific and excluded.

For a new server with no `server.properties`, the supervisor seeds online authentication and an enforced whitelist before first launch. It does not overwrite an existing server configuration or player list. Add trusted accounts before enabling the whitelist on an existing server; an empty whitelist prevents every player from joining. The game process owns its working directory so it can update its own files; removing group and other write access does not protect those files from a compromised game process.

On 2026-11-08, a read-only check of the running phone found online mode,
whitelist and whitelist enforcement enabled, the management listener bound to
loopback, key-only SSH enabled, and the Java process running as the Minecraft
user with an empty capability bounding set and `no_new_privs`. This is a
point-in-time check, not a guarantee about later configuration changes.

The updated dashboard uses the power button to wake the panel after powering it down. It reopens the DRM connection to make the panel visible again; touch does not wake a powered-down panel. If power-down fails, the dashboard falls back to zero brightness with touch wake. In either case a 150 ms guard follows display wake. This version was installed and physically tested on the device; see [the power baseline](power-baseline.md). Power usage, OLED wear, multiplayer load, and fully depleted-battery startup have not been characterized. A normal reboot restored Wi-Fi, the server and USB MTP. On unplugged reboots Wi-Fi and the dashboard returned, but the clock required operator correction before the server could start; see the timekeeper notes below. Wi-Fi internet, DNS and NTP also worked with USB networking temporarily disabled.

## Development dependencies

The phone uses Debian 13 arm64. Build the dashboard and MTP sandbox with `gcc`, `libdrm-dev`, and `libfreetype-dev`. The radio helpers require `gcc`, `libqrtr-dev`, `libudev-dev`, and `libzstd-dev`. `QRTR_PREFIX` can point `tools/build-radio.sh` at an extracted development sysroot, for example `QRTR_PREFIX=/tmp/n20-qcom-tools/usr sh tools/build-radio.sh /tmp/n20-radio-build`; the script also requires the system `libudev.so.1`. `restic`, `rsync`, `nice` and `ionice` are needed for the isolated backup tests. The runtime also needs `wpasupplicant`, `iw`, the patched uMTP responder described below, OpenSSH, BusyBox, Python 3, the Java runtime, `chrony`, and the system CA certificates.

The preserved `pd-mapper` source checks firmware-path read errors and rejects
potentially truncated input before indexing the buffer; it also skips firmware
directories that cannot be opened. The corrected radio helper builds on the
phone and was staged at its installed path, retaining the previous binary as
`pd-mapper-n20.before-read-fix` beside it. The tested phone was subsequently
rebooted and now runs the updated helper. Device firmware maps loaded successfully in a read-only
test, including checks that their domain responses fit the response buffers.
The helper rejects excessive domain counts and overlong names. After a normal
reboot the hardened helper published the live service registry and answered a
read-only QMI query; Wi-Fi reconnected. This does not exercise every service
registry response.
Its bundled map parser also bounds file size, checks complete reads, releases
the input buffer, and rejects truncated or overlong strings. Focused tests and
undefined-behavior sanitizer checks pass with the device's maps. AddressSanitizer
could not initialize its shadow address space on this phone, so that check is
not available from the on-device run. The same undefined-behavior check also
passed a deterministic 20,000-input malformed-JSON smoke test; this is not an
exhaustive fuzzing campaign. A temporary, non-installed Valgrind setup also
passed the focused tests, device-map loading and the malformed-input smoke test
with memory-error checking enabled and leak checking disabled. It does not
establish that the helper is free of memory-safety defects.
To test the firmware-path read guard without running a radio service, compile
`tests/test-pd-mapper-read.c` with `assoc.c`, `json.c`, `servreg_loc.c` and
`libqrtr`, using the same QRTR include and library paths as the radio build.
`tests/test-pd-mapper-live.c` sends an empty-result lookup to a running QRTR
service-registry endpoint; it does not modify modem state.

The tested installation has a diverted `/usr/bin/kmscon`; `runtime/usr/bin/kmscon` launches the dashboard and falls back to `/usr/bin/kmscon.distrib`. Copying a launcher into a new installation is not enough: the diversion, display permissions, device nodes, firmware, server user, Java, SSH configuration, keys, and actual server setup also need to exist. This repository does not automate a fresh root filesystem or image installation.

The admin tests change their module's paths to disposable directories under `/tmp`; snapshot and restore tests create their own temporary Restic repositories. The tests do not alter the live world. Run them from a Unix-like environment: `fcntl`, Linux paths, and the external tools above are required.

## Wi-Fi implementation

The C.24 kernel enforces module signatures. Its matching signed `qca_cld3_wlan.ko` has SHA-256 `f52a095b045c6163c2b682973b31a473f0152d04fe8f7366b1becb244faaae00`. `n20-radio-start` starts patched `rmtfs`, `pd-mapper`, and `tqftpserv`, holds the modem open, starts the C.24 `cnss-daemon` with an Android compatibility library set and the included `cnss-pm-stub.c`, waits for firmware, then loads the signed WLAN module. The EFS and calibration partitions are copied to private temporary memory and served through read-only/shadow rmtfs; the original partitions are not intentionally written. The hard-coded partition names and sysfs paths are device-specific.

The existing installation stores the built helpers and extracted vendor files in `/usr/local/lib/n20-radio`, including the signed module, a `qrtr/` library directory, and `android/` with `cnss-daemon`, `linker64`, Android shared libraries and the built PM stub. It mounts the stock `modem_b` partition read-only at `/mnt/n20-modem`; `/vendor/firmware_mnt/image` points to `/mnt/n20-modem/image`, `/data/vendor/wifi` to `/run/n20-radio-test/wifi`, and `/mnt/vendor/persist` to `/run/n20-radio-test/persist`. The vendor C.24 `ipa_fws.mdt` and `.b00`–`.b04` are in `/usr/lib/firmware`; its `WCNSS_qcom_cfg.ini` is present both under `wlan/` and `wlan/qca_cld/`. These vendor files are excluded from Git. Preserve them from verified matching C.24 sources or the existing installation; arbitrary firmware will not work. Re-extraction of the Android library set is not automated by this repository.

`n20-wifi-manager` uses `wpa_supplicant`, writes only a derived WPA2 key to root-only `/etc/n20-wpa.conf`, and supplies the dashboard via `/run/n20/wifi-state`. Newly added WPA2 networks require RSN and CCMP. Its touchscreen UI supports open and WPA2-PSK networks, but not hidden SSIDs, WPA3-only, or enterprise authentication. The password keyboard starts masked, provides Show/Hide and cursor controls, and rejects non-hexadecimal 64-character keys. The manager subscribes to supplicant events and waits for touchscreen commands, with a 15-second fallback to detect DHCP failure and recover from missed events; when an event subscription is unavailable, it retries every 3 seconds. `n20-wifi-dhcp` configures the lease and DNS.

`n20-wifi-power-policy` watches local TCP sockets on the configured Minecraft
port. It confirms that the server's Java process owns the listener, enables
Wi-Fi power saving after ten seconds without client sockets, and switches it
off when a client appears or the server state cannot be verified. Its worker
checks every half second. An independent parent detects a stopped or stalled
worker and restores low-latency mode; a worker also restores that mode if its
parent disappears. This is not instantaneous: the first connection can incur
power-save latency and the switch can take a polling interval plus processing.
Other TCP clients, including status probes, conservatively keep power saving
off. Driver failures or loss of the interface can prevent a requested change;
the policy retries. The policy is separate from the Wi-Fi manager and does not
use the potentially stale player-list cache. The Wi-Fi page shows whether the
automatic policy's heartbeat is current; it does not report the radio's
instantaneous power-save state.

The timekeeper uses one-shot Chrony NTS over Wi-Fi, with no listening NTP or command ports and no unauthenticated Wi-Fi NTP fallback. It trusts only the pinned SSL.com ECC root and always checks certificate validity dates. Successful NTS synchronization persists a root-only, nondecreasing trusted-time floor in `/var/lib/n20/trusted-time`; a clock more than a day behind that floor is treated as invalid. When the clock is invalid, the timekeeper tries the USB time-recovery path, restricted to a route through `usb0` and not allowed to roll back the trusted floor. The USB time service is unauthenticated, and the USB port already grants an unauthenticated root shell; use a trusted computer and protect physical access. If the clock is too wrong for NTS certificate validation and USB time recovery is unavailable, Minecraft waits for a credible clock instead of relaxing certificate checks. On the original Windows workstation the optional helper is `tools/start-pc-helpers.ps1` with its Python executable path; see [local materials](local-materials.md). An authorized root operator can also correct the time manually. A CA change can stop synchronization until the trust anchor is updated; a corrupt or erroneously future trusted-time floor may require root recovery. A full-battery-depletion recovery has not been physically tested.

If a successful NTS correction places the clock outside the accepted bounds, the timekeeper rejects it and attempts to restore the previous clock, even when that previous value was invalid. The stored floor is not treated as the current time. The correction may have briefly affected the system clock before this check. An already-running timekeeper must be restarted to load an updated script.

On 2026-11-08, the phone was connected to the original workstation and a
read-only check confirmed USB SSH access, a route to the workstation through
`usb0`, and a time-service response within ten seconds of the phone's clock.
The test did not change the clock or simulate a depleted battery.

An unplugged reboot on the tested phone exposed a separate clock limitation: the
hardware RTC returned to January 1970 despite a full battery, so Wi-Fi and the
dashboard came up but Minecraft waited for trusted time. The RTC device reported
`EPERM` when root attempted to set it; its write restriction has not been
resolved. An authorized operator set the clock over the existing key-authenticated
Wi-Fi SSH connection, after which NTS synchronized and Minecraft started with
the whitelist enforced. Thus USB is not necessary for normal operation or Wi-Fi
SSH recovery, but unattended restart currently cannot be guaranteed. USB serial
and the trusted USB time helper remain recovery options if Wi-Fi is unavailable.
The MTP service also started while disconnected, and idle Wi-Fi power saving
returned to on; this does not establish the USB hardware's power consumption.

In a second unplugged reboot, the raw RTC seconds counter advanced from
113330 before reboot to 113381 after boot, even though the system clock again
started in 1970. This suggests that an offset saved after authenticated time
synchronization could support ordinary reboots. It does not establish how to
detect every counter reset or loss of RTC power, so no automatic offset recovery
is enabled: accepting an undetected reset could give certificate validation an
incorrect date. The upstream Linux pm8xxx RTC driver supports persisted offsets
on some configurations, but that does not establish support in this vendor
kernel or authorization to change the boot image.

An experimental alternative is Roughtime, which authenticates a fresh signed
time response using a pinned server key without consulting the local clock.
A read-only trial on the phone used `pyroughtime` commit
`f70cd0c6a53d0bf5bc87d7c1ddfbdc00be867b08` and an extracted Debian
`python3-pycryptodome` package in `/tmp`. The test obtained a signed response
from `time.txryan.com:2002`, rejected a replay because its nonce differed,
and rejected a response whose signature was modified. A second listed server,
`roughtime.se:2002`, timed out. The trial did not set the phone clock or install
either component. These tests are not a security audit or proof of long-term
availability: selecting and maintaining independent trust anchors, validating
malformed responses and actual boot integration, and testing fail-closed USB
recovery still remain before this could replace the current recovery policy.
`tests/test-roughtime-auth.py` now repeats replay, modified response signature,
and modified delegation-signature checks against an ephemeral loopback server;
it requires `python3-pycryptodome` and never changes the clock.

The repository now includes an experimental read-only probe in
`tools/roughtime-probe.py`, plus the upstream GPL-licensed client and its
license in `third_party/pyroughtime`. It requires Debian's
`python3-pycryptodome`. It pins keys published by the operators of
`time.txryan.com` and `rough.time.nl`, requires both signed responses to agree,
and rejects overly uncertain or stale samples. On the phone both servers
responded and the agreement check succeeded; isolated tests cover disagreement,
missing samples, excessive uncertainty, stale data, out-of-range dates, an
unsupported or inconsistent response version, and a zero uncertainty radius.
Use an external timeout when running this trial; it does not set the clock and
is not part of boot or the production timekeeper. Key rotation and changes in
server availability still require operator attention.

## Backups and USB

`n20-save-events` watches the local Minecraft management API. `n20-admin` schedules low-priority Restic snapshots after ordinary save events and checks for quiet files; it does not force a server save for automatic backups. This reduces, but does not eliminate, inconsistent online copies. The quiet-file check includes directory entries, symlinks, permissions and change timestamps as well as file sizes and modification times. An updated signature format can cause one additional snapshot after upgrade. The copier pins the world root directory, rejects a symlink at that path, and aborts if the root is replaced during copying. The copy runs as the Minecraft account without capabilities in a private mount namespace; it cannot copy files the account cannot read. The pre-restore checkpoint uses the same copy path. This requires `/usr/local/sbin/n20-backup-copy`, `unshare`, `setpriv` and mount-namespace support; if they are unavailable, the backup fails rather than falling back to a root copy. A compromised game process can still alter the world data it owns. A read-only phone check on 2026-11-08 found no unreadable regular files, unsearchable directories, or nested mounts in the current world; it did not create a live backup. Restic history is local to `/srv/backups/world-history` and uses the separate `/etc/n20-backup.pass`; keep both off-device for disaster recovery. Automatic Google Drive replication is not configured. Restore staging and retained pre-restore worlds are kept under root-only `/srv/backups/.n20-restore` on the same filesystem as the world; keep retained worlds until their recovery value has been assessed. New restore holds are recorded in root-only `/var/lib/n20/restore-hold`; the supervisor also honors a legacy `/srv/minecraft/.n20-restore-hold` until an operator resolves it. If either exists after an interruption, retain the hold and inspect its recorded recovery paths before resuming the server. The low-battery hold is likewise kept in `/var/lib/n20/low-battery-hold`; a legacy hold in the server directory is still honored and cleared only when the charging threshold is met. Previous preferences were direct phone-to-Drive incremental backups using the owner's Google Cloud project, without an additional encrypted cloud archive.

`n20-mtp-start` adds an MTP function to the existing USB ACM/NCM gadget. It requires the compiled `n20-mtp-sandbox` and `n20-mtp-refresh` in `/usr/local/sbin`, plus the patched responder at `/usr/local/lib/n20/umtprd`; install all three before updating the startup script. The sandbox fails closed if the patched responder is missing. Build instructions, source provenance and rollback guidance are in [the responder patch notes](../third_party/umtp-responder/README.md). The sandbox starts the responder under reserved numeric UID and GID 61123, a private network and mount namespace, a read-only chroot, `no_new_privs`, and only `CAP_DAC_OVERRIDE`. When the world directory is replaced, the monitor withdraws the old export and the refresh helper reconnects MTP after a safe replacement appears. This briefly resets USB networking. A disposable-world test confirmed that Windows re-enumerated the replacement; an in-progress copy is not a consistent snapshot. A game process that is already compromised can still expose files it owns by deliberately hard-linking or copying them into the world; the sandbox does not protect against that. The UID and GID must not be assigned to another account.

A later phone-side comparison found the running responder byte-identical to the
installed binary and the retained patched build. Its effective UID was 61123,
its effective capability set contained only `CAP_DAC_OVERRIDE`, and
`no_new_privs` was set. This checks the running process and local build
correspondence, not independent provenance of the build environment.

The repository MTP configuration exports only `/srv/minecraft/world` read-only, avoiding the normal server configuration and backup repository paths. Existing installations must update the MTP configuration and restart the responder. The patched responder skips symlinks, including links within the world, and continues enumerating other entries. Files reached only through symlinks are therefore absent from MTP copies. Raw files copied while the server is changing the world may not form a consistent backup. The gadget setup may briefly reset USB networking; its fallback attempts to restore it.

The supervisor creates `/run/n20` as a root-owned, mode-0700 directory before
starting services. An earlier boot exposed a race where the MTP sandbox could
start before that directory existed and fail closed. The corrected startup
order is covered by an isolated service-launch test; on the affected running
phone the sandbox was started again and its read-only export was verified.

A read-only check on 2026-11-08 found the running responder using UID and GID
61123, `no_new_privs`, only `CAP_DAC_OVERRIDE` in its effective and bounding
sets, distinct mount and network namespaces, and a read-only world mount. This
confirms the observed process configuration at that time; it does not prove the
absence of responder vulnerabilities or prevent game-owned hard links.

## Rebuild the initramfs and boot image

The builder expects the exact Alpine aarch64 `busybox-static-1.37.0-r20.apk` SHA-256 `ee469aee2958feffd7f64dd96655704025ff60af614f77a1d7323dc237c34da2` and a C.24 `vendor_ramdisk` SHA-256 `8dd5faeefde663f7037540c07c3bce6a6e0452afdb3b995a08c2bc5b98c3eea9`. The BusyBox APK is in Git LFS; the vendor ramdisk is kept locally. With these inputs:

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
