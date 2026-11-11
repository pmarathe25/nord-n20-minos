# Nord N20 tuned Wiki launcher trial, November 10, 2026

The phone ran Android ARM64 llama.cpp b12110 built with the OpenCL, server,
and disk prefix patches. Both modes used the installed Bonsai 2 4B group-64
Q2_0 checkpoint, C.24 Adreno 619 driver, one 1,536-token slot, and two
efficiency-core threads at nice 15. The tuned launcher profile enabled the
64-column FP16 partial sums with FP32 outer accumulation, four-lane tail,
custom decode including the wide vocabulary tile, direct weight packing and
recurrent-state copy, and radix-32 width-512 Hadamard transform. With the disk
prefix enabled it also enabled single-turn batching. Both modes used disk
prefixes, with a separate cache for each profile.

The scratch copy of `n20-wiki-start` used private paths for the bundle,
markers, locks, shaders, snapshot, socket, log and FIFO. The real bridge
retrieved from a private copy of the offline Wiki cache and replied through
the FIFO under the Minecraft account. The test used a five-second idle
threshold and logged first-token logit hashes. It checked memory and battery
temperature while waiting, with limits of 1,200 MiB available and 40 °C.
The shared GPU lock serialized the run. The live Minecraft status endpoint
responded with zero players and the installed Wiki feature stayed disabled.

## First complete reply

The redstone output strength question used model inference in both modes.
Each evaluated 352 prompt tokens and generated 34 tokens, returning the same
private reply, `15`, with the Observer Behavior section link. Each timing is
one run. Reply time starts when the private chat log receives the question
after the worker reports healthy and ends when the bridge writes `tellraw`.
Launch to health includes the prefix identity check.

| Empty prefix | Launch to health | Reply | Launch to reply | Prefill | Decode |
| --- | ---: | ---: | ---: | ---: | ---: |
| Stock, partially populated shader cache | 82.94 s | 72.28 s | 155.22 s | 55.44 s | 16.74 s |
| Tuned, stock shaders already cached | 22.34 s | 51.24 s | 73.59 s | 37.42 s | 13.73 s |

The tuned profile reduced time from chat to complete reply by 21.04 s
(29.1%) for this prompt. The first-token logit hashes differed. An interrupted
setup start partially populated the shader cache. The timed stock launch
finished populating it, and the tuned launch reused those compiled shaders.
Treat seed startup and launch-to-reply times as observations of that sequence.

## Restart and idle wake

Both restarted processes mapped the scratch `libggml-opencl.so` and C.24
`libOpenCL.so`. Their sanitized environments used separate prefix directories
and fingerprint keys. The tuned environment contained the selected 64-column
source path. Each model restored 106 tokens, evaluated the remaining 246 and
generated 34. The first-token hash in each mode matched its seed, and the
bridge returned the same answer and citation across all four runs.

| Interval from server logs | Stock | Tuned |
| --- | ---: | ---: |
| Model process start to health | 24.79 s | 18.18 s |
| Restarted prompt evaluation | 39.48 s | 26.13 s |
| Restarted decode | 16.79 s | 13.70 s |
| Idle wake to complete generation | 65.24 s | 44.24 s |

Both idle requests asked how many game ticks the observer pulse lasts. Each
mode restored 106 tokens, evaluated 312 and generated 11. The bridge answered
`2` and linked the Observer Behavior section. Idle-wake times in the table
include reload, which took 10.90 s with stock kernels and 7.09 s with tuned
kernels. The tuned worker completed the restarted model request in 39.82 s
versus 56.27 s for stock, a 29.2% reduction. Idle-wake generation took 32.2%
less time in the tuned run.

In the stock restart, the launcher reached health in 29.08 s, the bridge
answered in 56.44 s and the total from launch through answer was 85.58 s.
The stock idle reply took 65.61 s through the bridge. The tuned harness
stopped on a reply assertion that expected the units in the cited numeric
answer. That lost its full launcher and bridge timing for this arm. The
table compares intervals retained in both server logs.

Both prefix files were 56,168,292 bytes with mode 0600, owned by the model
UID and GID. Each file was stored in its own mode-0700 directory.

## Fallback and verification

With the tuned marker present, a missing prefill source triggered the
launcher's validation fallback. The stock worker reached health in 25.17 s.
A nonempty malformed source passed the file checks and triggered a compiler
error in the tuned worker. The launcher logged one retry and the stock
worker reached health in 30.77 s from the original launch. In both cases,
the running process mapped the scratch OpenCL libraries, carried the stock
prefix directory and a valid fingerprint key, and received no tuned options.
The caller's deliberately supplied prefill override was absent from the
worker environment.

The launcher passed `sh -n`, the Python modules compiled, and the four
prefix and seven chat tests passed on ARM64 Debian. These are single runs of
two grounded prompts with the stock and tuned profiles. The trial measured
reply and worker latency while the live server was idle and did not measure
player tick latency. Scratch processes and files were removed afterward.
