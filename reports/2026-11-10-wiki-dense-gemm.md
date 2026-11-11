# Nord N20 dense FP16 prefill trial, November 10, 2026

A separately tiled dense GEMM did not beat the tuned packed Q2_0 projection.
The new expansion kernel reduced conversion to about 6–7 ms per matrix,
but the tested dense projection remained several times slower. No model
backend or installed service was changed.

## Method

The C.24 Adreno 619 OpenCL driver ran real layer-0 up and down tensors from
the installed Bonsai 2 4B Q2_0 model. Inputs and the sampled double-precision
checker came from the existing projection harness. The process used CPU
cores 0–2 at nice 15. The shared `/tmp/n20-q2-gpu-benchmark.lock` serialized
GPU experiments. This was not a gameplay or calibrated-power measurement.

Unlike the earlier expanded-trit experiment, this kernel used a different
matrix-multiplication dataflow. Expansion decoded four trits per work item,
applied their block scale, and wrote a K-major FP16 matrix with adjacent rows
contiguous. Each matrix required 45 MiB of scratch storage. A 64-thread
workgroup cooperatively loaded a 32-by-32 weight tile and activation tile
into local memory. Each thread computed four rows by four tokens. It
accumulated each 32-column tile in FP16 and added those partials into FP32.
Two local-memory barriers separated loading, computing, and reusing tiles.
The 16-token tests retained the 32-token tile and masked its unused outputs.

The table reports medians of the last five of seven launches. Every launch
included expansion. The packed baseline ran five launches immediately before
and after each dense test; its column gives the two median values. GPU span
runs from expansion start through GEMM end. Wall time runs from enqueue through
`clFinish`. Neither includes initial allocation, host packing, transfers,
program compilation, or verification.

| Projection | Tokens | Packed GPU ms, before/after | Expansion ms | Dense GEMM ms | GPU span ms | Wall ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Up | 128 | 61.972 / 61.573 | 6.510 | 298.544 | 305.050 | 305.469 |
| Up | 16 | 7.926 / 7.844 | 6.741 | 75.044 | 81.786 | 82.141 |
| Down | 128 | 61.565 / 61.598 | 6.136 | 273.134 | 279.270 | 279.674 |
| Down | 16 | 8.367 / 8.365 | 6.110 | 70.893 | 77.004 | 77.291 |

Sampled normalized RMS errors were 0.000731–0.000784 for the dense kernel,
versus 0.000972–0.001008 for the packed baseline. Every sampled output was
finite. These are projection checks, not a whole-model quality evaluation.

A second 32-by-32 tiled kernel used explicit four-element vectors and FP32
accumulation throughout. On the 128-token up projection its last-five median
GEMM time was 289.231 ms, GPU span 296.215 ms, and wall time 296.628 ms.
Its sampled normalized RMS error was 7.05e-7. Thus changing accumulation
precision and register representation did not close the gap. An earlier
scalar-array FP32 variant using explicit `fma` yielded no result within a
90-second limit; that run did not distinguish compiler from execution time.
The later probes printed compilation-stage markers and completed normally.

## Scope and reproduction

This rejects these two dense local-memory tiles, not all dense GEMM
implementations or an autotuned CLBlast implementation. Conversion is no
longer the dominant expense, but even zero-cost expansion would leave the
tested GEMM much slower than the packed kernel. A full-model trial was not
warranted. The larger-tile sweep was not executed after the initial timeout.

Build `wiki/experiments/n20-q2-dense-bench.c` with the same Android ARM64
toolchain and OpenCL headers/library as the ordinary projection harness.
It includes that harness for file reading, input generation, and verification.
The checked build passed `-O3 -Wall -Wextra -Werror`. Invoke via the Bionic
linker with an absolute executable path:

```sh
dense-bench n20-q2-dense-prefill.cl blk.0.ffn_up.weight.q2_0 9216 2560 128 4 4
dense-bench n20-q2-dense-prefill.cl blk.0.ffn_down.weight.q2_0 2560 9216 16 4 4
dense-bench n20-q2-dense-vector.cl blk.0.ffn_up.weight.q2_0 9216 2560 128 4 4
```

The vector kernel is fixed at four rows by four tokens per thread. The
FP16-partial kernel accepts compile-time tile sizes supplied by the harness;
only `4 4` was measured here. Scratch tensor copies, binaries, raw logs,
and shader-cache directories were removed after recording the findings.
