# Nord N20 activation texture layout trial, November 10, 2026

Two new activation texture layouts failed to improve the existing 2D
texture prefill path. Both preserved the complete projection output hashes.
They were rejected before full-model testing; no backend or service changed.

The C.24 Adreno 619 driver ran real layer-0 Bonsai 2 4B Q2_0 up
(9,216 by 2,560) and down (2,560 by 9,216) projections with 128
deterministic FP16 input tokens. The baseline was the existing
four-row/eight-token texture kernel, with 64-column FP16 partial sums and
FP32 outer accumulation. Its RGBA pixels contain four consecutive tokens,
with image width `padded_tokens / 4` and height `columns`.

The first candidate swapped the image axes, placing consecutive reduction
columns along x, with width `columns` and height `padded_tokens / 4`.
The second placed four consecutive reduction columns in each RGBA pixel,
with width `columns / 4` and height `padded_tokens`. It fetched eight
token pixels before each four-column calculation and explicitly swizzled
their named lanes into the existing token vectors. Neither changes the
accumulation order, weights, scales, or four-by-eight output tile.
The driver rejected dynamic subscripts on `half4`; named lanes compiled.

The motivation was to test locality along the reduction axis and whether
grouping reduction loads altered texture execution cost. No hardware cache
or register counters were collected, so these results do not identify a
microarchitectural cause. The buffer-to-image copy used each layout's
tightly packed source and pixel dimensions, following the
[Khronos copy API](https://registry.khronos.org/OpenCL/specs/unified/refpages/man/html/clEnqueueCopyBufferToImage.html).

Each row below is a baseline/candidate/candidate/baseline sequence. Each
number is the median of five launches in one process, in milliseconds.
The measured span begins at the per-launch GPU copy and ends when GEMM
finishes. Host layout conversion, allocation, upload, compilation and
verification are excluded for all modes. Candidate host conversion would
need replacement by a timed GPU transform in a complete model; the
absence of a projection improvement made that integration unwarranted.

| Layout | Projection | Baseline before | Candidate 1 | Candidate 2 | Baseline after |
| --- | --- | ---: | ---: | ---: | ---: |
| Swapped axes | Up | 55.744 | 55.876 | 55.797 | 55.649 |
| Swapped axes | Down | 56.929 | 56.816 | 56.163 | 56.063 |
| Reduction columns in RGBA | Up | 55.473 | 57.818 | 58.705 | 55.899 |
| Reduction columns in RGBA | Down | 56.743 | 58.864 | 58.863 | 56.102 |

The changing down baseline prevents interpreting its early axis-swapped
comparison as a speedup. The reduction-pixel version was consistently slower.
Runs held `/tmp/n20-q2-gpu-benchmark.lock`, used cores 0–2 at nice 15,
and set a private XDG cache directory. Thermal state was not controlled.
These are projection timings, not whole-model or electrical measurements.

Every baseline and candidate produced FNV-1a hash `959f94cc3d04f9e1` for
up and `259a995f55f8ed3b` for down. Sampled double-precision verification
checked 1,824 and 513 outputs respectively. All were finite; normalized
RMS error was 0.001006845 for up and 0.001007799 for down, and maximum
absolute error was 0.001976439 and 0.007879520. Matching hashes are strong
evidence for these fixed inputs, not a proof for arbitrary activations.

The scratch harness was based on `n20-q2-opencl-bench.c`, with the existing
copy-inclusive texture measurement and full-output hash instrumentation.
It compiled with Android NDK r30 at `-O3 -Wall -Wextra -Werror`; both
candidate kernels compiled on the phone and completed all measured runs.
Temporary tensors, kernels and executables on the phone were removed after
recording the findings; the Windows scratch directory went to the Recycle
Bin. The shared GPU lock was retained.
