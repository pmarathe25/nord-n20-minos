# Security controls and remaining boundaries

This is an experimental, device-specific appliance. The controls below were
installed and checked on November 15, 2026; they do not establish that the
previously exposed system was never compromised. See the dated security audit
in `reports/` for evidence and limitations.

## Startup and raw storage

`n20-security-sysctls` runs before unprivileged services. It requires
`n20-security-devices`, which checks that `/dev/userdata` is a block device
matching the sysfs partition named `userdata`, then restricts all real block
nodes under `/dev` to root:root mode 0600. Symlinks are not followed. A wrong
userdata identity fails startup. The boot source also creates userdata with
mode 0600; this source change has **not** been flashed. The installed older
initramfs is contained by the runtime check before service startup.

The helper retains its root ownership, protected-directory and filesystem
link/FIFO checks. It also requires `unprivileged_bpf_disabled=1`,
`dmesg_restrict=1`, and `kptr_restrict=2`. BPF value 1 can remain locked until
reboot; do not assume root can undo it immediately. These controls passed on
the installed vendor kernel.

## Service confinement

`n20-service-sandbox` creates root-controlled memory cgroups under the private
`/run/n20-cgroups/memory` mount and installs an inherited seccomp filter before
executing the existing capability-dropping launch commands. Current limits:

| Service | Charged RAM | Combined charged RAM plus swap | OOM preference |
| --- | --- | --- | --- |
| Minecraft | 4 GiB | 6 GiB | -250 |
| Wiki model | 3.5 GiB | 4 GiB | +900 |
| Wiki bridge | 256 MiB | 384 MiB | +600 |
| Optional Wiki parent only | 3.5 GiB | 4.5 GiB | Children remain preferred victims |

Minecraft is outside the optional parent; that parent's OOM selection cannot
kill it. The combined limit is not a fixed swap quota: Java can, for example,
have 2 GiB RAM and 4 GiB swap within its 6 GiB combined ceiling. Its heap stays
at `-Xmx2G`. Minecraft is not OOM-immune; OS/SSH recovery must remain possible.
Memcg charges can differ from RSS because of shared/file-backed page ownership,
and vendor GPU allocations may escape those charges. Limits therefore do not
guarantee avoidance of system OOM. Core dumps are disabled.

The small syscall deny policy covers BPF, performance events, userfaultfd,
ptrace/process-memory writes, keyring operations, raw-handle opens, mounts,
module loading, reboot, kexec and swap administration. Normal Java, Python,
networking and OpenCL calls remain permitted. Live Java, bridge and model
processes were checked for `Seccomp: 2`, `NoNewPrivs: 1`, zero effective and
bounding capabilities, and the intended cgroup membership. A real small GPU
completion succeeded after restart.

This is not a comprehensive syscall allowlist, filesystem sandbox or namespace
isolation for these services. The Wiki model still needs vendor GPU ioctls. The
bridge has dedicated socket group **61125**, separate from GPU group **61124**;
it can connect to the setgid model directory/socket but cannot open KGSL/ION.
The model retains UID/GID 61124 and its GPU access. Reserve both numeric groups
for this installation; do not assign socket group 61125 unrelated privileges. The
vendor kernel, GPU/radio code and shared host namespaces remain important
attack surfaces. The old 5.4 vendor kernel has not acquired a maintained patch
baseline through these changes. Migration to supported hardware/software with
tested recovery remains the durable platform remedy.

### Swap-aware Minecraft priority

Wiki is allowed to page while Minecraft remains responsive. Initial model or
launcher admission needs at least 2 GiB free swap and 3 GiB of combined
`MemAvailable` plus free swap, with 30 seconds of healthy samples. There is no
physical-RAM-only minimum: even zero reported available RAM can pass when
swap provides the headroom. The chat bridge uses matching capacity checks.
Low RAM alone and cold pages in swap do not stop Wiki.

The running model yields after three checks with less than 768 MiB
free swap or less than 1 GiB combined headroom. Ten consecutive checks with
memory PSI avg10 full at least 5%, some at least 20%, or IO full at least 25%
also cause a yield. Healthy samples reset each counter independently. The
launcher checks about once per second, so these are sample counts, not precise
real-time deadlines. These are initial guardrails, not proven crash prevention.
Linux can reclaim cache and swap anonymous pages, but some kernel/driver
allocations are not reclaimable. Free swap does not guarantee GPU allocation
success; the service limits, OOM preference and pressure guards still apply.

