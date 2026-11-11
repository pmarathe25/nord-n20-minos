# Branch-free ternary mask on the activation texture

2026-11-10. This scratch experiment revisited the FP16 sign/magnitude kernel without its per-code branch. The idea was prompted by the bit-mask arithmetic in an [Adreno OpenCL runtime](https://github.com/a8nova/adreno-llms/blob/2b1aee9fdd7d285b35292a9a73cc4f02c94bb65c/src/models/bonsai-27b/kernels/q1_gemv.cl), which targets a different Q1_0 format and model. Its reported performance is not directly comparable to this device or ternary kernel.

The candidate uses bitwise sign and magnitude operations for the three codes representing -1, 0 and +1. It does not implement the fourth Q2_0 code (+2) and assumes finite activations. Both selected layer-0 up and down weight tensors were scanned before execution; neither contained the fourth code. This is a standalone specialization without the necessary model-wide validation or fallback, and must not be used as a general backend.

The C.24 Adreno driver processed real Bonsai 4B layer-0 weights and deterministic FP16 activations. The bracketed baseline/candidate/baseline test used 128-thread groups and eight-token tiles, the shared GPU lock, CPU cores 0–2 and nice 15. Each five-sample median covers buffer-to-image copy and projection, excluding compilation, host packing, allocation, initial upload and checking.

| Projection | Tokens | Baseline before | Branch-free mask | Baseline after |
| --- | ---: | ---: | ---: | ---: |
| Up 9,216 × 2,560 | 16 | 7.143 ms | 16.012 ms | 7.115 ms |
| Up | 128 | 55.823 ms | 126.371 ms | 55.538 ms |
| Up | 256 | 110.968 ms | 252.504 ms | 111.712 ms |
| Down 2,560 × 9,216 | 16 | 7.604 ms | 16.757 ms | 7.626 ms |
| Down | 128 | 56.869 ms | 126.726 ms | 56.857 ms |
| Down | 256 | 113.020 ms | 252.604 ms | 111.876 ms |

The candidate was over twice as slow in each case, despite matching the complete output-buffer FNV hashes and sampled scalar-reference errors of both controls. It was not integrated or tested as a complete model or chat request. These checks cover the selected tensors and synthetic inputs, not all Q2_0 values, NaNs, infinities, or other tensors. The scratch kernel is `wiki/experiments/n20-q2-image-ternary-mask-prefill.cl`.

Minecraft remained running and passed its status check. Minimum observed available memory was 4,301,508 KiB, maximum battery temperature 25.9 °C; no installed worker or device configuration was changed. Scratch inputs, binaries and logs were removed.
