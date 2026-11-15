# Minecraft availability and Wiki isolation — 2026-11-15

The owner requested the next security/availability priorities with Minecraft
as the phone's primary purpose, while keeping Wiki usable alongside it.
Changes were tested on ARM64 Debian and installed on the running phone.
This supplements the earlier [security audit and remediation](2026-11-15-security-audit.md).
Current durable policy is in [security controls](../docs/security.md).

## Result

- Minecraft and Wiki returned online after a fresh zero-player check and a
  normal console stop/save. FIFO identity and all recovery holds were
  preserved. No phone reboot, production restore or extra game server ran.
- The live bridge has only supplementary socket GID 61125. Its actual dropped
  identity connected successfully to the model's health endpoint and failed
  KGSL/ION read/write opens. The model retains UID/GID 61124, no supplementary
  groups, and GPU access; a real one-token GPU completion succeeded.
- Java and both Wiki processes retain Seccomp 2, NoNewPrivs 1 and empty
  effective/bounding capabilities. Java is outside the optional-only OOM
  hierarchy. New charged-RAM and combined RAM-plus-swap limits were verified
  in the running kernel, with no allocation-limit failures or cgroup OOMs.
- The automatic backup worker completed the tenth snapshot following the
  clean restart; its uploaded state and Drive completion marker were checked.
  The final refreshed phone state was online, zero players, ten snapshots,
  and empty operation/error fields. A transient changed-world copy was safely
  rejected during restart; an overlapping manual request was also rejected
  while cleanup was busy. Successful normal retention/cache refresh cleared
  that stale busy diagnostic after the automatic operation finished.

## Memory budget and swap decision

| Group | Charged RAM ceiling | Combined RAM plus swap ceiling | oom_score_adj |
| --- | --- | --- | --- |
| Minecraft | 4 GiB | 6 GiB | -250 |
| Optional Wiki parent | 3.5 GiB | 4.5 GiB | Child preferences below |
| Wiki model | 3.5 GiB | 4 GiB | +900 |
| Wiki bridge | 256 MiB | 384 MiB | +600 |

The optional parent's OOM selection cannot kill Minecraft. Java's 6 GiB
combined ceiling retains substantial room above the earlier audit's roughly
2.8 GiB observed RSS-plus-swap sample. It is not a fixed 2 GiB swap quota.
The maximum Java heap remains 2 GiB. Minecraft is not OOM-immune; OS/SSH must
remain recoverable. Shared/file-backed page ownership and vendor GPU memory
mean cgroup charges do not describe all physical pressure, so global guards
remain necessary.

The initial conservative 1 GiB physical yield proposal was revised after the
owner explicitly requested that useful Wiki paging remain possible. The
installed policy allows healthy paging below 1 GiB available RAM:

- Admission: at least 768 MiB available RAM, 2 GiB free swap and 3 GiB combined
  headroom, healthy for 30 seconds. Old worker/bridge tasks block duplicate
  launches while cleanup is pending.
- Running: immediate yield below 256 MiB available RAM; five consecutive
  checks below 384 MiB; or three checks below 768 MiB free swap/1 GiB combined
  headroom. Ten consecutive checks of memory avg10 PSI full >=5%, some >=20%,
  or IO full >=25% also yield. Healthy samples reset individual counters.
- Recovery: 120-second post-yield cooldown, then a new 30-second healthy
  admission window. Cold pages in Java swap do not independently cause a
  Wiki stop. Temperature guards remain.

These are initial measured guardrails, not a proven guarantee against crashes
or tick stalls. Tests used synthetic samples, including 800 MiB admission and
500 MiB running with healthy paging, transient versus sustained stalls,
counter resets, emergency floor and recovery hysteresis. No production OOM
or memory-pressure stress was induced.

**Keep the existing 6 GiB swapfile.** Before changes, MemAvailable was about
3.7 GiB, only about 225 MiB of swap was used, all memory/IO PSI averages were
zero, and the kernel OOM count and service limit-failure counts were zero.
About 93 GiB storage was free. After deployment, thirty one-second idle
samples recorded minimum MemAvailable **3,853,392 KiB**, maximum swap used
**339,872 KiB** (about 332 MiB), memory PSI avg10 maxima **0**, and IO some/full
avg10 maxima **0.18%**. This was an idle check around service activation and a
small GPU functional request, not a gameplay benchmark or workload peak.

