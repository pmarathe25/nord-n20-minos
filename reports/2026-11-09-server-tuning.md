# Server tuning, 2026-11-09

The strongest tested general-survival starting point is **view 24 / simulation
10**, a **512 MiB initial / 2 GiB maximum heap**, **G1**, and
**`-XX:ActiveProcessorCount=4`**, with **Fabric + Lithium**. Vanilla with the
same flag captures most of the CPU reduction if keeping the existing server
implementation is preferred. These are recommendations from disposable tests;
they have not been installed in production.

Simulation 4 from the earlier exploration test is a deliberate gameplay
tradeoff, not the recommended general survival default. Normal
[server defaults](https://docs.papermc.io/paper/reference/server-properties/)
are view 10 / simulation 10. Use simulation 8 as a lower-load alternative to
10; keep 4 for cases where its smaller active area is acceptable.

## Method

Minecraft Java 26.3, protocol 777, installed Java 25, one headless vanilla-protocol
client on the workstation, and fresh normal worlds with seed 73111. The phone
uses Wi-Fi SSH forwarding, with USB power connected. Plugged-in vendor current
readings are **not battery-power measurements** and are excluded from the
performance conclusions. Display, charging, Wi-Fi power policy, CPU governors
and thermal controls are unchanged.

Each full case has initial loading until 20 seconds of chunk-delivery silence
(minimum 30, maximum 240 seconds), 30 seconds stationary, 60 seconds moving
into new terrain, and 60 seconds retracing the route. Movement is a 16-block
console teleport every two seconds, averaging 8 blocks/second, not a physical
walking/flight simulation. The return trip benefits from generated terrain and
memory caches; it does not measure a cold pre-generated-world load.

Tick work is native `FULL_TICK - IDLE`, including scheduled work and excluding
intentional sleep. P95 and the percentage over the 50 ms tick budget measure
smoothness better than just mean tick time. CPU is Java process CPU seconds
per wall second (occupied cores); RSS includes native memory and committed but
possibly unused heap. Samples near phase boundaries have small timing uncertainty.

Production is stopped only after the existing idle check. Temporary servers
run as `minecraft`, with offline authentication confined to a verified loopback
listener. Temporary worlds are removed, the previous production state is
restored, and its configuration hash is checked. Real world data and settings
are not benchmark inputs.

## Profiles

All profiles start with a 512 MiB initial heap. Unless specified, the maximum
is 2 GiB and the collector is G1, verified as the installed JVM default.

| Profile | Change |
| --- | --- |
| Baseline | Vanilla |
| Heap 3 GiB | Only `-Xmx3G` |
| Parallel GC | Only `-XX:+UseParallelGC` |
| CPU count 4 | Only `-XX:ActiveProcessorCount=4`; changes multiple pool sizes, not core affinity |
| Lithium | Fabric Loader 0.20.1 + Lithium 0.26.3 for Minecraft 26.3 |
| Lithium + CPU count 4 | Same Lithium setup + `-XX:ActiveProcessorCount=4` |
| C2ME | Same Lithium setup + C2ME 0.4.2-alpha.0.116 for 26.3, default configuration |
| DH compatibility | Lithium + CPU count 4 + Distant Horizons 3.3.4 for 26.3, including its bundled Fabric API 0.160.5 |

Fabric Installer 1.1.2 prepares the isolated loader. Pinned Modrinth version
IDs are `fWv5bnZk` (Lithium), `aOP6eqZA` (C2ME), and `TGgEbP9A` (DH).
Artifact URLs and full integrity hashes are pinned in
[`prepare-benchmark-mods.py`](../tools/prepare-benchmark-mods.py). No mod binary
or production installation is committed to Git.

## View 24 / simulation 8 results

Loading includes the 20-second quiet period, but excludes JVM/server startup.
Tick values are mean / P95 milliseconds. RSS is the maximum sampled across
all four phases, not a minimum-memory requirement.

| Profile | Loading, s | New-terrain tick ms | Ticks >50 ms | New-terrain CPU cores | Peak RSS GiB |
| --- | ---: | ---: | ---: | ---: | ---: |
| Vanilla baseline | 109.8 | 28.1 / 58.0 | 8.45% | 4.02 | 2.43 |
| Heap 3 GiB | 112.8 | 28.4 / 57.9 | 8.71% | 4.12 | 1.61 |
| Parallel GC | 103.7 | 26.5 / 53.7 | 6.30% | 3.70 | 1.92 |
| CPU count 4 | 109.7 | 29.5 / 49.0 | 4.64% | 3.18 | 2.43 |
| Lithium | 107.7 | 26.5 / 52.4 | 6.30% | 3.78 | 2.18 |
| Lithium + C2ME | 84.6 | 29.6 / 52.2 | 6.55% | 3.60 | 2.25 |

| Profile | Stationary tick ms | Return-trip tick ms | Return-trip CPU cores |
| --- | ---: | ---: | ---: |
| Vanilla baseline | 21.5 / 25.8 | 22.1 / 33.8 | 1.16 |
| Heap 3 GiB | 21.3 / 25.9 | 22.5 / 36.4 | 1.21 |
| Parallel GC | 21.8 / 26.7 | 21.5 / 30.6 | 1.13 |
| CPU count 4 | 22.1 / 27.0 | 22.1 / 31.3 | 1.12 |
| Lithium | 18.0 / 21.6 | 21.2 / 30.9 | 1.08 |
| Lithium + C2ME | 17.5 / 21.0 | 20.5 / 31.8 | 0.90 |

The larger heap did not improve exploration latency. Its smaller observed RSS
does not establish a lower memory requirement: G1's committed heap and collection
decisions vary, and neither case needed to exhaust its maximum heap.

Four-processor pool sizing reduced exploration CPU by 21% and P95 tick work by
16%, while increasing mean tick work by 5%. Initial delivery took essentially
the same time. All four cases settled at 2,181 client-visible chunks and retained
that count through movement; each direction delivered 1,479 chunk packets.
There were no client protocol errors. Parallel GC gave a smaller improvement
in this single screening trial; it is not a demonstrated universal replacement
for G1.

Lithium reduced stationary mean tick work by 16% and exploration P95 by 10%
relative to baseline. C2ME's default executor resolved to four workers. It
shortened initial loading but raised initial-load P95 tick work to 158.3 ms,
versus 65.7 ms on vanilla and 62.6 ms with Lithium. Initial-load CPU averaged
5.86 cores, and the worst tick was 648 ms. Sustained exploration P95 was nearly
unchanged from Lithium alone, with a worse mean. It is not the preferred
interactive configuration from this screening; its loading throughput might
be useful for separate pre-generation work.

This C2ME build logged that its ARM64 native math library was unavailable and
disabled that accelerator. It also reported missing optional Fabric API and
ScalableLux integration targets; neither was installed in this profile. No
required dependency prevented startup, and no client error or server crash
occurred. Do not extrapolate benchmarks using native acceleration on x86 to
this phone. C2ME's worker count was not independently swept.

On baseline vanilla, the return trip used 71% less Java CPU than new-terrain
exploration, and P95 tick work fell from 58.0 to 33.8 ms. This supports moving
generation ahead of play in a bounded area. It does not mean pre-generation
cuts electrical power by 71%, or that cold loading will match a cached return trip.

## View 24 / simulation 10 follow-up

The view-24/simulation-10 follow-up keeps the first three phase durations and
omits the return trip. It compares vanilla against Lithium plus four-processor
pool sizing, followed by a vanilla four-processor control. The follow-up
therefore preserves the normal default simulation radius.

| Profile | Loading, s | Stationary tick ms | New-terrain tick ms | Ticks >50 ms | New-terrain CPU cores | Peak RSS GiB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Vanilla baseline | 110.8 | 23.3 / 28.5 | 27.2 / 58.8 | 8.79% | 4.02 | 2.37 |
| Lithium + CPU count 4 | 107.7 | 21.6 / 26.2 | 28.7 / 50.4 | 5.06% | 3.13 | 1.85 |
| Vanilla + CPU count 4 | 110.7 | 22.0 / 28.4 | 30.5 / 54.1 | 7.55% | 3.32 | 2.15 |

The combined profile reduced exploration CPU by 22%, P95 tick work by 14%,
and P99 from 91.6 to 65.8 ms. Mean tick work increased by 5%; this is a tail
latency and CPU-efficiency tradeoff, not an improvement in every statistic.
Both retained 2,181 visible chunks, delivered the same 1,479 new-terrain chunk
packets, and had no client protocol errors. The combined profile still had a
386.8 ms worst tick during initial loading, versus 192.9 ms on baseline, so it
does not eliminate occasional stalls. These short runs do not establish that
the lower observed RSS persists in a longer session.

The vanilla four-processor control captures most of the CPU reduction without
adding mods: 17% below baseline. Adding Lithium to that configuration reduced
exploration CPU by a further 6%, mean tick work by 6%, P95 by 7%, and P99 from
84.3 to 65.8 ms. Stationary means were close, and stationary CPU was slightly
higher with Lithium in this short sample. Lithium plus four-processor pool
sizing is the strongest tested general-survival starting point, with vanilla
plus the same flag as a simpler alternative. Neither establishes a universal
optimum for a developed world or multiple players.

A subsequent source review found documented, default-enabled behavior differences
in this Lithium release. The performance recommendation does not establish
vanilla equivalence; see the [versioned optimization reference](https://github.com/CaffeineMC/lithium/blob/mc26.3-0.26.3/lithium-fabric-mixin-config.md)
and [compatibility guidance](../docs/server-tuning.md).

## Distant Horizons compatibility

A separate 45-second compatibility run used view 24 / simulation 10, Lithium,
four-processor pool sizing, and DH 3.3.4 with its bundled Fabric API. The client
advertised no Fabric or DH capabilities and sent no DH requests. It joined,
received ordinary terrain, acknowledged movement from X=0.5 to X=64.5, and
remained connected without protocol errors. At the end it had 625 visible
chunks; initial loading had not finished. The server stopped cleanly and the
production restoration check passed.

This confirms optional-client compatibility for this tested setup: an unmodded
protocol client can use the server without receiving DH's extended horizon.
It is not a test with the official rendered client, and the short, unsettled
phases are deliberately excluded from the performance tables. They establish
neither DH's steady overhead nor 256-chunk LOD performance. DH can still build
LOD data from ordinary loaded chunks even without a DH client requesting
distant terrain. Its G1 warning was present; the collector recommendation above
is for the measured ordinary Minecraft workload, not a validated choice for
heavy DH generation.

Client-only DH preserves explored terrain without installing DH on the phone.
Server-assisted distant generation needs a separate load test with DH requests.
A rendered DH client is needed to judge the horizon and frame rate. A 256-chunk
LOD radius does not require a 256-chunk full-detail server view or simulation
radius. See the [project's multiplayer description](https://modrinth.com/mod/distanthorizons).

## Restoration and checks

Nine complete performance cases and the DH compatibility case produced 36
measured phases, with no client protocol errors. All five experiment batches
restored production successfully and passed the configuration-hash check.
Every phase reported USB connected and AC absent; these are performance tests,
not battery-only comparisons. Maximum sampled battery temperature was 32.4°C,
and minimum available system memory was 2,523 MiB. Battery temperature is not
CPU die temperature.

After the final restoration, the real server remained configured at view 12 /
simulation 12 with `pause-when-empty-seconds=60`, and its log confirmed the
normal empty-server pause. The stop marker was absent. Brightness was zero,
Wi-Fi power saving was on, and the existing charge cap was active. The unchanged
dashboard process recorded zero CPU ticks and zero context switches over a
30-second screen-off interval. No temporary benchmark worlds remained.

The protocol fixture tests and JavaScript syntax check passed. Both changed
Python tools compiled on the phone; invalid-profile and existing-asset refusal
checks passed. The pinned asset preparation also completed from an empty scratch
directory, with downloaded hashes verified. Documentation links and whitespace
checks passed. Downloaded mods, installer files, disposable worlds, remote
scripts and raw logs were removed after the summarized findings were recorded. No production mod,
launcher flag, server setting or power policy is installed by this work.

## Limits of inference

These short, sequential trials are a screening comparison, not an exhaustive
optimization or multiplayer capacity study. World caches, JVM warm-up, natural
spawning and temperature can affect results. A lower mean does not establish
better responsiveness if the tail gets worse. This test does not validate
farms, combat, player-built redstone or all mod/backup interactions.

See [benchmark instructions](../docs/benchmarking.md) and
[available tuning controls](../docs/server-tuning.md). Raw output and downloaded
assets are temporary; only these summarized findings and reusable tools belong
in Git.
