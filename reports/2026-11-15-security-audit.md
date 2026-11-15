# Nord N20 Minecraft phone security audit — 2026-11-15

> The audit below records the initial state. Later the same day, the runtime
> findings were remediated and verified; see [Remediation and backup-error
> verification](#remediation-and-backup-error-verification). Physical recovery
> and vendor-platform risks remain. The original result and no-mutation
> statement describe the pre-remediation audit, not the final installed state.

The later [Minecraft availability and Wiki isolation follow-up](2026-11-15-minecraft-availability.md)
records dedicated socket/GPU groups, combined memory limits, swap-aware guards,
supervision and the available local router checks.

## Result

**The phone has useful application hardening, but its credential isolation is currently bypassable.** The most urgent finding is a world-readable userdata block device. Both the Minecraft account and the separate Wiki model account successfully read the ext4 signature directly from it despite having no capabilities. A compromise of either account could therefore bypass ordinary file permissions and read on-device secrets from disk.

This audit identified **three high-severity risks and three medium-severity findings**. Severity reflects this device's exposure and consequences; it is not a CVSS score or evidence that an attacker has exploited anything. Physical access and the aging platform are explicitly distinguished from remotely exploitable software defects.

No production configuration was changed, no software was installed, and no reboot, flash, firmware replacement, partition change, game command, or production restore was performed by this audit. Temporary test data and isolated backup restores were removed afterward. Credential values, player identities, world contents, and public IP addresses are omitted.

## Scope and evidence

Live observations were collected over existing host-key-checked SSH on November 15, approximately 10:04–10:20 a.m. PST (18:04–18:20 UTC). The device runs Debian 13 arm64, vendor kernel `5.4.254-qgki-g93bb89e5a062`, Temurin `25.0.5+8`, and vanilla Minecraft `26.3`.

Coverage included boot and physical interfaces; IPv4/IPv6 listeners and firewall state; effective SSH policy; accounts, device nodes, filesystem permissions and privileges; Minecraft settings and executable provenance; privileged helpers, dashboard and Wiki boundaries; Wi-Fi; Debian security advisories; local and cloud backups; and focused regression tests.

The checkout and running server changed during the audit through other activity. HEAD was initially `8c7a7b9f9775e2024bd32ea81d49a416ccfb3c81` and later `d1f67500553bc15618b2038e8193aa064427d171`; Java PIDs also changed. Source tests used a separate temporary copy of the working tree. Findings are point-in-time observations, not a frozen forensic image. Existing work was not overwritten.

## Prioritized findings

| ID | Severity | Finding | Evidence / prerequisite |
| --- | --- | --- | --- |
| F01 | High | Raw userdata is readable by unprivileged services | Confirmed two-byte read as UID 999 and UID 61124; requires local code execution, such as a compromised game or model process |
| F02 | High, physical | USB provides an unauthenticated root shell | Running root shell on `ttyGS0`; matching boot script; requires physical USB access |
| F03 | High, platform risk | Old vendor kernel with exposed unprivileged attack surface | Unsupported upstream branch, unprivileged BPF map creation and kernel-log reads confirmed; no particular kernel exploit tested |
| F04 | Medium | No host firewall, including IPv6 | All legacy tables have only default ACCEPT policies; Minecraft reachable over global IPv6 from the LAN |
| F05 | Medium | SSH authentication events lack an active log destination | No syslog socket or logging daemon; empty journal directory; sshd stderr points to `/dev/null` |
| F06 | Medium | Wiki requests have no admission limit | Isolated reproduction accepted 2,000 requests from one player even while processing was disabled |

### F01 — Raw userdata bypasses private-path protections

`/dev/userdata` is a root-owned block device with mode **0644**. `/` is an unencrypted ext4 filesystem mounted from that device. Other device-specific partition nodes inspected were mode 0600.

Using `setpriv --no-new-privs --bounding-set=-all --clear-groups`, both UID/GID 999 and UID/GID 61124 opened `/dev/userdata` read-only and read exactly two bytes at offset 1080. Both obtained the expected ext4 magic. No filesystem payload or credential contents were read through the block device.

Normal pathname checks correctly deny these accounts access to backup passwords, Wi-Fi settings, SSH private host keys and the Drive OAuth configuration. Those checks do **not** prevent reading the underlying blocks. The device also contains an unencrypted swap file. The exposure potentially includes credentials, encrypted backup keys/passwords, private service data and data remnants; extraction of individual secrets was intentionally not attempted.

The cause is visible in [`boot/persistent-init`](../boot/persistent-init), line 53: userdata is created with `mknod` without an explicit restrictive mode. The running `/init` is byte-identical to that source. [`n20-security-sysctls`](../runtime/usr/local/sbin/n20-security-sysctls) protects executable paths and link behavior but does not validate block-device access.

**Remediation:** first validate the existing node's type, ownership and device identity, then restrict it to root with mode 0600. This can be corrected on the running filesystem without flashing. Make the startup security helper validate and enforce this before unprivileged services start, and test read/write denial as both service identities. An explicit `mknod -m 600` belongs in the boot source for any future separately reviewed boot build; do not flash merely to address this finding. Audit all block-node aliases and inherited descriptors as part of the fix. If compromise is suspected, treat on-device secrets as exposed and plan rotation while preserving recovery access to existing backups.

### F02 — Physical USB access is administrative access

The running root shell's stdin was `/dev/ttyGS0`. The matching boot script continuously launches an interactive root Bash shell without authentication and provides an unauthenticated emergency shell if the root filesystem cannot be mounted. The dashboard also exposes administrative actions to anyone holding the phone. Repository documentation records an unlocked bootloader and disabled verified boot; these properties were not changed or independently re-established by a boot test.

This is a documented recovery design, but it means anyone able to attach to the data port can control the server and obtain the phone's credentials and backups. The phone is not a secure unattended appliance in an untrusted physical location.

**Remediation:** physically restrict the phone and its data port now. Redesign console authentication and unattended recovery only alongside a verified alternate recovery path. Do not remove the sole working recovery interface or relock the bootloader as an audit cleanup step. Storage encryption and verified boot would require a larger device-specific migration, not a routine package update.

### F03 — The kernel is outside a verifiable maintained security baseline

The phone runs a custom vendor 5.4.254 kernel and older device-specific radio/GPU components. Linux 5.4 is no longer listed among upstream maintained long-term branches. Vendor backports cannot be inferred from the version string, and this audit did not establish a current vendor patch manifest for the exact installed build. [Kernel.org maintenance list](https://www.kernel.org/releases.html).

The live kernel exposes additional attack surface to a compromised service:

- `kernel.unprivileged_bpf_disabled=0`; a single-entry BPF hash map could be created as the capability-free Minecraft identity. Its descriptor was immediately closed; no BPF program was loaded or attached.
- `kernel.dmesg_restrict=0`; the same identity could read the kernel log. Contents were discarded. `kernel.kptr_restrict=0` too, although sampled kallsyms addresses were zero, so a symbol-address leak is **not** claimed.
- Java, the Wiki bridge and the GPU worker have `Seccomp: 0`. Java and the model worker share the host mount/network namespaces. The memory cgroup places Java at the root, without a dedicated service limit.
- AppArmor and Yama are not built in. SELinux is built in, but processes reported the initial `kernel` label; no loaded application confinement policy was established. PID and user namespaces are not built in.
- The model worker intentionally has read/write access to KGSL and ION GPU device nodes. The bridge's supplementary group also grants GPU-device access. The JVM itself has no supplementary groups and failed direct GPU-device access checks.

ASLR, strong stack protection, hardened usercopy, signed-module enforcement, and the four filesystem link/FIFO protections were enabled. They reduce risk but do not replace kernel maintenance.

**Remediation:** isolate this appliance on a restricted network and plan migration to a platform with supported kernel/firmware and a tested recovery path. In the current setup, assess enabling `unprivileged_bpf_disabled=1`, restricting dmesg/kernel pointers, and introducing compatible syscall, device and memory restrictions. Validate these against radio, GPU and recovery requirements before deployment. No specific privilege-escalation CVE or remote kernel compromise is asserted from a version match alone.

### F04 — Network policy depends entirely on service configuration and the router

The kernel's legacy IPv4 and IPv6 tables were queried with read-only `getsockopt` calls. Every table had only its built-in ACCEPT policy entries and terminal error entry; there were no filtering rules. nftables is not built into this kernel, and neither nft nor iptables administration tools were installed.

| Service | Live bind | Workstation Wi-Fi IPv4 | USB IPv4 | LAN to global IPv6 |
| --- | --- | --- | --- | --- |
| SSH, TCP 22 | All IPv4 interfaces | Reachable | Reachable | Not reachable; no IPv6 SSH listener |
| Minecraft, TCP 25565 | IPv4/IPv6 wildcard | Reachable | Reachable | Reachable |
| Minecraft management, TCP 25585 | IPv4 loopback only | Not reachable | Not reachable | Not reachable |

No UDP listeners were reported by `ss`. RCON and query were disabled. A global IPv6 address and default IPv6 route were present. The initial short-timeout probes were inconclusive; repeated four-second probes produced the results above, corroborated by phone-side listener and connection checks.

The IPv6 test originated on the LAN. **Internet reachability, router forwarding, UPnP and the router's IPv6 firewall were not verified.** A global address alone does not prove Internet exposure, and IPv4 NAT does not establish an IPv6 access policy.

**Remediation:** define permitted player and administration networks, then enforce an explicit IPv4 and IPv6 host policy using this kernel's supported legacy netfilter interface or a tested equivalent. Preserve established SSH sessions, loopback, DHCP, necessary ICMPv6 and the existing recovery path during rollout. Restrict SSH to trusted administration sources; keep the management API loopback-only. Independently check router rules from an authorized off-LAN vantage point.

### F05 — SSH authentication logging is absent

The effective SSH policy uses `LogLevel INFO` and facility `AUTH`, but `/dev/log` and `/run/systemd/journal/dev-log` do not exist. No syslog/journald process was present; `/var/log/journal` was empty, and `auth.log` and `syslog` were absent. The listening sshd's stderr descriptor pointed at `/dev/null`. The supervisor starts sshd without an explicit log file.

Successful and failed authentications therefore lack the normal detailed, persistent log trail. Existing login accounting or application logs are not substitutes. This limits detection and incident reconstruction, particularly after earlier documented writable system paths. It also prevents this audit from establishing the absence of prior unauthorized access.

**Remediation:** add a supervised, persistent logging destination appropriate to the non-systemd boot architecture, or an explicit protected sshd log file. Verify both a successful and a rejected authentication reach it, and add retention/rotation and optional off-device collection. Existing append-only service files also need rotation; the Minecraft log was approximately 58 MB during this audit.

### F06 — Wiki queue can grow without bound

[`wiki_chat.py`](../runtime/usr/local/lib/n20/wiki_chat.py), `accept_chat()` around line 1743, accepts a valid player's request and unconditionally appends it to the queue. `STATUS_QUEUE_LIMIT=128` limits displayed/serialized entries in some paths; it does not bound the live `deque`. `REQUEST_INTERVAL_SECONDS=30` limits starting work, not incoming requests. Requests are accepted while `can_run=False`.

An isolated test replaced the server-identity and console-output functions with inert stubs and submitted 2,000 ordinary requests from one synthetic player. All 2,000 remained queued. No requests were sent to the live server or model.

A whitelisted player can build a long backlog, repeatedly broadcast acknowledgments and keep work pending after resource pressure prevents execution. Sustained abuse can consume memory, disk/log capacity and GPU time. Online authentication and the three-entry whitelist reduce the attacker population; this is an authenticated-player availability issue, not an unauthenticated remote-code-execution finding.

**Remediation:** bound total pending requests and pending bytes, enforce per-player quotas/cooldowns, expire old requests, and return a rate-limited queue-full response. Keep the model resource guard, but make admission limits independent of whether the worker can currently run. Test limits with processing disabled and with one player dominating the queue.

## Controls that passed

- Minecraft uses online authentication, an enabled and enforced whitelist, secure-chat enforcement, a five-player limit, no operators, and disabled RCON/query. Its management secret was set with length 40; value and entropy were not exposed or independently proven. Management TLS is disabled, consistent with its loopback-only bind.
- Java ran as UID/GID 999, with empty supplementary groups, zero effective/bounding capabilities and `NoNewPrivs: 1`, including the final process observed. The GPU worker used UID/GID 61124 with no capabilities and `NoNewPrivs: 1`.
- Root-enumerated, service-identity write checks found no Minecraft-writable non-symlink entries in `/usr`, `/opt`, `/etc`, `/var/lib/n20` or `/run/n20`. Private credential paths denied direct reads. **F01 supersedes any inference that these pathname checks guarantee confidentiality.**
- SSH permits only root with public-key authentication; passwords, keyboard-interactive authentication, empty passwords and GSSAPI authentication are disabled. It has three authentication attempts and bounded unauthenticated connection startup. One Ed25519 authorized-key entry was present, and no account had an empty password field. Authorized-key values were not printed.
- The current Wi-Fi connection used WPA2-PSK with CCMP pairwise/group ciphers. Supplicant and touchscreen control paths were root-only. New WPA2 configurations explicitly require RSN/CCMP.
- The MTP responder used UID/GID 61123, `NoNewPrivs: 1`, only `CAP_DAC_OVERRIDE`, separate mount/network namespaces, and a read-only chroot/world mount with restrictive mount flags. Raw userdata is not exposed inside that chroot. This was process/mount inspection, not USB-protocol fuzzing.
- The running init matched the repository boot script. Twenty-nine installed runtime files matched the temporary source snapshot exactly; the remaining Wi-Fi manager differed by one blank line. The newly added log configuration was included in that comparison. This does not prove that an older in-memory Python process has reloaded its file.
- `dpkg --verify` reported only the intentional MTP configuration modification, which matched this repository. This verifies against the local package database, not an independently trusted installation image.
- Mojang HTTPS metadata identified 26.3 as latest and matched the installed server JAR's size and SHA-1. The linked version metadata matched its manifest SHA-1. All 40 bundled server/library SHA-256 entries matched extracted files. All 42 observed open JVM JAR descriptors pointed into the protected `/opt/n20-minecraft` bundle. [Mojang manifest](https://piston-meta.mojang.com/mc/game/version_manifest_v2.json).
- The installed signed WLAN module matched the device documentation's pinned SHA-256. Vendor binaries, firmware and the toolchain were not independently authenticated in full.
- The workstation's ignored `local/` ACL was restricted to the owner, Administrators and SYSTEM. Targeted current-tree filename/key-pattern checks found no matching private keys or common API-token forms. This was not an exhaustive Git-history secret scan.

## Patch and advisory review

Debian's live security-tracker JSON was retrieved over HTTPS and compared with all **142 installed source-package identities**, using Debian version ordering. The feed reported a last-modified time of November 15 at 17:32 UTC. No installed non-kernel source package was older than a fixed trixie version recorded by that comparison. The 46 below-fixed-version matches were all from source package `linux`, installed here only as `linux-libc-dev` headers; these are **not 46 proven vulnerabilities in the running vendor kernel**. Open and undetermined tracker issues still require feature/architecture-specific triage. [Debian tracker feed](https://security-tracker.debian.org/tracker/data/json).

| Component | Observed version | Assessment |
| --- | --- | --- |
| OpenSSH server | `1:10.0p1-7+deb13u4` | Matches current trixie version; open advisories remain |
| OpenSSL / libssl | `3.5.7-1~deb13u3` | No installed-below-fixed trixie match in the feed comparison |
| CA certificates | `20250419+deb13u1` | Installed security update recorded in device documentation |
| glibc | `2.41-12+deb13u4` | Open issues remain; source matches do not prove reachable vulnerable functions |
| Python 3.13 source | `3.13.5-2+deb13u5` | Open issues remain, including feature-specific archive/network cases |
| wpa_supplicant | `2:2.10-24` | Tracker includes open PMKSA-context and privileged configuration-write issues |
| rclone | `1.60.1+dfsg-4` | Numerous open upstream issues; active use is a Drive upload/check client |
| Restic | `0.18.0-1+b4` | Local repository data and isolated restore verified below |
| rsync | `3.5.0+ds1-0+deb13u1` | Backup copier uses local transfers with dropped privileges |
| Temurin | `25.0.5+8` | Matches current published Java 25 release and documented October trust-store fix |

For example, OpenSSH CVE-2026-60000 and CVE-2026-55654 concern GSSAPI paths; live GSSAPI authentication and key exchange are disabled. They are not demonstrated attacks against this configuration. Compression is enabled, so review CVE-2026-106582 and consider disabling unused SSH compression. An undetermined tracker entry remains undetermined. [GSSAPI authentication DoS](https://security-tracker.debian.org/tracker/CVE-2026-60000), [GSSAPI cleanup issue](https://security-tracker.debian.org/tracker/CVE-2026-55654), [compression advisory](https://security-tracker.debian.org/tracker/CVE-2026-106582).

The uploader does not enable rclone RC, WebDAV, S3 serving, `--links`, or `--metadata`; advisories requiring those features do not establish an exposed service here. The wpa control interface is not accessible to the game identity, which limits the configuration-write issue's applicability. Keep monitoring both packages rather than treating the absence of an available stable update as proof of no vulnerabilities. [rclone tracker](https://security-tracker.debian.org/tracker/source-package/rclone), [wpa tracker](https://security-tracker.debian.org/tracker/source-package/wpa).

Adoptium lists 25.0.5+8 as containing its October eMudhra trust-store remediation. This audit checked the installed version; it did not repeat the earlier certificate-chain experiments or authenticate every Java binary against a freshly downloaded release archive. [Adoptium advisory](https://adoptium.net/news/2026/10/temurin-october-2026-emudhra), [release/support schedule](https://adoptium.net/support).

`apt-get -s upgrade` found no upgrades against cached lists, but those lists were old enough that this was not used as a fresh patch-status guarantee. No package-list refresh or upgrades were performed. Normal systemd timers are not an update mechanism for this BusyBox-init system; establish an explicit operator review cadence that also covers Java, Minecraft, BusyBox, the patched MTP responder, the Wiki runtime and vendor components.

## Backup and recovery verification

The initial inventory found six local snapshots, all marked uploaded. A seventh appeared during concurrent activity. Local retention history began November 10; retention policy cannot recreate any earlier missing history.

- Read-only Restic metadata check passed. A subsequent complete `check --read-data` also passed for approximately **689 MB** of repository data at that point. `--no-lock --no-cache` avoided repository-lock and persistent-cache changes; this is a live-repository check, not a transactionally frozen audit.
- The newest snapshot was restored with `--verify` into a private disposable directory. Its 212 regular files were restored successfully, and `level.dat` decompressed with the expected NBT compound tag. The production world was never replaced or loaded by a test server.
- An initial cloud comparison overlapped a newly created snapshot's upload and returned nonzero. A recheck of the snapshot from **18:14:13 UTC** found **212 matching files and zero differences**. Its `COMPLETE.json` was then verified and recorded completion at **18:16:09 UTC**. Incomplete cloud copies must continue to be rejected for recovery.
- The cloud check compared the entire restored local file tree against Drive checksums. It did **not** download and boot a complete cloud restore. Copies of the OAuth configuration used for read-only verification stayed in private temporary directories and were deleted; any refresh could not rewrite the installed configuration.
- The configured OAuth scope was `drive.file`. The remote is ordinary Drive, without rclone crypt; that matches the documented choice to keep readable cloud copies. Actual Drive sharing permissions, Google account security and independently protected recovery copies were not audited.
- Approximately 94 GiB remained free during the initial check. Local snapshots do not force Minecraft saves and are not atomic filesystem snapshots. A successful checksum/restore test does not prove application-level consistency of a live snapshot.

A full stock-phone recovery remains unverified, and local backups share the phone's hardware failure domain. Preserve independently protected recovery materials and perform a planned end-to-end cloud recovery drill on isolated data. Do not experiment with the production world or boot partitions to close this audit limitation.

## Test results and audit limitations

**Sixteen applicable script suites passed individually** in the temporary ARM64 source copy: admin settings, save events, status deadline, SSH policy, Wi-Fi security, timekeeper, backup/restore, backup retention, restore-area guards, Drive backup, Wiki prefix validation, Wiki resource policy, Wiki chat, map chat, swap startup, and the shell filesystem-security test. Headless dashboard and capture C tests also passed. The dashboard compile initially supplied a duplicate `_GNU_SOURCE` flag; it passed when rerun with the repository's documented flags.

The disposable-server `test-pin-that-server.py` invocation exited on missing required arguments without running a server. It is **not** counted as passed; no extra gameplay server was launched for this audit.

The existing `test-live-private-paths.py --check-live` **did not pass as a whole**: its unprivileged `find` stopped on the root-only `/usr/local/lib/n20/__pycache__` directory. Earlier direct-denial checks passed. A separate root-enumerated, unprivileged `os.access` scan found no writable protected paths. This is a test-coverage/robustness issue, and the test also omits raw block-device reads and the newer Wiki/Drive boundaries. Extend it as part of F01 remediation; do not weaken the private directory just to make traversal succeed.

Not performed: destructive exploitation, credential extraction, load/flood attacks, wireless attacks, exhaustive native-code fuzzing, modem/GPU firmware reverse engineering, boot flashing, off-LAN/router testing, Google sharing review, full Git-history secret scanning, independent vendor/toolchain provenance verification, or forensic acquisition. Package checks and local file hashes cannot establish that previously writable system files were never abused. No claim of an uncompromised system or absence of all vulnerabilities is made.

The final status probe returned Minecraft 26.3 online with zero players and the five-player maximum; its capabilities remained empty and `no_new_privs` remained enabled.

## Remediation order

1. Close F01 on the running phone, add startup enforcement and negative access tests, and assess whether credential rotation is warranted. No boot flash is needed for the immediate containment.
2. Restrict physical/data-port access, define an IPv4/IPv6 network policy, and restore SSH security logging with retention.
3. Add Wiki admission limits and test them with stalled/disabled processing; review service resource limits and compatible kernel hardening.
4. Establish advisory reviews, verify router exposure and cloud account/sharing controls, and rehearse isolated disaster recovery.
5. Plan eventual migration to supported kernel/firmware with a verified recovery path; preserve this device's existing recovery materials throughout.

## Remediation and backup-error verification

The user subsequently authorized fixes and safe live deployment. This section
describes those changes; the earlier audit's no-mutation statement applies to
the initial audit only. Durable operator guidance is in
[security controls and remaining boundaries](../docs/security.md).

| Finding | Resolution checked on the running phone |
| --- | --- |
| F01 | **Closed for service access:** userdata identity was checked against sysfs (block node 259:0, partition name userdata) and restricted to root:root 0600. Startup now restricts all real block nodes before services. Both capability-free service identities failed read/write opens, without reading bytes. The boot source also uses mknod mode 0600; it was not flashed. |
| F02 | **Residual physical risk:** unauthenticated USB/emergency root and the unlocked bootloader remain. Existing recovery was preserved; no boot, firmware, partition or lock-state change was made. Physical/data-port restriction and a separately verified recovery/platform migration are required. |
| F03 | **Mitigated, platform risk remains:** BPF=1, dmesg=1 and kptr=2 verified live. Java, Wiki bridge and GPU worker now have inherited seccomp filters and separate memory cgroups. Vendor 5.4 maintenance/provenance, shared namespaces and necessary GPU ioctls remain unresolved platform boundaries. |
| F04 | **Host policy deployed and tested:** IPv4/IPv6 INPUT and FORWARD DROP, explicit loopback/established/ICMP/DHCP/game allowances, SSH only from current on-link IPv4 subnets on Wi-Fi/USB. TCP25565 deliberately remains permitted from any source to preserve remote players. Router/WAN reachability is still unverified. |
| F05 | **Closed for local observability/retention:** supervised foreground sshd writes a root-only explicit authentication log; accepted and failed public-key events were observed. Protected rotation runs under BusyBox init; real oversize rotation/retention passed and no live rotation error remained. Copytruncate has a small message-loss window and periodic size checks can overshoot. Off-device collection is not configured. |
| F06 | **Closed:** shared admission bounds chat and asynchronous map misses while processing is disabled: 16 waiting requests, 8 KiB UTF-8 text, two per player including active work, ten-second admission spacing, five-minute expiry, and private rejections limited to one/player/30 seconds. Handoffs preserve age and caps. |

### Phone backup error

The actual dashboard diagnostic was:

`Snapshot saved; cleanup failed: Backup tool: the unlock command can be used to remove stale locks`

The local snapshot had succeeded. No production Restic process or repository
lock remained when investigated. Source ordering allowed an immediate Drive
restore wake to take a shared lock just before local retention requested an
exclusive lock. A real isolated Restic contention test reproduced the need to
wait for the legitimate holder; this was not evidence of corrupt snapshots.
No production lock was removed and no `unlock` command was used on production.

The uploader is now woken after local retention, including when cleanup fails;
pre-restore checkpoints still wake it. Both clients use bounded Restic lock
retry. Short administration reads wait 30 seconds within a 120-second timeout;
longer mutations/Drive work allow five minutes within longer timeouts. A prior
dashboard error is cleared only after a successful operation; a new cleanup
warning remains visible.

A real background snapshot and retention completed with exit 0, increased
history from seven to eight snapshots, and cleared the phone error. The newest
snapshot was copied to Drive, its full checksum comparison succeeded and its
completion marker was written. After the clean service restart, the new
automatic scheduler also observed the resulting saved-file changes and
created the ninth snapshot. The ninth snapshot was listed as uploaded by the
new worker and its Drive `COMPLETE.json` was independently read and matched
the local snapshot identity. Exactly one instance of each of the six service
roles was present. There were zero production Restic locks and no Restic
processes at final verification; `/run/n20/operation` was absent. Refreshing
the normal dashboard state reported server online, nine snapshots, and empty
operation/error fields. The prior displayed cleanup operation had been a
cached state entry while the display was asleep, not an active/stuck cleanup.

### Service activation and validation

A fresh zero-player check preceded a normal console stop/save. The existing
server-only supervisor was replaced while preserving the FIFO inode and all
restore/battery holds. The Wiki queue was empty with no active question before
its launcher was restarted through its cleanup trap. No extra game server was
started. Both backup schedulers were reloaded. No full phone reboot, world
replacement or production restore was performed.

Live Java (UID/GID 999, no supplementary groups), bridge (UID/GID 999,
model-socket group 61124) and model (UID/GID 61124, no supplementary groups)
all reported `Seccomp: 2`, `NoNewPrivs: 1`, and zero effective/bounding
capabilities. Their cgroups imposed charged-RAM ceilings of 4 GiB, 256 MiB and
3.5 GiB respectively, with memory fail counts zero at verification. These are
not total RAM-plus-swap limits or a guarantee against whole-device OOM; the
existing resource guards were retained. Minecraft returned online with zero
players; the sandboxed model returned healthy and completed a real one-token
GPU request.

The firewall was first accepted by the real vendor kernel in disposable
network namespaces. Veth socket tests verified IPv4 administration/game
allowances, IPv6 game allowance and denied unsolicited listeners in both
families. Mock failures verified dual-stack rollback, including an IPv4
rollback failure still attempting IPv6 restoration. The live initial tables
were checked for concurrent rules, then a timed rollback was armed before
application. Fresh independent Wi-Fi and USB SSH plus game TCP/status checks
passed before confirmation. DHCP bound/renew/deconfig and failure handling
were tested with fake network tools.

Applicable suites passed individually on ARM64 Debian:

- New raw-device identity/alias restrictions, service seccomp/cgroup child,
  real dual-stack firewall/socket/rollback checks, DHCP firewall refresh,
  real log rotation/retention, real Restic contention/error-state behavior,
  and disabled-worker Wiki admission/map/expiry/handoff checks.
- Existing filesystem-security setup, live private paths, SSH effective
  policy, Wiki resource policy, Wiki chat (29 tests), map chat (73 tests),
  autosave scheduler, admin settings, backup/restore including failed swap,
  backup retention, world-copy replacement guards, backup wake events,
  Drive isolation, Wi-Fi integration, supervisor server-only/FIFO,
  supervisor recovery/battery holds and supervisor startup ordering.
- Shell syntax and Python compilation for changed sources/tests. No native
  binaries or firmware were changed, so a native/boot rebuild was unnecessary.

The previously failing live private-path suite now root-enumerates protected
trees and checks access after dropping identity, avoiding unreadable-directory
traversal failures without weakening private directories. It additionally
checks raw block nodes and newer Wiki/Drive credential boundaries.

Public installed scripts/configuration have retained rollback copies outside
Git. Disposable source/test trees, raw outputs and ephemeral authentication
test keys are removed after recording evidence. Existing credentials, Wi-Fi
settings, game configuration/worlds, backup secrets and recovery materials
remain intact. Closing raw access cannot establish whether anything was read
earlier; credential-rotation assessment remains warranted if compromise is
suspected. The operator maintenance cadence and remaining cloud/account,
router and disaster-recovery checks are documented in `docs/security.md`.
