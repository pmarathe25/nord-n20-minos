# Nord N20 CLBlast HGEMM prefill trial, November 10, 2026

Actual upstream CLBlast HGEMM did not accelerate the full-weight Bonsai 2 4B
projection workload. Its default path took about twice the time of the
explicit 2D activation-texture baseline, including expansion and output
conversion. An Adreno 640 tuning preset was slower still. This rejects the
two measured configurations, not every possible CLBlast tuning. No complete
model experiment or deployment followed this failed projection gate.

## What was new

The earlier [dense GEMM trial](2026-11-10-wiki-dense-gemm.md) tested custom
local-memory kernels and explicitly left CLBlast untested. This experiment
built the actual CLBlast library at commit
`3c9a2641e435bd864e6871acb1a0f66fc030b9d4` with Android NDK r30 for ARM64/API 30.
[CLBlast's GEMM documentation](https://github.com/CNugteren/CLBlast/blob/3c9a2641e435bd864e6871acb1a0f66fc030b9d4/doc/details_gemm.md)
describes its direct and indirect paths, including possible preprocessing
and postprocessing costs. Those costs are included here. The upstream
[tuning database](https://github.com/CNugteren/CLBlast/blob/3c9a2641e435bd864e6871acb1a0f66fc030b9d4/src/database/kernels/xgemm/xgemm_16.hpp)
has Qualcomm fallback and Adreno 640 parameters, but no 619-specific entry.

The [adreno-llms project](https://github.com/a8nova/adreno-llms) was a lead for
examining a mature GEMM implementation. Its published Bonsai Q1_0 models and
Adreno 620 measurements differ from this phone's Bonsai 2 Q2_0 checkpoint
and Adreno 619. Those claims were not used as performance evidence here.

## Method

Real layer-0 up (9,216 by 2,560) and down (2,560 by 9,216) Q2_0 tensors
were extracted from the installed 4B GGUF. The existing deterministic FP16
activation generator and sampled double-precision checker were reused.
The baseline was `n20-q2-activation-image-prefill.cl`, four rows by eight
tokens per work item, 128-thread workgroups, 64-column FP16 partial sums
and FP32 outer accumulation. Each timed baseline launch included its
buffer-to-2D-image copy. It was not the older 1D-buffer-image baseline.

Each candidate launch expanded the original codes and scales into a
K-major FP16 matrix (45 MiB), called `CLBlastHgemm` in column-major NT mode,
and converted its FP16 output to the backend's FP32 format. The input
layout already provided the transposed B matrix. The GPU span starts at
expansion and ends at the output cast; it includes intervening CLBlast
kernels and device gaps. It excludes initial upload, allocation, compilation,
host weight packing and verification. Each process ran seven launches;
reported candidate medians use the last five. Each baseline is the median
of five launches. Warm wall times tracked candidate spans within about
1.5 ms. The initial candidate wall times included about 3.3–3.6 seconds
of CLBlast setup/compilation and are excluded.

All GPU trials held `/tmp/n20-q2-gpu-benchmark.lock`, used CPU cores 0–2 at
nice 15, and set a private shader-cache path. Runs were serialized, but not
thermally controlled or randomized. No services were stopped or replaced.

## Results

Each row brackets the candidate with explicit texture baselines. Times
are milliseconds. The default CLBlast configuration and forced-indirect
Adreno 640 preset were separate bracketed sequences.

| Projection | Tokens | Texture before/after | Default CLBlast span | 640-preset texture before/after | 640-preset span |
| --- | ---: | ---: | ---: | ---: | ---: |
| Up | 128 | 55.879 / 55.593 | 116.854 | 55.582 / 55.548 | 165.419 |
| Up | 256 | 112.235 / 112.223 | 227.282 | 112.734 / 110.985 | 327.097 |
| Down | 128 | 56.780 / 56.435 | 109.334 | 56.800 / 56.850 | 138.142 |
| Down | 256 | 111.795 / 113.445 | 214.937 | 112.069 / 112.065 | 268.218 |

Expansion took about 6.3–7.1 ms per launch; casting took 0.15–1.13 ms.
Even removing both costs would leave the tested HGEMM paths slower.
The scratch harness initially labeled the cast event `gemm_ms`; that event
is **only the cast**, not HGEMM. The retained harness names it `cast_ms`.
Only the complete expansion-to-cast span supports the comparison above.

All sampled results were finite. Both CLBlast configurations produced the
same sampled error statistics, with more rounding error than the texture
kernel:

| Projection | Tokens | Checked outputs | Texture NRMSE | CLBlast NRMSE | CLBlast maximum absolute error |
| --- | ---: | ---: | ---: | ---: | ---: |
| Up | 128 | 1,824 | 0.001006845 | 0.006065100 | 0.02192950 |
| Up | 256 | 3,552 | 0.001018906 | 0.006080147 | 0.01956962 |
| Down | 128 | 513 | 0.001007799 | 0.017415020 | 0.34926800 |
| Down | 256 | 999 | 0.001139718 | 0.013068790 | 0.16251700 |

These synthetic-activation projection checks do not establish model quality.
No answer-equivalence, first-token logit, or grounded-answer claim is made
for CLBlast, and no full-model speedup is inferred from projection timings.

## Reproduction and scope

The retained [host harness](../wiki/experiments/n20-q2-clblast-bench.c) includes
the existing projection checker. Build it with the CLBlast C headers and
Android shared library plus the existing OpenCL headers/link library, at
`-O3 -Wall -Wextra -Werror`. The retained source passed that build. Run it
through the existing Bionic linker with
[the expansion/cast kernel](../wiki/experiments/n20-q2-clblast-expand.cl),
raw tensor, rows, columns, tokens, and final compatibility arguments `4 4`.
Leave `N20_BLAST_TUNE` unset for the library default. Setting it selects the
Adreno 640 preset and forces the indirect path. The source records every
overridden parameter; no on-device autotuning was performed.

For the texture baseline, apply the existing
`n20-q2-activation-image-bench.patch` to a scratch copy of the projection
harness and set `N20_IMAGE2D=1`, using the activation-image kernel with
`-DN20_PARTIAL=64` and final arguments `4 8`.

The library emitted a missing optional `libicu.so` diagnostic but completed
all GEMMs and numerical checks. Scratch tensors, binaries, CLBlast checkout,
build files and shader caches were removed after recording the findings.
Only the small experiment harness/kernel and this report were retained;
no third-party source or binary was added to the repository.