There was no evidence of swap exhaustion warranting more flash-backed swap.
Extra swap would not enlarge the Java heap or remove paging latency, and can
trade an OOM for long stalls. The 6 GiB file, swappiness 10 and active swap
configuration were preserved; no resize, new swapfile or swapoff was done.

## Supervision and diagnostics

The Wiki launcher now has single-instance outer supervision, resource-gated
restart backoff from 30 seconds up to 15 minutes, and stable-run reset. The
model retains profile fallback with bounded 30–300 second backoff. Its exit
is checked before pressure handling so an OOM is not mislabeled as a routine
pause. TERM waits five seconds before KILL, then waits at most another two;
unreaped kernel tasks block replacement workers. Launcher cleanup is also
bounded. An uninterruptible driver/kernel task may still require operator
recovery; userspace cannot promise to free it.

Minecraft's initial restart delay remains ten seconds, rising to at most two
minutes for repeated short failures; a clean exit or five-minute stable run
resets it. Existing recovery/battery holds still prevent launches.

Root-only service health records include current role state and 32 incident
entries. Identical holds are deduplicated. Leaf and optional-parent OOM
counters, system OOM count since launch, bounded Java heap/native allocation
error signatures, and ordinary exit/signal statuses are distinguished. A
system-wide OOM counter increase does not establish which process it killed.
Existing rotated logs retain fuller context.

The existing dashboard attention notice now displays the Minecraft restart
reason, Wiki recovery or actual Wiki pause reason. These notices do not mark
a backup/server operation busy. Existing backup errors and restore/battery
holds take precedence. No native dashboard rebuild was needed.

## Available network checks and limitation

The owner confirmed that no off-LAN test access was available. Tool and
trusted-local-material discovery found no connected remote executor/router
control session. No public scanning service received the home endpoint.

A bounded, read-only check reached the router's HTTP login landing page;
HTTPS was unavailable in that check. Gateway-unicast and multicast SSDP IGD
discovery returned no gateway response. This does **not** prove UPnP is
disabled or enumerate mappings behind authentication. No login guessing,
router setting change or mapping change occurred.

From the existing LAN, targeted connections to the phone's global IPv6 address
reached Minecraft TCP25565, while TCP22 and management TCP25585 were not
reachable. This is **LAN-to-global-address verification, not WAN verification**.
The existing host firewall intentionally permits the game port; no player
exposure or router policy was changed. Actual Internet reachability, NAT
forwarding, router IPv6 policy and UPnP mapping state remain unverified until
the owner has trusted off-LAN testing/router access.

## Validation and deployment

Passed individually on ARM64:

- `test-wiki-device-boundary.py`: real dropped identities, fake character
  node, setgid socket inheritance, successful bridge request and denied GPU.
- `test-wiki-resource-policy.py`: healthy paging, admission, consecutive-check
  resets, sustained stalls, emergency yield and cooldown/recovery hysteresis.
- `test-service-health.py`: bounded/deduplicated incidents, heap-signature
  filtering and leaf/optional-parent OOM attribution.
- `test-wiki-supervision.py`: bounded backoff, actual TERM-resistant disposable
  child escalation/reaping, and restart/pressure-gate integration.
- `test-service-sandbox.py`: real seccomp, memory/memsw ceilings and OOM score
  in a small disposable child, with no game server.
- Existing admin settings (including visible health notice and precedence),
  Wiki chat (29 tests), supervisor startup ordering, recovery/battery holds
  and private-EULA/FIFO guards, and read-only live private-path/GPU checks.
- Python compilation and shell syntax for changed files; `git diff --check`.

Eight public runtime files were deployed: `n20-resource-policy`,
`n20-service-health`, `n20-stop-process`, `n20-wiki-supervisor`,
`n20-service-sandbox`, `n20-wiki-start`, `minecraft-supervisor`, and `n20-admin`.
Protected public rollback copies remain outside Git in
`/var/lib/n20/availability-rollback-20261115`. Disposable source, gateway HTML,
measurements and raw outputs were removed after recording these findings.
Credentials, Wi-Fi configuration, worlds/backups, firmware, partitions,
bootloader state and physical recovery paths were preserved.
