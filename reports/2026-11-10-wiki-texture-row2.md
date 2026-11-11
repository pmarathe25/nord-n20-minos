# Two-row activation-texture tile

2026-11-10. A GPU-only scratch kernel computes two output rows per work item instead of four, retaining the existing eight-token activation texture, 64-column FP16 partial sums, FP32 outer sums and per-output arithmetic order. This reduces the number of private accumulators per work item but doubles the row work items; no register-allocation or hardware counters were measured.

The phone used real Bonsai 4B layer-0 Q2_0 weights, deterministic FP16 activations, 128-thread workgroups, the shared GPU lock, CPU cores 0–2 and nice 15. Each five-event median includes the GPU buffer-to-image copy and projection, excluding compilation, allocations, initial uploads and checking. Minecraft remained running, but gameplay was not measured.

| Projection | Tokens | Four-row before | Two-row | Four-row after |
| --- | ---: | ---: | ---: | ---: |
| Up 9,216 × 2,560 | 16 | 7.127 ms | 12.668 ms | 7.165 ms |
| Up | 128 | 55.549 ms | 105.058 ms | 55.510 ms |
| Up | 256 | 111.488 ms | 209.589 ms | 112.697 ms |
| Down 2,560 × 9,216 | 16 | 7.665 ms | 14.913 ms | 7.626 ms |
| Down | 128 | 56.871 ms | 103.575 ms | 56.769 ms |
| Down | 256 | 111.690 ms | 209.300 ms | 112.109 ms |

The candidate was approximately 1.8–2.0 times slower and was not integrated or tested as a full model. Complete output-buffer FNV hashes and sampled scalar-reference errors matched between candidate and controls in each case. These checks cover the tested inputs, not arbitrary tensors. The source is `wiki/experiments/n20-q2-activation-image-row2.cl`. Minimum observed available memory was 4,310,812 KiB and maximum battery temperature 26.2 °C. Scratch weights, logs, and binaries were removed; the installed worker and device state were unchanged.
