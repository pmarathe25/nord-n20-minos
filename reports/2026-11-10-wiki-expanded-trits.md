# Nord N20 expanded ternary prefill trial, November 10, 2026

Expanding packed Q2_0 codes into FP16 trits did not accelerate the existing
prefill projection. The projection alone was 1.56–2.07 times slower, before
paying for conversion. No full-model experiment or deployment followed.

The phone used its C.24 Adreno 619 OpenCL driver and real Bonsai 2 4B layer-0
feed-forward weights. The probe ran at nice 15 on CPU cores 0–2, with its own
scratch directory and shader cache. A shared `flock` serialized GPU experiments.
This was not a Minecraft gameplay or thermal-controlled experiment.

The baseline was `wiki/experiments/n20-q2-prefill.cl` with
`N20_PARTIAL=64`. Its existing standalone projection harness supplied the same
deterministic FP16 activations to both paths. The alternative expanded one
matrix to a 45 MiB FP16 buffer, leaving group scales separate. The existing
inner loop then loaded four expanded weights instead of shifting packed codes
and converting integers. Accumulation order, scaling, input layout, and launch
geometry stayed the same. The expansion buffer was reused on each of five runs.

| Shape | Tokens | Packed projection median | Expanded projection median | Conversion plus projection median |
| --- | ---: | ---: | ---: | ---: |
| Up, 9,216 × 2,560 | 16 | 7.836 ms | 16.215 ms | 1,036.205 ms |
| Down, 2,560 × 9,216 | 16 | 8.370 ms | 13.090 ms | 1,033.007 ms |
| Up, 9,216 × 2,560 | 128 | 62.832 ms | 126.474 ms | 1,203.054 ms |
| Down, 2,560 × 9,216 | 128 | 62.002 ms | 104.619 ms | 1,125.246 ms |

Projection columns use OpenCL event execution times. The last column spans
the expansion event's start through the projection event's end, including
their device-side gap. These figures exclude compilation, uploads, allocation,
CPU verification, and model execution. Each baseline ran immediately before
its alternative; order was not randomized. The scalar expansion used one work
item per weight and integer division/remainder. Its approximately one-second
cost is not evidence of the best achievable conversion speed. Even a free
conversion would leave this expanded projection slower.

The final output buffer's FNV-1a 64-bit hash matched between each pair:

| Shape and tokens | Matching full-output hash |
| --- | --- |
| Up 16 | `5df88dd81c87db5f` |
| Down 16 | `7294f1e941a394ba` |
| Up 128 | `959f94cc3d04f9e1` |
| Down 128 | `259a995f55f8ed3b` |

The existing sampled double-precision checker also produced identical errors
for each pair: normalized RMS error ranged from 0.0009715 to 0.0010078.
Matching hashes are evidence of matching checked buffers, not an exhaustive
proof for other inputs or a full-model quality evaluation.

This rejects expanded trits with the existing projection dataflow. It does
not evaluate a separately tiled dense FP16 GEMM, different memory layouts,
or expansion fused into a tiled local-memory load. The temporary harness,
raw tensor copies, binaries, logs, and shader cache were removed after recording
these findings; the repository baseline kernel and harness remain unchanged.
