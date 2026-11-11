# Nord N20 Q2_0 weight-image prefill trial, November 10, 2026

The Adreno 619 projection harness used real layer-0 up and down weights
from the installed Ternary Bonsai 2 4B Q2_0 model and deterministic FP16
activations. The tuned baseline used four rows per work item, 128-thread
workgroups, 64-column FP16 partial sums and an FP32 accumulator. It used
four token lanes for a four-token batch and eight lanes for larger batches.
All variants used the same FP16 activation image and separate scale buffer.

The experiment viewed the packed Q2 code buffer as an OpenCL
`image1d_buffer_t` and fetched its unsigned integer pixels with
`read_imageui`. Four-byte pixels used either four 8-bit channels
(`CL_RGBA`, `CL_UNSIGNED_INT8`) or one 32-bit channel
(`CL_R`, `CL_UNSIGNED_INT32`). Both modes preserved the tuned buffer
layout. The driver reported a maximum 1D buffer image width of
134,217,728 pixels, and each of these images used 1,474,560 pixels.
[OpenCL C specification](https://registry.khronos.org/OpenCL/specs/unified/html/OpenCL_C.html#image-read-functions)

The wider-pixel trial used four 32-bit channels. The original layout
spanned 16 adjacent rows in one pixel, with each work item selecting one
four-row word. A second layout packed four consecutive groups of four
input columns into the same pixel for each four-row work item. Its
matched buffer control loaded the same four words with `vload4`. Both
layouts used 368,640 pixels per weight matrix and preserved the number
of packed code bytes.

Each variant had two warmups and ten timed launches. Each comparison
bracketed the variants with two baseline runs and pooled those twenty
timings. The table gives median GPU event time in milliseconds and
excludes packing, transfers and compilation.

| Projection | Tokens | Buffer | RGBA8 image | R32 image |
| --- | ---: | ---: | ---: | ---: |
| Down | 4 | 3.010 | 3.195 | 3.242 |
| Down | 8 | 4.673 | 4.815 | 4.825 |
| Down | 16 | 8.284 | 8.530 | 8.499 |
| Down | 32 | 15.921 | 16.361 | 16.297 |
| Down | 128 | 61.486 | 63.206 | 63.621 |
| Down | 256 | 123.214 | 125.765 | 125.108 |
| Up | 4 | 2.545 | 2.745 | 2.739 |
| Up | 8 | 3.994 | 4.104 | 4.094 |
| Up | 16 | 7.788 | 8.009 | 8.043 |
| Up | 32 | 15.479 | 15.914 | 15.796 |
| Up | 128 | 62.164 | 63.289 | 63.538 |
| Up | 256 | 122.309 | 125.643 | 125.114 |

The four-byte image modes were 1.5% to 7.8% slower, with the largest
penalties at four tokens.

| Projection | Tokens | Buffer | RGBA32 image | Repacked RGBA32 image | Repacked buffer |
| --- | ---: | ---: | ---: | ---: | ---: |
| Down | 4 | 3.045 | 3.768 | 3.021 | 2.897 |
| Down | 8 | 4.671 | 5.890 | 6.924 | 6.757 |
| Down | 16 | 8.349 | 9.717 | 12.578 | 12.155 |
| Down | 32 | 16.005 | 18.904 | 28.096 | 27.742 |
| Down | 128 | 61.748 | 70.477 | 107.619 | 105.546 |
| Down | 256 | 122.377 | 139.878 | 217.054 | 213.780 |
| Up | 4 | 2.551 | 3.223 | 2.640 | 2.554 |
| Up | 8 | 4.006 | 4.715 | 5.766 | 5.514 |
| Up | 16 | 7.863 | 9.223 | 12.560 | 12.867 |
| Up | 32 | 15.595 | 17.855 | 29.035 | 24.752 |
| Up | 128 | 61.469 | 70.433 | 109.019 | 104.761 |
| Up | 256 | 122.312 | 141.916 | 214.791 | 204.232 |

All output bytes hashed identically across the five alternatives and the
baseline for every tested shape. Normalized RMS error against the sampled
double-precision reference was 0.00080 to 0.00155. At four tokens on the
down projection, the repacked image was within 1% of the tuned buffer.
The matched repacked buffer ran faster, and the repacked image lost its
small advantage in a subsequent smoke test. At eight tokens and above,
all image modes were slower than the tuned buffer.

## Two- to four-token tail

The follow-up ran the tuned four-lane tail, repacked buffer, fixed
eight-way split-K, and repacked buffer with eight-way split-K. Each
variant ran twice per configuration in forward and reverse order.
The numbers are medians of twenty timed launches. Split-K timing spans
projection start to reduction end and includes the reduction.

| Projection | Tokens | Tuned tail | Repacked tail | Split-K | Repacked split-K |
| --- | ---: | ---: | ---: | ---: | ---: |
| Down | 2 | 3.000 | 2.877 | 2.682 | 2.674 |
| Down | 3 | 3.019 | 2.891 | 2.689 | 2.703 |
| Down | 4 | 3.010 | 2.875 | 2.697 | 2.720 |
| Up | 2 | 2.558 | 2.474 | 2.521 | 2.518 |
| Up | 3 | 2.553 | 2.482 | 2.548 | 2.572 |
| Up | 4 | 2.547 | 2.485 | 2.581 | 2.601 |

The down tail gained 4.1% to 4.5% from repacking alone. Split-K was
faster and repacking offered no consistent additional benefit. The up
tail gained 2.4% to 3.3% in this follow-up, while the first four-token
comparison showed no gain. Repacking preserved each algorithm's full
output hash. Split-K changed the FP32 summation order, and its sampled
normalized RMS error stayed within 0.00061 to 0.00095, the same range as
the unsplit kernels. These results stopped at projection testing.

## Reproduction

Apply [the harness patch](../wiki/experiments/n20-q2-image-trial.patch)
in a scratch copy and build the projection harness with the Android
ARM64 toolchain. Extract `blk.0.ffn_up.weight` and `blk.0.ffn_down.weight`
using the [weight extraction instructions](../docs/wiki-chat.md#rust-vulkan-compute-experiments).
Use the C.24 OpenCL libraries and a private cache as described in the
[GPU worker notes](../docs/wiki-chat.md#gpu-worker-setup).

Run `n20-q2-prefill.cl` with the same value for the environment variable
`N20_CODE_IMAGE` and compiler define `-DN20_CODE_IMAGE`. Mode 0 is the
tuned buffer. Modes 1 and 2 are the RGBA8 and R32 images. Modes 3 and 4
are the original and repacked RGBA32 images. Mode 5 is the repacked
buffer. Use compiler flags `-DN20_PARTIAL=64 -DN20_TILE_TOKENS=4` and
final arguments `4 4` for a four-token tail. Use an eight-token tile and
final arguments `4 8` for the larger batches.

Use `n20-q2-prefill-splitk.cl` with code modes 0 and 5 for the follow-up.
Set `N20_SPLIT_K` in the environment and pass the matching
`-DN20_SPLITS` define, either 1 or 8, and set `-DN20_COLUMNS` to the
input width. Use the four-token tile and final arguments `4 4` for
each tail batch.
