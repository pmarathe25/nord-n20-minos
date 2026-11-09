# Chunk-distance benchmark, 2026-11-09

Measurements use this Nord N20 and the installed vanilla Minecraft Java 26.3
server, with its usual Java 25 runtime and `-Xms512M -Xmx2G` heap settings.
The production world and configuration are not benchmark inputs.

For one player exploring new terrain, **24 view / 4 simulation** is the best
tested starting point for a wide view with more tick-time headroom. Use
simulation 8 if distant farms or other activity need it. View 32 was affordable
when stationary, but more expensive and less smooth while generating terrain.
These are workload-specific recommendations, not guaranteed limits.

## USB performance results

All rows below use simulation distance 8. Tick values are mean / P95 in ms;
memory is maximum Java process RSS across the case, in GiB. Exploration is the
90-second new-terrain route, not stationary play. CPU is occupied cores.

| Configured view | Settled visible chunks | Stationary tick ms | Exploration tick ms | Exploration ticks >50 ms | Exploration CPU | Peak RSS GiB |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 12 | 637 | 18.4 / 22.7 | 26.3 / 51.8 | 5.7% | 3.02 | 1.05 |
| 16 | 1,057 | 20.3 / 25.0 | 26.5 / 50.7 | 5.2% | 3.40 | 1.15 |
| 24 | 2,181 | 21.4 / 25.9 | 27.4 / 57.4 | 7.1% | 3.97 | 2.40 |
| 32 | 3,725 | 23.0 / 29.1 | 32.9 / 71.7 | 14.1% | 5.19 | 2.47 |
| 256 (effective 32) | 3,725 | 23.6 / 28.8 | 34.4 / 81.1 | 16.3% | 5.21 | 2.41 |

The 256 case advertised 256 in its login packet, but delivered exactly the same
settled chunk count as 32, even with the client requesting 127. This is a
measured cap check, **not a benchmark of a true 256-chunk-radius world**.
The 32 and configured-256 runs finished exploration with only 3,589 and 3,543
visible chunks respectively, below the settled 3,725: terrain delivery was
behind the movement. The 12, 16 and 24 runs ended with their full settled counts.
The configured-256 run also had a roughly 1-second worst tick during exploration.

| Configured view | Initial delivery + settling, s | Initial stream MiB | Exploration stream MiB | Peak battery °C |
| ---: | ---: | ---: | ---: | ---: |
| 12 | 54.3 | 2.93 | 7.34 | 27.3 |
| 16 | 68.4 | 4.95 | 9.58 | 28.6 |
| 24 | 109.6 | 10.50 | 14.00 | 30.0 |
| 32 | 158.8 | 18.66 | 17.68 | 31.1 |
| 256 (effective 32) | 163.8 | 18.77 | 17.40 | 31.8 |

Initial delivery includes the 20-second quiet period, but excludes server
startup (about 25–27 seconds) and work before the player joins. Temperature
reflects sequential runs, not independent equilibrated thermal tests.

### Simulation comparison at view distance 16

| Configured simulation | Stationary tick ms, mean / P95 | Exploration tick ms, mean / P95 | Exploration ticks >50 ms | CPU stationary / exploration | Peak RSS GiB |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 4 | 14.8 / 19.3 | 23.2 / 45.8 | 3.8% | 0.42 / 3.24 | 1.15 |
| 8, first | 20.3 / 25.0 | 26.5 / 50.7 | 5.2% | 0.56 / 3.40 | 1.15 |
| 16 | 18.9 / 25.5 | 28.6 / 65.4 | 12.3% | 0.64 / 3.43 | 1.17 |
| 32 | 18.2 / 23.6 | 29.3 / 68.4 | 12.9% | 0.63 / 3.45 | 1.10 |
| 8, repeat | 20.1 / 24.9 | 26.6 / 52.5 | 5.9% | 0.55 / 3.37 | 1.10 |

All these cases delivered the same 1,057 settled visible chunks. The first and
repeated 16/8 results were close, while simulation 4 reduced stationary CPU
and tick work. Exploration improved less because terrain generation remained.
The simulation-16 and -32 cases had worse exploration tails, despite stationary
means not being monotonic. Mob behavior, asynchronous work and garbage
collection make a simple area-to-tick-time formula unreliable.

