# Four-trit activation lookup texture

2026-11-10. An experimental GPU prefill kernel precomputes the contributions of all 81 patterns of four ternary weights for each four-column activation group and token. It preserves each 64-column row scale, stores four token values per RGBA FP16 image pixel, and gathers those contributions for four output rows and eight tokens at once. The experimental host packs each Q2_0 byte into a base-three pattern index and rejects any tensor containing code 3 (+2). The complete activation table is constructed on the CPU before timing and uploaded when creating the image. A real implementation would need GPU table construction, lifecycle and synchronization; neither its cost nor CPU packing was charged to the candidate here.

This differs from the earlier [pairwise local lookup](2026-11-09-wiki-lut-followup.md), which rebuilt tables per workgroup and used local-memory barriers. The new table occupies 12.66 MiB for the 128-token up projection and 45.56 MiB for down. It requires only 16 four-term contribution additions per output per 64-column block, but has weight-dependent texture accesses and a much larger working set. The table's FP16 rounding and changed accumulation order also differ from the baseline.

The phone's C.24 Adreno driver ran real Bonsai 4B layer-0 Q2_0 weights and deterministic synthetic FP16 activations. Each five-sample median below covers a GPU event span. The baseline includes buffer-to-image copying and projection; the candidate includes **only projection from the already-built, already-uploaded table**. The scratch run used 128 tokens, four output rows/eight tokens per work item, 128-thread groups, the shared GPU lock, CPU cores 0–2 and nice 15. Minecraft remained running; gameplay was not measured.

| Projection | Baseline before | Prebuilt LUT gather | Baseline after | Baseline / LUT sampled NRMSE |
| --- | ---: | ---: | ---: | ---: |
| Up 9,216 × 2,560 | 55.582 ms | 96.947 ms | 56.112 ms | 0.001007 / 0.000635 |
| Down 2,560 × 9,216 | 56.108 ms | 93.698 ms | 56.519 ms | 0.001008 / 0.000609 |

Even without its table-building or upload costs, the LUT path was about 66–74% slower. It was not integrated or run as a full model. Its complete output hashes differed, as expected from the changed arithmetic; the sampled double-precision errors above are not full-model quality or range validation. The code-3 rejection covers the selected tensors only. NaNs, infinities, tails, dynamic table lifetimes and alternating inputs were not tested. No hardware counters were collected to isolate the slowdown.

The scratch kernel and harness are `wiki/experiments/n20-q2-activation-image-lut81.cl` and `wiki/experiments/n20-q2-lut81-bench.c`. They are research tools, not a deployable backend. The guard observed at least 4,280,480 KiB available and at most 25.5 °C battery temperature. Minecraft passed its status check; no installed worker, model, firmware, server configuration or world was changed. Scratch weights, binaries and logs were removed.
