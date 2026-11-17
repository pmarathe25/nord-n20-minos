# Wiki paging admission correction — 2026-11-16

The owner reported that Wiki was unavailable and requested startup without a
physical free-memory requirement. Live health history showed repeated
`sustained low RAM reserve` stops, including the latest model attempt. At
inspection Minecraft had two players, approximately 2.7 GiB available RAM and
4.9 GiB free swap after the model had stopped. The bridge and supervisor were
running; the demand-loaded model was absent. The fixed RAM guard was stopping
the model, not evidence that swap was exhausted.

Removed the launcher's 768 MiB physical admission floor, its immediate
256 MiB stop and its sustained 384 MiB stop. Also removed the chat bridge's
independent 208/980 MiB physical floors and corrected its startup-status check.
Both now permit zero reported `MemAvailable` when swap supplies enough total
headroom. Startup still requires 2 GiB free swap and 3 GiB combined available
RAM plus free swap; ongoing work retains the 768 MiB swap / 1 GiB combined
checks. Sustained PSI, temperature, cooldown, cgroup and OOM-preference controls
remain. Swap size, swappiness, JVM heap and Minecraft configuration are unchanged.

The existing on-demand model lifecycle is preserved. An idle model being off
is expected; a request needing inference starts it. The model is released after
the existing idle timeout.

## Verification and deployment

- ARM64 policy tests cover admission and continued operation at zero available
  RAM with sufficient swap, rejection when combined capacity is insufficient,
  and retained sustained-pressure/cooldown behavior.
- Wiki chat: 31 tests; demand lifecycle: 4; map chat: 82, all passed. Bridge
  tests cover zero-RAM paging, missing measurements and the temperature guard.
- Changed Python files passed compilation; launcher passed `sh -n`;
  `git diff --check` passed.
- Deployed the resource policy, Wiki launcher and bridge module after checking
  installed originals against repository HEAD and confirming the Wiki queue was
  idle. Reloaded only Wiki. Minecraft's Java PID remained 8600 and its status
  stayed online with two players.
- A temporary, validated demand marker started the model through the normal
  launcher. The Unix-socket health endpoint became ready and a one-token GPU
  completion succeeded without sending a player chat message. The marker was
  removed afterward without replacing any player-created demand.
- During this functional check, minimum available RAM was 549,144 KiB (about
  536 MiB) and maximum swap use 1,144,508 KiB (about 1.09 GiB). Minecraft, model
  and bridge cgroup limit-failure and OOM-kill counters were all zero. The
  resource policy's current reason was empty.

This verifies functionality, not heavy-gameplay latency or successful allocation
at literally zero physical headroom. The zero-headroom cases used synthetic
measurements. Linux can reclaim cache and swap anonymous pages, but not all
driver/kernel memory is reclaimable; free swap cannot guarantee allocation
success. See the kernel's [memory concepts](https://docs.kernel.org/admin-guide/mm/concepts.html)
and [pressure metrics](https://docs.kernel.org/accounting/psi.html).

Root-only rollback copies of the three public runtime files remain under
`/var/lib/n20/wiki-swap-rollback-20261117T022525Z` (UTC timestamp; November 16
local time). Temporary test sources, verification helpers and archives were
removed after validation. No firmware, world, credential or backup changes were
made. A pre-existing busy-world backup postponement was left intact; this work
did not bypass snapshot safeguards.
