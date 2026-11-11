# Packed integer projection trial, November 10, 2026

Packing pairs of Q8-by-Q2 products into integer multiplies was slower than
the existing FP16 texture projection on the Nord N20. Even without charging
the packed path for activation quantization, a 128-token projection took
about 130 ms versus about 56 ms. It was therefore not integrated into the
model or measured as a complete chat request.

## Method

For each group of 64 activations, the experiment rounds to signed Q8 using
a per-token, per-group scale and biases the values by 127. Two biased
activations `a0,a1` and their two 2-bit weight codes `c0,c1` are encoded
as `A = a0 + (a1 << 11)` and `W = c1 + (c0 << 11)`. Bits 11–21 of
the 32-bit product contain `a0*c0 + a1*c1`: the lower term is less than
2,048 and the pair sum is at most 1,524. A per-activation-group sum and
per-weight-group code sum correct for the activation and weight biases.
The approach resembles packed low-bit-width multiplication described in
[ULPPACK](https://proceedings.mlsys.org/paper_files/paper/2022/file/e09d45e14e9ece7142217550ddd3c4d0-Paper.pdf),
but uses a different radix and operand layout.

The [experimental OpenCL kernel](../wiki/experiments/n20-q2-radix-prefill.cl)
reads four packed activation bytes per token from a 2D integer texture. It
computes four output rows and either four or eight tokens per work item.
The [standalone harness](../wiki/experiments/n20-q2-radix-bench.c) prepares
the packed activations and correction sums on the CPU before timing. It
compares sampled output both to a scalar quantized reference and to a
double-precision reference using the original FP16 inputs. A separate
exhaustive integer check tested all 1,040,400 combinations of two biased
activations in 0–254 and two weight codes in 0–3; all matched the scalar
two-product result.

## Device measurements

The C.24 Adreno 619 used real Bonsai 2 4B layer-0 up-projection weights
(9,216 rows by 2,560 columns) and deterministic synthetic activations.
The isolated Android ARM64 harness ran on efficiency cores at nice 15 under
the shared GPU lock while Minecraft remained running. The table reports
medians of five GPU event spans from buffer-to-image copy through projection;
the baseline runs bracketed the candidate. The 128-token eight-token tile
was tested separately after the bracketed run. All battery readings sampled
before the bracketed runs were 24.7 °C.

| Tokens | FP16 texture before | Packed Q8, four-token tile | FP16 texture after | Packed Q8, eight-token tile |
| ---: | ---: | ---: | ---: | ---: |
| 16 | 7.149 ms | 16.403 ms | 7.252 ms | — |
| 128 | 55.525 ms | 130.785 ms | 55.802 ms | 129.409 ms |

At 128 tokens, sampled normalized RMS error against the original FP16
input was 0.380% for Q8 and 0.101% for the baseline. Across 1,824 sampled
outputs, the Q8 kernel differed from its scalar quantized reference by at
most 0.00000042 absolute. The eight-token tile also passed the sampled
reference at a seven-token tail. These checks do not establish downstream
model quality or accuracy for all activations.

The timings exclude host Q8 quantization, transfer of prepared data,
sidecar construction, model execution, retrieval, and chat delivery.
Since even this favorable projection-only measurement is over twice the
baseline, integrating this specific layout would not support the goal of
lower end-to-end latency. Other packed layouts or fused preprocessing would
need separate measurements; the experiment does not rule them out.

Build the harness with Android NDK r30, the OpenCL headers and OpenCL link
library as described for the ordinary projection harness in
[`docs/wiki-chat.md`](../docs/wiki-chat.md). Run the baseline with the FP16
texture kernel, or set `N20_RADIX=1` with the radix kernel. Add
`N20_RADIX8=1` for the eight-token tile. Use a private shader cache and real
weights extracted into scratch space; do not modify the installed model.
The live Wiki worker and configuration were not changed.