A pressure stop waits at least 120 seconds before a new 30-second healthy
admission window. Temperature protection remains active. Bounded TERM then
KILL avoids waiting forever on a nonresponsive optional worker. If a kernel
task cannot finish even after KILL, a nonempty old worker cgroup prevents
another model from being launched; a kernel/driver hang cannot be solved by
user-space supervision.

The existing root-only 6 GiB swapfile and swappiness 10 are unchanged. At the
November 15 check, only about 225 MiB was used, memory/IO PSI averages were
zero and no kernel/cgroup OOM event was recorded. More swap can help actual
swap exhaustion, but cannot enlarge Java's heap or remove storage latency
from paging. No active swapfile was resized and no swapoff was attempted.

### Restart and failure visibility

`n20-wiki-supervisor` holds a single-instance lock and restarts a failed Wiki
launcher with delays from 30 seconds up to 15 minutes, gated by healthy resource
admission. A failed model uses bounded 30–300 second backoff and retains its
existing profile fallback. Minecraft restarts after 10 seconds initially,
increasing to at most 120 seconds for repeated short failures; a clean exit or
five-minute stable run resets that delay. Recovery holds still override starts.

`n20-service-health` keeps 32 nonprivate incident records plus current role
state in root-only `/var/lib/n20/service-health.json`. Repeated identical holds
are deduplicated. It distinguishes cgroup OOM counters (including the optional
parent), a system OOM event since launch, Java heap/native allocation error
signatures, and other exit statuses/signals. A system OOM counter increase
since launch does not prove that it killed that particular process. Existing rotated
logs retain the fuller context. The dashboard's existing attention notice
shows Minecraft restart reasons, Wiki recovery, and the actual pressure/pause
reason without marking a backup/server operation busy. Higher-priority backup
errors and recovery/charge holds take precedence. Recent Minecraft incidents
remain visible for a day; successful restarts do not erase the incident ring.

## Network policy

`n20-firewall` uses **legacy** iptables/ip6tables; this kernel lacks nftables.
It sets IPv4 and IPv6 INPUT and FORWARD to DROP, with OUTPUT ACCEPT. It allows
loopback, established/related traffic, ICMP/ICMPv6 and Wi-Fi DHCP replies. SSH
is allowed only on `wlan0`/`usb0` from their current directly connected IPv4
subnets. This is an on-link boundary, not proof that everyone on that network
is trusted; SSH still requires the authorized key.

Minecraft TCP **25565 is deliberately allowed from any source in both
families**, preserving possible remote-player use. Online authentication and
the whitelist remain the player-admission controls. Changing `server-port`
requires a coordinated firewall update; the root firewall never follows a
game-owned settings file to derive it. Management remains loopback-only.
Router forwarding, UPnP, router IPv6 policy and actual Internet reachability
have not been verified from an off-LAN vantage point. The owner confirmed no
external test access was available. A bounded read-only gateway check reached
the router's HTTP login landing page; no IGD discovery response arrived from
the gateway or multicast SSDP. No response does not prove UPnP disabled. No
router login was attempted, no mapping changed, and no public scan service
received the home endpoint.

The DHCP helper reapplies the policy after bound, renew and deconfig events,
so administration subnets follow the lease without wildcard reopening. A
refresh failure is reported and preserves the previous policy. Applying two
families is not atomic: the helper captures both existing tables and attempts
both rollbacks on any apply failure, reporting rollback failures. It owns the
filter table; coordinate additional rules with this helper rather than adding
independent rules that a refresh would replace. The initial live deployment
first checked that no concurrent rules had appeared, armed a timed rollback,
and verified fresh Wi-Fi/USB SSH and game connections before confirming it.

## Authentication logging and retention

`n20-ssh-start` supervises foreground sshd under BusyBox init, with a root-only
`/var/log/n20-ssh.log` and explicit `-E` logging. VERBOSE logging records
accepted and rejected public-key attempts. SSH compression and GSSAPI
authentication are explicitly disabled. Existing authenticated sessions can
survive a listener restart.

