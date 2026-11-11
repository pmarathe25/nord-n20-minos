# Hexagon sixteen-token projection tile

An experimental v66 HVX projection kernel reduced FastRPC round-trip time
for 16–32-token batches on the Nord N20. On the tested 16-token up
projection it took 16.58 ms, versus 20.09–20.11 ms for bracketing controls;
the down projection took 16.53 ms versus 20.24–20.30 ms. This is an
isolated DSP improvement. It has not been integrated into model inference
and does not establish a faster Minecraft chat reply.

## Kernel and checks

The original kernel keeps eight token sums in HVX registers while it scans
the 64-column weight blocks. The [experimental variant](../wiki/experiments/v66-q2-hvx-16.c)
keeps sixteen sums and computes two eight-token dot-product groups for each
block. Its separate, non-inlined sixteen-token function is selected for batch
sizes 16 through 32; other sizes retain the existing eight-, four- and
one-token arithmetic. A first version that selected the sixteen-token tile
for every larger batch was slower at 128 tokens, so the bounded dispatch was
tested separately. A [second experimental source](../wiki/experiments/v66-q2-hvx-16-all.c)
retains that unbounded layout for comparison.

The benchmark used C.24 FastRPC and Hexagon v66 HVX with shared ION input
and output buffers. It loaded real Bonsai 2 4B layer-0 up and down Q2_0
weights and deterministic quantized activations. Each process timed four
calls per batch size; the table reports the median of the last three host
round trips, including FastRPC, for baseline/candidate/baseline processes.
Matrix upload, initial packing, GPU-side activation conversion and output
scatter are excluded. The shared GPU benchmark lock prevented concurrent
accelerator tests.

| Batch | Up baseline before | Up candidate | Up baseline after | Down baseline before | Down candidate | Down baseline after |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 15 | 19.769 ms | 19.399 ms | 19.790 ms | 20.309 ms | 19.383 ms | 20.265 ms |
| 16 | 20.105 ms | 16.581 ms | 20.086 ms | 20.235 ms | 16.530 ms | 20.302 ms |
| 17 | 20.563 ms | 18.116 ms | 20.555 ms | 21.499 ms | 17.705 ms | 21.524 ms |
| 31 | 30.304 ms | 23.778 ms | 30.289 ms | 44.512 ms | 35.540 ms | 44.214 ms |
| 32 | 30.778 ms | 27.021 ms | 30.769 ms | 45.831 ms | 35.694 ms | 45.775 ms |
| 33 | 31.495 ms | 29.918 ms | 31.474 ms | 49.250 ms | 47.015 ms | 49.100 ms |
| 128 | 107.819 ms | 100.225 ms | 107.983 ms | 331.665 ms | 321.507 ms | 331.459 ms |

The candidate also ran at 1, 4, 63 and 127 tokens. For every tested size
and shape, the complete output-buffer FNV-1a hashes matched all controls;
the benchmark additionally checked sampled integer outputs against its
scalar reference. Hash equality is not a mathematical proof for all
inputs. The fixed-point activation and block-scale approximation was
already present in the original DSP kernel.

The 33- and 128-token paths do not invoke the new sixteen-token routine,
yet their timings changed. Compiler output and code layout changed along
with the dispatch; the cause was not isolated. The compiler emitted vector
spills for the sixteen-token function, so neither register pressure nor
cache behavior should be inferred from timing alone. With the unbounded
sixteen-token layout, a separate 128-token comparison took 110.10 ms up
and 337.98 ms down, versus 107.79–107.98 ms and 331.46–331.76 ms for its
bracketing controls. That layout was not selected for long batches.

## Scope and reproduction

Build either HVX source in place of `v66-q2-hvx.c` using the v66 Zig/LLD
procedure in the [projection report](2026-11-09-hexagon-projection.md).
The updated [host benchmark](../wiki/experiments/v66-q2-bench.c) accepts
`N20_SHARED=1` for ION, `N20_OUTPUT_HASH=1` for complete output hashes,
and `N20_EXTRA_BATCHES=1` for boundary-size checks. It built with Android
NDK r30 at `-O3 -Wall -Wextra -Werror`.

The DSP was activated in a private mount namespace using the existing C.24
firmware and unsigned shell; no firmware, partition metadata, installed
library or service configuration was replaced. The idle-checked Minecraft
server was saved and stopped for each DSP maintenance window and resumed
afterward. The guard's final run sampled at least 5,043,704 KiB available
memory and at most 26.0 °C; Minecraft passed a status query and CDSP returned
to `OFFLINE` with zero crashes. The live Wiki worker remained disabled.

These results leave an integration question: the GPU and DSP share memory
bandwidth, and the earlier full-gate offload slowed whole-model prefill.
A concurrent row-shard trial must include activation packing, synchronization,
output conversion and full-request timing before this kernel can be judged
as a chat optimization.