These are configured simulation values with **view fixed at 16**. They are not
proof that a full 32-chunk-radius simulation area was loaded, and do not replace
a 32-view / 32-simulation or player-built-farm stress test.

The first two cases ran in one session, and the remaining seven in a second
session, with production restored between sessions. A preliminary 8/8 harness
smoke test and an aborted startup are excluded from the tables.

## Battery-only results

All four completed cases kept the display off and both external-power flags at
zero. The existing Wi-Fi policy selected `Power save: off` during each phase.
The table contains **raw vendor-current means**; calibration and conversion
to electrical units have not been validated.

| View / simulation | Initial loading current proxy | Stationary current proxy | Exploration current proxy | Peak battery °C |
| --- | ---: | ---: | ---: | ---: |
| 24 / 8, first | 602.0 | 166.6 | 535.3 | 30.0 |
| 24 / 4 | 633.1 | 149.8 | 511.9 | 31.3 |
| 32 / 8 | 659.7 | 170.8 | 681.3 | 32.2 |
| 24 / 8, repeat | 623.2 | 180.9 | 545.1 | 32.8 |

| View / simulation | Stationary tick ms, mean / P95 | Exploration tick ms, mean / P95 | Exploration ticks >50 ms | Exploration CPU | Peak RSS GiB |
| --- | ---: | ---: | ---: | ---: | ---: |
| 24 / 8, first | 21.4 / 25.2 | 28.0 / 56.9 | 7.5% | 4.02 | 2.20 |
| 24 / 4 | 15.3 / 18.6 | 25.9 / 48.2 | 4.4% | 3.82 | 2.45 |
| 32 / 8 | 23.1 / 29.4 | 33.9 / 79.8 | 14.6% | 5.29 | 2.45 |
| 24 / 8, repeat | 21.9 / 26.4 | 28.2 / 59.9 | 8.5% | 3.98 | 2.47 |

The first and repeated 24/8 runs bracket the comparisons:

- At view 24, simulation 4 reduced the measured stationary current proxy by
  about **10–17%**, and the exploration proxy by about **4–6%**. It kept the same
  2,181 visible chunks and improved the exploration P95 tick work to 48.2 ms.
- At simulation 8, view 32 raised the exploration current proxy by about
  **25–27%** relative to view 24. It finished with 3,578 of the settled 3,725
  visible chunks, so terrain delivery was still behind movement.
- The stationary current proxy at 32/8 falls inside the two 24/8 readings.
  This experiment does **not** resolve a meaningful steady-draw penalty for
  increasing view from 24 to 32 once loaded. Memory use and new-terrain work
  remain different.
- Lowering simulation did not reduce initial-generation current consistently.
  That phase still generates the same visible terrain. Initial loading also
  lasted about 107–112 seconds at view 24 versus 160 seconds at view 32,
  including the settling interval.

The repeated 24/8 current means differed by 8.6% while stationary and 1.8%
while exploring. Temperature, voltage, mob activity, JVM behavior and coarse
telemetry limit precision. The percentages above are observations of this
counter, not calibrated electrical-power savings or battery-life predictions.
Reported battery capacity went from 95% at the first measured loading phase to
90% at the last exploration phase; this mixed, short run is not a runtime test.

An earlier battery attempt stopped at the display-state guard during
exploration and restored production. Its incomplete case is excluded from
these tables; the four completed reruns supplied all results above.

### Idle and restoration checks

Each monitored idle baseline lasted 60 seconds and contained 61 samples, with
the display off and Wi-Fi power saving on.

| Idle sample | Mean current proxy | Minimum / maximum | Battery °C, start / end | Capacity |
| --- | ---: | ---: | ---: | ---: |
| Before | 64.8 | 52 / 97 | 28.0 / 27.6 | 96% |
| After | 67.2 | 54 / 133 | 29.8 / 29.1 | 90% |