Install `logrotate`, `/etc/n20-logrotate.conf`, and `n20-log-maintenance`
alongside the supervisor. The maintenance loop checks once per minute,
rotating daily or above 4 MiB and retaining four compressed generations for
the listed service logs. The limit is checked periodically, so fast writers
can exceed it between checks. `copytruncate` preserves the open inode used by
existing writers but has a small copy/truncate message-loss window; logs are
not lossless forensic records. Failures go to the protected maintenance log
and `/var/lib/n20/logrotate-error`; success clears that marker. Off-device log
collection is not configured.

## Wiki request admission

Chat requests and asynchronous map-search misses share an in-memory queue with
no request-count, per-player, or admission-rate limit. This allows bursts to wait,
but a large backlog can consume memory; the server whitelist is the access
boundary for submitting questions. Each question is limited to 256 characters.
Questions do not expire while waiting. A temporary lookup failure returns the
same request to the front with a 30-second retry delay and one private waiting
notice. Requests for a previous Minecraft process are discarded. A manually
supplied restart handoff is limited to 128 KiB. The model's resource guards and
the separately bounded marker command queues remain in place.

## Backup locking and recovery

Local snapshot creation wakes Drive **after** local retention, including when
cleanup fails, rather than racing a Drive restore against an exclusive
retention lock. Pre-restore checkpoints still wake Drive. Both clients use
Restic's bounded lock retry: short administration reads wait up to 30 seconds
inside their 120-second operation timeout; longer mutations and Drive
operations allow five minutes of lock waiting within their longer timeout.
No production lock is forcibly removed. An old dashboard error remains until
an operation succeeds; a new cleanup warning remains visible even when
snapshot creation succeeded.

Automatic snapshots still do not force saves or provide atomic filesystem
snapshots. Keep restore holds, retained old worlds and the pre-restore
checkpoint safeguards. Drive copies remain readable, as previously chosen,
and require a verified `COMPLETE.json` before recovery. Keep the repository
and its separate password independently off-device. Google sharing/account
security and a full cloud-to-game disaster recovery drill remain operator
tasks; checksum verification is not that drill.

## Physical access, credentials and maintenance

USB serial and the emergency shell still provide unauthenticated root. The
bootloader remains unlocked and verified boot disabled. These are real
physical-access risks. The only verified recovery interfaces were preserved;
no boot image, partition metadata, firmware or bootloader lock was changed.
Physically restrict the phone/data port and use a trusted USB host. Eliminating
these risks requires a separately tested recovery and platform migration,
not removing the sole recovery interface from this installation.

Closing raw-device access cannot retract anything read earlier. No active
credential theft was established. Assess SSH, Wi-Fi, Drive OAuth and backup
credential rotation from a trusted machine if compromise is suspected, while
preserving independent recovery access and encrypted-backup readability. No
credentials were printed, changed or copied into Git during remediation.

BusyBox init does not run systemd package-update timers. Review Debian
security updates and upstream Java, Minecraft, BusyBox, MTP and Wiki releases
at least monthly and after relevant advisories; check feature applicability
rather than equating every source-package CVE with a reachable vulnerability.
Keep credentials, signed modules, firmware and recovery materials outside Git
as described in [local materials](local-materials.md).

## Deployment and verification

Install the new helpers (including resource policy, health recording, bounded
stopper and Wiki supervisor), the logrotate configuration, legacy iptables and
logrotate **before** the updated supervisor. Missing required security helpers
fail normal service startup. Changing an installed Python file does not reload
its running worker. Safely reload both backup schedulers when idle; activate
Java/Wiki launch changes through a clean, zero-player stop/start or normal
maintenance reboot. A `--server-only` supervisor takeover must preserve the
existing FIFO and every recovery hold. Do not reboot or flash merely to test
these controls.

Run the new `test-security-devices.py`, `test-service-sandbox.py`,
`test-firewall.py`, `test-firewall-dhcp.py`, `test-log-rotation.py`,
`test-backup-locking.py`, and `test-wiki-admission.py` individually, plus the
relevant existing suites listed in the README. The device/firewall/sandbox
tests require root; firewall socket tests use two disposable network
namespaces. The private-path live test is read-only and checks denied raw-node
opens as both service identities, without reading storage contents.

The availability follow-up also uses `test-wiki-device-boundary.py`,
`test-wiki-resource-policy.py`, `test-service-health.py` and
`test-wiki-supervision.py`. These use fake nodes, synthetic pressure and
disposable children; do not pressure-test the production world to validate
memory guardrails.
