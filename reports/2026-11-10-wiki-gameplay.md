# Nord N20 Wiki chat gameplay trial, November 10, 2026

The tuned profile sustained about 20 tick samples per second in every
one-client comparison. Stationary p99 rose by about 4–5 ms and all sampled
stationary ticks stayed within the 50 ms budget. On a repeat movement route,
the model increased p95 from 24.55 to 30.09 ms, with four ticks over budget
during the matched window versus five in the baseline. These results leave
the no-noticeable-lag requirement unverified for normal gameplay.
Three sourced replies arrived in 32.20–45.70 seconds. The fourth request
timed out during decode.

## Tick and reply times

| Workload | Chat to reply | Ticks, baseline / request | P50, baseline / request | P95, baseline / request | P99, baseline / request | Max, baseline / request | Over 50 ms, baseline / request |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Stationary, idle wake | 45.70 s | 914 / 914 | 18.43 / 18.80 ms | 19.59 / 23.37 ms | 20.90 / 25.94 ms | 46.15 / 44.16 ms | 0 / 0 |
| Movement, first route traversal in baseline | 32.20 s | 644 / 644 | 19.87 / 20.25 ms | 55.51 / 26.19 ms | 113.52 / 35.84 ms | 194.66 / 73.14 ms | 40 / 4 |
| Stationary, worker awake | 34.47 s | 690 / 690 | 18.71 / 19.31 ms | 20.33 / 23.37 ms | 21.97 / 26.03 ms | 38.52 / 49.78 ms | 0 / 0 |
| Movement, repeat route | 75.13 s, timed out | 940 / 940 | 19.62 / 22.08 ms | 24.55 / 30.09 ms | 43.34 / 45.90 ms | 61.31 / 65.62 ms | 5 / 4 |

Worker startup took 23.18 seconds from launcher request to a healthy socket.
During that interval the stationary client received 463 tick samples. Tick
work was 24.56 ms at p95 and 28.94 ms at p99, with a 31.76 ms maximum and
no tick over 50 ms. This startup used a partially warmed shader cache.

The cold question asked how strong an observer's redstone signal is. The
bridge returned `15` with an Observer Behavior link after 32.62 seconds of
prefill and 5.80 seconds of decode. The full request includes model reload.
The first movement request asked how long an observer's pulse lasts and
returned `2` with the same section. The first movement baseline traversed the
route edges for the first time. Chunk delivery and generation confound that
pair and prevent attributing the lower tail to inference.
The second stationary request asked how bookshelves affect enchanting. The
model cited Bookshelf Enchanting and gave the maximum level, `30`. It left out
how the count of shelves affects that level.
The lightning rod request on the repeat route returned the bridge's
unavailable message. The worker had generated 124 tokens by cancellation.
The table compares the first 47.01 seconds against the equal-duration
baseline. Across the full 75.13 seconds, the client received 1,503 tick
samples, with p95 30.43 ms, p99 45.10 ms, a maximum of 65.62 ms and seven
ticks over 50 ms. The repeat movement baseline had six gaps over 100 ms
between received tick packets, versus four during matched inference.

## Method

The Nord N20 ran a disposable vanilla Minecraft Java 26.3 server with Java 25,
the production heap settings of 512 MiB initial and 2 GiB maximum, view distance
24 and simulation distance 10. The disposable server bound to its own loopback
port. The workstation ran the repository's protocol client through USB SSH and
requested view distance 24. The client received 2,181 chunks before the timed
runs. Automatic saves were off in the disposable server during measurement.

The offline bridge followed the disposable server's real chat log. The bot sent
unsigned player chat, and the reply passed through the Minecraft account's
console FIFO and came back in a private system-chat packet. The Rust retriever
used a copy of the on-phone Wiki cache. Each question was checked against the
retriever and exact-answer route before the run to confirm it would require the
model. The launcher used Android ARM64 llama.cpp b12110, the OpenCL, server and
disk-prefix patches, the Bonsai 2 4B group-64 Q2_0 model and the C.24 Adreno 619
driver. The profile used the tuned 64-column prefill, custom decode, wide
vocabulary tile, direct packing and recurrent copy, width-512 Hadamard
transform, and single-turn disk-prefix option. The worker ran at nice 15 with
two efficiency-core threads. The trial used the existing tuned profile from
the [launcher comparison](2026-11-10-wiki-tuned-launcher.md).

Each request followed a baseline without a Wiki query in the same game
workload. The first query woke the worker from its 90-second idle sleep. The
remaining queries used an already-loaded worker. For movement, the client sent
position updates along a 32-block square at about 4.3 blocks per second in
creative flight. The route started at the same position in both arms. The
stationary and movement windows used the same disposable world and followed
one another in fixed order.

The client used native tick debug samples, with active work calculated as
`FULL_TICK - IDLE`. The 50 ms threshold is the server's 20 TPS tick budget.
Tick timing starts at the client's outgoing question packet and ends at its
incoming Wiki reply. Paired baseline and inference summaries use equal
wall-clock durations and their earliest samples. The client also counted
gaps longer than 100 ms between successive tick samples as a transport-level
cross-check.

## Memory and production lifecycle

The attempt with the idle production JVM still running reached 965 MiB of
available memory during model startup and stopped at the 1,000 MiB guard.
The completed run temporarily stopped production through its normal admin
lifecycle after two zero-player checks. Available memory in the completed
scratch run stayed at or above 1,347 MiB and battery temperature peaked at
27.4 °C. Production returned online with zero players and its whitelist
enabled and enforced. The restart took 22.49 seconds after releasing the
stop marker. Total elapsed time from the graceful stop request to readiness
was 628.65 seconds. The whitelist hash matched before and after. Substituting
only the previous generated timestamp comment in `server.properties`
reproduced its original hash. The production Wiki enable marker stayed absent.
The disposable processes, world, logs and build staging were removed from
the phone. The validated workstation scratch tree was sent to the Recycle Bin.

## Limits

The four requests used different prompts in fixed order. Only one paired
window revisited the movement route. The protocol client does not render or
simulate physics. The route samples creative flight across a small area. The
trial did not measure player input latency over Wi-Fi, new-terrain exploration
under comparable conditions, complex bases, autosaves or multiple players.
The sampled tick work and packet cadence cannot establish what a player would
notice in those conditions.