The final idle check confirmed that the real server had reached its normal
60-second empty-server pause. The dashboard accumulated zero CPU ticks and
zero voluntary plus involuntary context switches during the minute. Wi-Fi
power saving was on, brightness was zero, and charging was permitted as required
by the existing policy while unplugged. Production was restored online with
its original view 12 / simulation 12 settings and an unchanged configuration
checksum. No recommended gameplay setting was applied by the benchmark.

## Method

The workstation runs a minimal protocol-777 client through an SSH tunnel,
using USB for the performance sweep and Wi-Fi for the battery comparison.
Each case starts a new disposable normal-terrain world with seed 73111,
difficulty normal and one creative-mode player at `(0.5, 100, 0.5)`. The server
binds only to `127.0.0.1:25665`; authentication is offline only in that temporary
instance. Listener ownership and address are checked before connecting. The
real server is gracefully stopped after checking that it has no players, then
restored when the sweep exits. Its settings are compared before and after.

The client completes login/configuration, consumes compressed chunk packets,
acknowledges chunk batches and teleports, answers keepalives, and subscribes to
the server's native tick-time debug samples. It does not render, decode terrain
into a world model, simulate player physics, fight, or run farms. Protocol
layouts were checked against [minecraft-data PR #1345 at a pinned commit](https://github.com/PrismarineJS/minecraft-data/tree/312f3cada17a7d9e2e1a1834fe26f17296163096/data/pc/26.3).
The server identified its own protocol as 777. Packet framing, compression,
fragmentation, chunk unloads and the 26.3 teleport acknowledgement have an
isolated test in `tests/test-chunk-client.js`.

Each case has three phases:

1. **Initial load:** begins after joining, lasts at least 30 seconds, and stops
   after 20 seconds without another chunk packet, up to 240 seconds. This
   includes generation and a settling period, but excludes JVM startup and
   terrain generation before the join. Quiet delivery is a heuristic, not proof
   that every background task has finished.
2. **Stationary:** 60 seconds at the starting position. A settled sample should
   receive zero new chunk packets during this phase.
3. **Exploration:** 90 seconds, with a console teleport of 16 blocks along +X
   every two seconds, equivalent to an average of 8 blocks/second. These are
   bursts of movement through new terrain, not a walking or flight simulation.

The main sweep requests 32 chunks from the client, except that the configured
256 case requests 127 to check the server cap independently. The server pairs
are 12/8, 16/8, 24/8, 32/8, 256/8, 16/4, 16/16, 16/32, and a repeat of 16/8.
View and simulation values are radii in chunks. A configured 256 value must
not be reported as an effective 256-chunk benchmark on this vanilla server.

Tick work is `FULL_TICK - IDLE` from native debug samples, in milliseconds.
The installed `TpsDebugDimensions` enum was inspected to confirm dimension
order: full tick, server tick method, scheduled tasks, idle. This includes
scheduled work and excludes deliberate sleep. Console `tick query` summaries
provide a cross-check; they cover only the last 100 ticks, whereas the client
collects the phase's samples. Delivery near phase boundaries adds a small
timing uncertainty. The 50 ms threshold corresponds to the 20 TPS tick budget.

CPU is Java process CPU seconds divided by wall seconds, expressed as occupied
cores; 1.0 means one fully occupied core, not 100% of this eight-core phone.
Memory is maximum sampled resident memory, sampled once per second. Packet
bytes are compressed Minecraft stream bytes before SSH overhead. Client-visible
chunk counts exclude server-only generation margins and simulation chunks.
Resident memory includes native allocations and committed heap that may contain
garbage or unused space; it is not a measurement of the minimum heap required
by those chunks. A 2 GiB Java heap limit is not a 2 GiB process-memory limit.

The display stays off. Charging, Wi-Fi power policy, thermal controls and all
other device policies are unchanged. The USB performance sweep uses
USB/loopback traffic and does not measure gameplay Wi-Fi latency or radio
energy. The separate battery comparison below uses Wi-Fi. Battery temperature
is not CPU temperature. The vendor current counter is uncalibrated: unplugging
permits a relative comparison, but absolute watts or battery-life estimates
still require calibration or an external measurement.

The harness stops on battery temperature reaching 42°C, available system memory
falling below 600 MiB, a changed display state, a failed client, or an exited
server. These are experiment abort thresholds, not new device power policy.

### Battery-only comparison

The battery-only comparison uses Wi-Fi SSH after physically disconnecting USB,
with cases 24/8, 24/4, 32/8 and a repeat of 24/8. The same phase durations, world
seed and movement route apply. `--require-unplugged` requires both USB and AC
online flags to remain zero throughout measurement. Each phase records mean,
median, minimum and maximum raw battery-current readings, voltage, capacity,
battery temperature and Wi-Fi power-save state.

The existing Wi-Fi policy selects its low-latency mode while the production
server is stopped, because its normal server listener cannot be verified.
This is its existing fail-open behavior, not a benchmark override. The current
readings include the whole phone, Wi-Fi and SSH encryption overhead. They are
relative vendor-current measurements, not calibrated watts, and they should
not be compared directly with the plugged-in current readings. A monitored idle
sample before and after the sweep checks the baseline; it still includes SSH
telemetry traffic. Repeating 24/8 helps reveal drift rather than proving an
exact causal power multiplier.

## Reproduce

Run from the original Windows checkout with Node.js and OpenSSH. The private
identity remains in ignored `local/`. This is explicitly a disruptive benchmark
and requires an idle, running production server and the existing accepted EULA.
Do not run it against an arbitrary server version or simultaneously with another
benchmark. Verify enough disk space and keep the SSH connection available.

```powershell
ssh -i local/secrets/n20_usb_key root@192.168.137.2 "mkdir -m 700 /run/n20/chunk-benchmark"
scp -i local/secrets/n20_usb_key tools/benchmark-chunk-server.py root@192.168.137.2:/run/n20/chunk-benchmark/
ssh -i local/secrets/n20_usb_key root@192.168.137.2 "python3 -m py_compile /run/n20/chunk-benchmark/benchmark-chunk-server.py"
node tests/test-chunk-client.js
node tools/benchmark-chunk-host.js --cases 12:8,16:8,24:8,32:8,256:8,16:4,16:16,16:32,16:8
```

`--durations 240,60,90` changes the three phase durations. `--client-view 127`
requests the largest positive value representable by the client's signed-byte
view-distance field, which is useful for checking a server cap independently
of the usual client request of 32. It does not enable a true 256-chunk view.
`N20_BENCHMARK_SCRIPT` can select a separately staged script under `/run/n20/`.
For a battery comparison, set `N20_BENCHMARK_HOST` to the phone's verified Wi-Fi
IPv4 address, physically disconnect external power, and add
`--require-unplugged --cases 24:8,24:4,32:8,24:8`. Keep the display off and the
phone on the same surface. This does not change charging or Wi-Fi policies.

Keep the summarized measurements and reusable tools; discard temporary worlds,
raw logs, downloaded protocol data and scratch measurement files after review.
The server harness deletes its temporary worlds automatically.

## Limits of inference

This is a one-player terrain workload, not a capacity guarantee. Established
farms, redstone, combat, multiple separated players and client rendering can
behave differently. A fresh world makes view-distance generation work visible;
it is not a representative player-built base. Sequential runs share filesystem
caches and warming device temperatures. The repeated 16/8 performance case and
24/8 battery case check drift but do not replace randomized, repeated trials.
Do not translate a chunk-area ratio or CPU ratio into a battery-life ratio.

For much longer vistas, [Distant Horizons](https://modrinth.com/mod/distanthorizons)
supports 256+ chunk horizons using simplified distant terrain. Its client-only
mode retains terrain already explored; with compatible server support it can
receive distant terrain automatically. That is a different workload from full
vanilla chunks and has not been measured by this harness. A compatible rendered
client is needed to judge its visual quality and frame rate.

[Paper's maximum-view-distance override](https://docs.papermc.io/paper/reference/system-properties/#papermaxviewdistance)
is another existing option, but raising that limit does not itself establish
256-chunk client support or acceptable performance on this phone. No production
server replacement is part of these tests.
