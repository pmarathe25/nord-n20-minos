# Dynamic down-projection packing screen

2026-11-10. The preceding [down-projection trial](2026-11-10-wiki-hexagon-down-block32.md) used the row-view harness's default per-token reduction and original pack order. That harness already contains an eight-token coalesced reduction and adjacent-token pack order, both previously tested on the shorter up-projection shape. This screen measures those existing options on the 9,216-column down-projection activation shape before another inclusive GPU–DSP test.

The new [standalone harness](../wiki/experiments/n20-q2-dynamic-pack-bench.c) compiles the actual kernels from the row-view harness, generates deterministic FP16 activation inputs with zero, unit and eightfold token magnitudes, and compares all maxima and packed bytes against the existing CPU reference. It uses ordinary OpenCL buffers, not the mixed path's shared ION buffers. Five GPU-event intervals per option cover reduction through packing, excluding buffer setup, transfers, RPC, GPU projection, DSP work and scatter. Original settings bracket the other three variants.

| Tokens | Original before | Adjacent-token pack | Eight-token reduction | Both options | Original after |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 128 | 26.319 ms | 16.984 ms | 23.243 ms | 13.925 ms | 26.342 ms |
| 127 | 23.883 ms | 15.209 ms | 20.810 ms | 12.134 ms | 23.730 ms |
| 33 | 5.545 ms | 3.669 ms | 5.714 ms | 3.823 ms | 5.545 ms |
| 32 | 5.580 ms | 3.223 ms | 5.733 ms | 3.383 ms | 5.577 ms |
| 31 | 5.614 ms | 3.401 ms | 5.786 ms | 3.545 ms | 5.654 ms |

All variants matched every packed byte and token maximum for these finite fixtures, including tails. At 128 tokens, using both options reduced this isolated interval by about 47%; at the short sizes the adjacent-token order alone was faster than the combination. The comparison does not measure the guard's fallback behavior, ION coherence, full projection, numerical accuracy on other inputs, or model latency. In particular, the previous mixed path also spent about 63 ms in its DSP kernel, so the isolated saving does not establish a mixed or end-to-end win over the 57 ms GPU-only projection. An inclusive mixed-path comparison with the optimized packing and different DSP row splits is the next required test.

## Inclusive mixed projection

The subsequent phone trial combined both coalesced options with the 32-token-blocked DSP kernel and the same real layer-0 down weights and deterministic inputs as the prior screen. It includes reduction, packing, synchronization, both compute shards, RPC and scatter. Each number is a five-sample median; GPU-only controls bracket every mixed arm. Weight setup and model loading are excluded. All arms used 128 tokens; the 256-row candidate was repeated at the end.

| DSP kernel and rows | GPU-only before | Mixed | DSP kernel | GPU-only after |
| --- | ---: | ---: | ---: | ---: |
| Original, 256 | 56.829 ms | 69.831 ms | 34.024 ms | 56.875 ms |
| Blocked, 256 | 57.660 ms | 72.075 ms | 23.602 ms | 57.622 ms |
| Blocked, 512 | 57.551 ms | 60.824 ms | 43.660 ms | 57.564 ms |
| Blocked, 768 | 57.598 ms | 78.574 ms | 62.919 ms | 57.572 ms |
| Blocked, 256 repeat | 57.562 ms | 72.028 ms | 23.658 ms | 57.627 ms |

The best mixed case, 512 rows, was approximately 5.7% slower than its GPU controls, so this experiment does not justify integration. The original 256-row control ran first and its GPU control was faster than later controls; its lower mixed time than the blocked 256-row cases cannot be attributed solely to DSP traversal. All DSP integers matched the scalar fixed-point reference for every sample, including six alternating-input checks per arm; packed bytes, shared-buffer visibility, GPU-shard parity and assembly checks passed. This is not a complete-model or gameplay comparison. Fallback behavior with the coalesced options was not retested in this run.

The standalone harness and the normal row-view executable compiled with Android NDK r30 and warnings treated as errors (the inclusion-based screen disables unused-function warnings). Both screens held the GPU lock and used efficiency cores and reduced priority. Minecraft stayed running during the standalone screen; it was idle-checked and stopped for the DSP trial, then its status query passed. Available memory after the standalone screens was at least 4,360,776 KiB and battery temperature was 26.6 °C. The mixed-run guard observed at least 5,028,772 KiB available and at most 26.6 °C, and CDSP returned offline with zero reported crashes. Scratch files were removed and no installed configuration, firmware or world was changed.
