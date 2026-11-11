# Fused feed-forward projections on the activation texture

Two new gate/up/SwiGLU layouts did not yield a compelling GPU improvement
on the Nord N20. At 128 tokens, the best fused two-row kernel took about
145–147 ms, while two existing texture projections and SwiGLU took about
112 ms. Combining both weight matrices into one ordinary projection was
near the baseline at 128 tokens, and slower at 256 tokens. These are
projection-level measurements, not complete reply times.

## What was tested

The earlier [fused feed-forward trial](2026-11-09-wiki-graph.md) used a
linear image-buffer input and was slower end to end. This trial uses the
newer real 2D FP16 activation texture and real layer-0 Bonsai 2 4B gate
and up matrices, each 9,216 by 2,560. The
[experimental fused kernel](../wiki/experiments/n20-q2-ffn-texture.cl)
loads an activation once for gate and up, keeps both intermediate sums
private, and writes SwiGLU directly. It tests one, two or four rows per
work item and four- or eight-token vectors.

A second candidate stores the two matrices concatenated in the existing
transposed Q2_0 format. One unmodified texture-projection dispatch produces
both outputs, and a separate 2D SwiGLU kernel combines them. Its packing
is done before timing. A deployed implementation would need to integrate
packing, graph matching, tensor lifetimes, and fallback behavior; this
experiment does not implement those pieces.

The [harness](../wiki/experiments/n20-q2-ffn-texture-bench.c) uses one
buffer-to-image copy for all paths. Its baseline is two ordinary texture
projections plus a SwiGLU kernel, sharing that one copy. Thus the measured
baseline already receives the benefit of avoiding a second activation copy;
the test does not claim that the current full-model graph does so. Event
timings cover the copy through the final output kernel, excluding packing,
allocation, host submission and full-model graph execution. The C.24
Adreno driver ran under the shared GPU lock on CPUs 0–2 at nice 15 while
Minecraft remained running. Temperature readings before repeated runs were
24.7 °C.

## Measurements

Each run used five GPU events per mode, ordered baseline, fused, merged,
baseline. The table gives the medians from three separate processes per
token count, showing the range across those three medians. The fused column
uses two rows and eight tokens per work item.

| Tokens | Baseline before | Fused | Merged projection | Baseline after |
| ---: | ---: | ---: | ---: | ---: |
| 16 | 14.420–14.428 ms | 18.251–18.481 ms | 14.193–14.241 ms | 14.424–14.438 ms |
| 128 | 111.926–112.914 ms | 145.117–146.794 ms | 112.591–112.876 ms | 111.933–112.909 ms |
| 256 | 223.815–224.890 ms | 290.496–296.797 ms | 225.690–230.363 ms | 223.768–224.904 ms |

The small merged-path gain at 16 tokens is not a whole-model or chat gain;
the 128-token result varied around the baseline and the 256-token result
was slower. A preliminary screen at 16 and 128 tokens also tried one and
four rows with eight-token vectors and two and four rows with four-token
vectors. None beat the two-row/eight-token fused kernel in those trials.
An additional [explicit two-row implementation](../wiki/experiments/n20-q2-ffn-texture-pair.cl)
avoided indexed private arrays. It took 167.8 ms at 128 tokens, against
112.0 ms for each bracketing baseline, and had zero full-buffer numerical
difference. This single run does not measure register allocation or spills.

Every candidate's complete output buffer had zero numerical difference from
the separate-projection baseline for these fixtures. Against a sampled
double-precision calculation on the original FP16 inputs, normalized RMS
error was 0.001434, 0.001471 and 0.001314 at 16, 128 and 256 tokens.
The two-row/eight-token candidate and merged path also had zero full-buffer
numerical difference on a 17-token tail. These checks do not prove
equivalence on other inputs or in a full model.

Build the harness with Android NDK r30 and the OpenCL headers/library as
described in [`docs/wiki-chat.md`](../docs/wiki-chat.md). Supply the texture
baseline source, extracted gate weights, row/column/token counts, and set
`N20_FFN_SOURCE` and `N20_FFN_UP_WEIGHTS`. Optional `N20_FFN_ROWS` and
`N20_FFN_TOKENS` select the fused tile. The benchmark compiled with
`-O3 -Wall -Wextra -Werror`. No live model, firmware, server configuration
or worker was changed. The experiment does not rule out other fusion or
partitioning designs that change memory traffic or arithmetic.
