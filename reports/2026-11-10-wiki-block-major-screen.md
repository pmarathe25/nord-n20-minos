# GPU and Hexagon block-major projection screen

2026-11-10. These are experimental scratch runs with the existing Bonsai 4B model on the Nord N20, not installed service changes. The mixed projection uses 3,072 DSP rows of the 9,216 by 2,560 gate/up matrix, with the remaining rows on the GPU. It includes GPU quantization and packing, FastRPC execution, GPU projection, and assembly. Each number is a median of five samples; runs bracketed by controls are shown in order. The DSP computes with approximate Q8 activations.

| Tokens | Token-major before | Block-major | Token-major after |
| ---: | ---: | ---: | ---: |
| 16 | 7.546 ms | 6.246 ms | 7.533 ms |
| 64 | 21.762 ms | 21.721 ms | 21.932 ms |
| 128 | 42.757 ms | 42.620 ms | 42.859 ms |

The block-major DSP activation layout improved the 16-token projection by about 17%; the longer cases are dominated by the GPU shard and do not show a meaningful benefit. A row sweep from 3,072 to 4,096 DSP rows did not improve the 16-token result; at 64 and 128 tokens changes were small until larger splits became slower. Full scalar references checked all 49,152, 196,608 and 393,216 DSP outputs for 16, 64 and 128 tokens respectively, along with alternating inputs, GPU packing and output assembly. This is a projection result, not an end-to-end answer time.

In an independent 256-token two-RPC trial the new layout took 85.157 ms and its GPU-only controls took approximately 112.9 ms. All 786,432 DSP outputs passed a scalar reference; GPU packed bytes, shared-memory visibility, GPU shard and result scatter checks passed. Alternating inputs also passed. This does not establish a gain against the existing two-RPC hybrid, which was also about 85 ms in a separate screen.

A single 256-token RPC, even internally split into two 128-token blocks, took about 220 ms versus about 85 ms for two calls. It passed the same full scalar and alternating-input checks but is not selected. A separate sign/magnitude GPU kernel replacing ternary multiplication matched the full output hash but was approximately three times slower on 128- and 256-token projections, so it is not selected.

The new block-major DSP kernel and auxiliary GPU packer are in `wiki/experiments/v66-q2-hvx-block-major.c` and `wiki/experiments/n20-hybrid-aux-block-major-256.cl`; the distinct module and runtime names prevent mixing activation layouts. The optimized path is experimental: it relies on the existing bounded-activation and serialized-runtime assumptions. Tests were run with the idle server stopped under the resource guard; Minecraft was restored, its status check passed, and CDSP returned offline with zero recorded crashes. These kernel screens do not show the requested order-of-magnitude end-to-end improvement.

## Full-model check

The 256-token implementation was then integrated into the same experimental pair-reuse model backend and compared with the token-major hybrid runtime in before/candidate/after order. The held-out question used four passages from a frozen offline Wiki cache and approximate chat framing, for 323 prompt tokens. Each fresh worker ran two identical prefills, using two CPU threads, the same Bonsai model, 256-token microbatches and two-column Gated DeltaNet. The first prefill in each process included lazy GPU shader compilation; the second is the warm comparison. These times exclude model loading, retrieval, Minecraft chat, and gameplay; one decode step does not measure a complete answer.

| Runtime | First prefill, includes compilation | Warm first token | Warm one-token decode |
| --- | ---: | ---: | ---: |
| Token-major before | 38.2813 s | 27.2482 s | 0.4522 s |
| Block-major | 38.3658 s | 27.4158 s | 0.4569 s |
| Token-major after | 38.3431 s | 27.3221 s | 0.4537 s |

The candidate was 94–168 ms slower than the bracketing controls in this one prompt, so it is not selected as an end-to-end optimization. All six iterations generated the same first two token IDs; full logits and complete answers were not checked. Every worker reported 124 of 124 projections dispatched and 124 of 124 eligible pairs reused across its two iterations, with zero recorded fallbacks. These counters establish activity in this run, not correctness for arbitrary graphs or production readiness. The guard recorded at least 2,606,388 KiB available memory and a maximum 28.2 °C battery temperature.
