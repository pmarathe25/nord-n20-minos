# Nord N20 activation texture gameplay trial, November 10, 2026

The hardened activation texture profile returned the two correct cited
Observer answers 1.51 and 1.23 seconds sooner than tuned image buffers,
about 4% in both cases. All four query windows delivered approximately
20 tick samples per second. Both profiles had slower p99 ticks during
inference than during their paired no-query windows. The texture's
movement p99 rose by 4.79 ms, compared with 5.95 ms for tuned. The
small number of ticks over 50 ms varied by window.

## Reply and tick measurements

| Profile and workload | Chat to reply | Samples, baseline / query | P50, baseline / query | P95, baseline / query | P99, baseline / query | Max, baseline / query | Over 50 ms, baseline / query | Packet gaps over 100 ms, baseline / query |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Tuned, stationary | 38.55 s | 771 / 770 | 24.09 / 23.96 ms | 27.81 / 30.05 ms | 30.49 / 32.68 ms | 44.26 / 50.68 ms | 0 / 1 | 0 / 0 |
| Texture, stationary | 37.04 s | 741 / 740 | 22.84 / 22.81 ms | 26.77 / 28.64 ms | 28.53 / 31.03 ms | 47.16 / 63.74 ms | 0 / 1 | 0 / 0 |
| Tuned, movement | 32.16 s | 642 / 643 | 22.88 / 22.90 ms | 31.57 / 31.00 ms | 41.13 / 47.08 ms | 70.87 / 72.15 ms | 4 / 6 | 4 / 3 |
| Texture, movement | 30.93 s | 619 / 618 | 22.38 / 21.67 ms | 29.15 / 29.89 ms | 37.30 / 42.09 ms | 76.19 / 74.05 ms | 5 / 4 | 1 / 2 |

The stationary prompt asked the strength of an observer's redstone signal
and returned `15`. The movement prompt asked its duration in game ticks
and returned `2`. All replies linked to Observer Behavior. First-token
logit hashes matched across profiles for each prompt. The stationary
query evaluated 302 tokens and generated 15. The movement query
evaluated 266 tokens and generated 11.

| Profile | Launcher to health | Startup tick p95 / p99 | Startup ticks over 50 ms | Stationary prefill / decode | Movement prefill / decode |
| --- | ---: | ---: | ---: | ---: | ---: |
| Tuned | 23.60 s | 29.53 / 31.71 ms | 0 of 473 | 32.58 / 5.81 s | 27.88 / 4.15 s |
| Texture | 23.89 s | 28.16 / 30.45 ms | 1 of 478 | 31.18 / 5.74 s | 26.72 / 4.11 s |

## Method

The phone ran vanilla Minecraft Java 26.3 on Java 25 with a 512 MiB
initial heap, a 2 GiB maximum heap, view distance 24 and simulation
distance 10. It hosted a disposable seeded world on a verified loopback
listener. The workstation's protocol client joined through USB SSH and
requested view distance 24. The client had 2,181 chunks loaded at the
origin after warming the route. Autosaves were disabled for timing. The
movement workload used creative flight on a 32-block square at about
4.3 blocks per second. The client flew two practice laps and reset to the
same position before each window.

The offline Wiki bridge used a private copy of the same cache and followed
the game server's chat log. The two questions were checked against the
retriever and exact-answer route. The client timed real player chat through
the private system-chat response written via the console FIFO. Each
profile began with an empty disk prefix. Its stationary query seeded
the system prefix, and the movement query used the same resident worker.
Each workload had a 60-second no-query window with the worker loaded.
The table uses the first portion of that window with the same duration as
the request. Native debug packets gave active tick work as `FULL_TICK -
IDLE`. Gaps over 100 ms between packets served as a transport cross-check.

The Android ARM64 llama.cpp b12110 build used the same OpenCL, activation
texture, server, and disk-prefix patches in both arms. Both used the
installed Bonsai 2 4B group-64 Q2_0 model and C.24 OpenCL driver. The
tuned arm used 64-column prefill, custom decode, wide vocabulary tile,
direct packing and recurrent copy, and width-512 Hadamard. The texture
arm added the 64 × 9,216 RGBA FP16 activation image and 256-token
microbatch. The launcher ran the worker at nice 15 with two threads and
kept its shader and prefix caches private. The texture worker mapped
the scratch OpenCL backend and C.24 driver. The driver identified
Qualcomm Adreno, sysfs identified Adreno619v2, and the log confirmed
texture allocation and dispatch.

## Memory and production lifecycle

| Profile | Minimum available, startup | Minimum available, measured gameplay | Peak model RSS | Peak game RSS |
| --- | ---: | ---: | ---: | ---: |
| Tuned | 1,570.1 MiB | 1,527.9 MiB | 2,083.3 MiB | 1,387.6 MiB |
| Texture | 1,610.4 MiB | 1,591.4 MiB | 2,040.8 MiB | 1,360.8 MiB |

The 40 °C battery guard remained active and the highest observed
battery temperature was 28.2 °C. The first attempt stopped at
1,194.7 MiB available during tuned startup after the disposable Java
process reached 1,822.3 MiB RSS generating terrain. Its memory guard
was 1,200 MiB. Subsequent attempts reused the saved disposable terrain
and used the prior gameplay trial's 1,000 MiB guard. None of those
attempts triggered it.

The tuned session ended at texture startup because the default
verbosity suppressed the allocation message. The first trace session
ended before requests because the assertion expected the model number
in the driver's generic Qualcomm Adreno name. The final texture
session used the observed driver label and the KGSL model identifier.
These two checks required separate production stops.

The installed admin helper checked for zero players twice before every
stop and sent Minecraft its normal stop command. The four interruptions
lasted 245.43, 404.09, 183.03 and 375.69 seconds from stop request to
production readiness. Every restart returned zero players with the
whitelist enabled and enforced. The whitelist digest, non-comment
server settings, world directory identity and level marker verified
before and after each run. The installed Wiki enable marker was absent.
The disposable world, launcher, model staging and raw logs were
removed from the phone after recording the results. The validated
workstation scratch tree was sent to the Recycle Bin.

## Limits

The profile order was fixed and the texture measurements came from a
separate disposable JVM session after the tuned measurements. Both used
the saved world and the same warmed route. The texture arm enabled
backend trace logging to prove dispatch. Different session history,
logging, and shader cache history can affect small timing differences.
The protocol client does not render gameplay or measure input latency.
The windows cover one connected client, no autosaves, and warmed terrain.
They do not establish that players will experience no noticeable lag in
ordinary play.
