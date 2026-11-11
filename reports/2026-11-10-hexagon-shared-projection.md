# Nord N20 shared Hexagon projection, November 10, 2026

ION-backed FastRPC activation and output buffers removed about 19 ms from
the 128-token Hexagon projection round trip. The unchanged DSP kernel still
took approximately 108–110 ms. This leaves little useful headroom for the
previous proposal to run one entire gate projection on Hexagon concurrently
with its up projection on Adreno, now that the texture GPU path is faster.

## Matched transport experiment

The existing v66 projection kernel processed the installed Bonsai 2 4B
layer-0 up matrix, 9,216 by 2,560, with its existing deterministic activations.
The host alternated ordinary allocations and `rpcmem_alloc(25, 0, size)`
ION buffers twice in one maintenance window. Each process uploaded the same
packed weights before timing. Four calls were measured at each batch size;
the table gives the median of the last three calls, separately for each
alternating pair. These are host wall times around the FastRPC invocation.

| Tokens | Ordinary, first / second run ms | ION, first / second run ms |
| --- | ---: | ---: |
| 1 | 5.111 / 4.821 | 4.840 / 4.556 |
| 4 | 7.727 / 7.344 | 6.810 / 6.514 |
| 16 | 24.195 / 23.806 | 20.557 / 20.054 |
| 128 | 127.816 / 127.970 | 108.945 / 108.202 |

The warm ION calls at 128 tokens spent only 0.28–0.33 ms outside the DSP
kernel. Both allocation modes used direct DSP output without its alignment
scratch copy. Every process checked 18,688 projection values against the
scalar integer reference, with no mismatches. The 128-token sampled
floating-point normalized RMS error was 0.003959 in all four runs; this is
the existing activation/fixed-scale approximation, not loss introduced by
ION. The single-token top-1 check also passed.

This experiment measures the RPC transport floor. It does not import these
buffers into OpenCL, measure GPU/DSP concurrency, or include GPU activation
quantization and output unpacking. The earlier
[shared Hadamard experiment](2026-11-09-wiki-accelerators.md) established
OpenCL import and cross-device visibility separately. No new whole-model
hybrid or chat-quality measurement was performed here.

## Implication for the previous graph split

The [texture experiment](2026-11-10-wiki-activation-texture.md) measured
55.466 ms for the same-shaped 128-token GPU up projection, including its
buffer-to-image copy. Twice that cost is about 110.9 ms. Against that
approximate gate-plus-up comparison, a 108–109 ms DSP gate alone consumes
almost all available time even with perfect overlap. Pack, unpack, graph
synchronization and shared-memory contention would consume the remaining
few milliseconds. This is a cross-experiment estimate, not a concurrently
measured pair or a rigorous bound across all inputs and clocks.

The old full-gate hook also duplicated approximately 177 MB of weights
across 24 layers and was already slower in its
[full-model trial](2026-11-09-hexagon-projection.md). Removing transport
copies therefore does not provide evidence for reinstating that hook
against the newer texture baseline. The estimate applies specifically to
offloading one entire gate while the GPU computes one entire up projection.
It does not rule out a balanced partial-row split, other independent graph
work, or a faster DSP kernel. Those require their own whole-request and
numerical validation.

## Reproduction and device state

The opt-in `N20_SHARED=1` in
[v66-q2-bench.c](../wiki/experiments/v66-q2-bench.c) selects shared buffers.
Unset it for the ordinary allocation control. Build and stage as described
in the [projection report](2026-11-09-hexagon-projection.md). The host built
with Android NDK r30 at `-O3 -Wall -Wextra -Werror`; the DSP used the same
v66 Zig/LLD procedure as before. The numerical data path was unchanged.

The benchmark held `/tmp/n20-q2-gpu-benchmark.lock` before invoking the
installed idle-checked server stop helper. Its `finally` block restored
the original stop-marker state and verified that Minecraft answered a
status request. FastRPC device nodes and the unsigned shell mount existed
only in a private mount namespace. The shell was read from the existing
DSP filesystem, and the matching vendor client dependencies were extracted
from the existing local C.24 images into scratch storage. No firmware,
partition metadata, installed library or service configuration was changed.
CDSP returned to `OFFLINE` with crash count zero, the temporary stop marker
was absent, and the phone remained up. Scratch images, extracted vendor
files, tensors, binaries and raw logs were removed from the phone after
recording results; the Windows scratch directory went to the Recycle Bin.
The shared benchmark lock was retained.
