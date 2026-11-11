# Nord N20 Flash Attention shader cache, November 10, 2026

Caching the five lazy Flash Attention programs saved about 11 seconds at
fresh worker startup on the Nord N20 once the cache was populated. The
texture profile returned its complete cited answer in 57.40 seconds from
process launch, compared with 67.87 seconds using the eager cache alone.

## Method

The phone ran Bonsai 2 4B Q2_0 on the Adreno 619 with the C.24 OpenCL
driver and Android ARM64 llama.cpp b12110. Each worker used one 1,536-token
slot, a 256-token microbatch and two host threads on CPUs 0–2 at nice 15.
All workers ran under the shared GPU benchmark lock. Scratch copies of the
shader caches and offline Wiki cache kept the test separate from the
installed Wiki feature, whose enable marker was absent. The live Minecraft
status endpoint reported zero connected players during the test.

The harness used the existing offline retriever, grounded chat prompt and
reply parser. It sent the Observer power-level question directly to a
private worker socket after the health endpoint responded. Each request
evaluated 352 prompt tokens and generated 34 tokens. The reported reply
interval includes retrieval, model generation and parsing. It ends at
the complete cited reply. Disk prefix restore was disabled for these arms.

The patch passes the lazy build sites through the existing binary-cache
lookup, whose key includes the source, compiler options and OpenCL driver
identity. A miss or invalid entry uses the same source-build function with
the original nonfatal flag, diagnostic tag and retry queue. Five lazy
programs for 256-wide Flash Attention added 608,356 bytes to the shader
cache. [Backend patch](../wiki/experiments/llama-b12110-n20-fa-cache.patch)

## Complete replies

| Backend and starting cache | Launch to health | Health to reply | Launch to reply | Time inside `clBuildProgram` |
| --- | ---: | ---: | ---: | ---: |
| Stock, eager programs cached | 22.235 s | 71.890 s | 94.125 s | 11.046 s |
| Stock with patch, eager programs cached | 22.096 s | 72.057 s | 94.153 s | 10.877 s |
| Stock with patch, all programs cached | 11.283 s | 72.236 s | 83.519 s | 0.029 s |
| Stock with patch, two unusable lazy entries | 13.732 s | 71.382 s | 85.114 s | 2.478 s |
| Tuned texture, eager programs cached | 18.415 s | 49.454 s | 67.869 s | 10.901 s |
| Tuned texture with patch, all programs cached | 7.470 s | 49.927 s | 57.398 s | 0.030 s |

For the failure trial, one lazy cache file was truncated and a second was
renamed under an unrelated cache key to simulate an obsolete entry. The
worker rebuilt both expected entries from source. The cache log recorded
270 hits and two saves. The warm patched stock worker recorded 272 hits
with zero misses or saves.

The two texture workers used the launcher's tuned kernel settings and
confirmed texture dispatch in their logs. A model-free 4 KiB backend
allocation seeded the eight texture-profile additions in a copy of the
stock eager cache. That seeding step took 1.970 seconds with 435,352 KiB
peak process RSS. The patched texture arm also received the five compatible
Flash Attention binaries from the earlier scratch run and recorded 279
hits with zero misses or saves. Process mappings verified the selected
scratch texture libraries and the installed C.24 driver in both arms.

Every arm returned `15` with the Observer Behavior citation and the same
raw generated response. Within each profile, the FNV-1a digest across
all 248,320 first-output logits matched. Texture prefill and decode took
35.834 and 13.558 seconds in the control and 36.209 and 13.666 seconds
with the cache patch. The timing comparison covers one question per arm
and an idle Minecraft server.

Both stock and texture backends built with the Android NDK. The patch
passed `git apply --check --whitespace=error` against b12110 both alone
and after the N20 OpenCL, activation texture, server and disk-prefix
patches. The model worker, caches and logs used scratch paths and were
removed after the trial.
