# Parallel Q8 activation packing

2026-11-10. A scratch [OpenCL packer](../wiki/experiments/n20-q2-parallel-pack.cl) assigns sixteen lanes to the 64 values in each token/block, with eight adjacent tokens per workgroup. Each lane writes one packed word, and a local reduction produces the original block sum. The packed output layout, per-token scaling and existing finite/range guard are unchanged. The [row-view harness](../wiki/experiments/n20-q2-overlap-rowview-bench.c) selects it only when `N20_PARALLEL_PACK_SOURCE` points to the source; without that option it retains its old path. It is not installed in the model runtime.

The standalone screen measured GPU events from reduction through packing using deterministic FP16 inputs, ordinary OpenCL buffers and five samples per arm. For 9,216 columns and 128 tokens, the existing coalesced implementation took 13.912/13.931 ms before/after the parallel candidate, which took 1.517 ms. At 33 tokens it took 3.826/3.831 ms versus 0.747 ms. For the 2,560-column up shape at 128 tokens, controls took 3.368/3.377 ms and the candidate 0.418 ms. Every token maximum and packed byte matched the CPU reference for these fixtures. These are isolated kernel intervals, not complete inference times.

## Complete mixed down-projection screen

The phone then ran the real Bonsai layer-0 down weights with 128 synthetic tokens, dynamic Q8, the 32-token-blocked Hexagon kernel and the same shared-ION row-view harness as the [previous down screen](2026-11-10-wiki-down-pack-screen.md). Timings include reduction, packing, guard synchronization, GPU copy and shard, DSP RPC and compute, and scatter. Each entry is a median of five samples; weight setup and model loading are excluded. The old coalesced packer bracketed the new 512-row path, and every mixed arm was also bracketed by GPU-only controls.

| Packer and DSP rows | GPU-only before | Mixed | GPU-only after | Pack kernel |
| --- | ---: | ---: | ---: | ---: |
| Existing, 512 | 57.464 ms | 60.810 ms | 57.515 ms | 13.519 ms |
| Parallel, 512 | 57.405 ms | 48.242 ms | 57.391 ms | 1.117 ms |
| Parallel, 768 | 57.378 ms | 65.998 ms | 57.338 ms | 1.106 ms |
| Parallel, 256 | 57.452 ms | 59.304 ms | 57.371 ms | 1.113 ms |
| Parallel, 512 repeat | 57.695 ms | 48.569 ms | 57.711 ms | 1.124 ms |
| Existing, 512 after | 57.145 ms | 60.560 ms | 57.150 ms | 13.531 ms |

The two 512-row candidate medians were approximately 16% faster than their GPU-only controls for this projection. DSP timing varied: candidate mixed samples ranged from 48.223 to 54.722 ms in the first worker and 48.478 to 54.549 ms in the repeat. All DSP integers matched the scalar fixed-point reference, packed bytes and shared-buffer visibility matched, GPU shard and scatter checks passed, and six alternating-input checks passed per arm. The candidate also passed full integer checks at 31, 32, 33 and 127 tokens. Separate 33-token large-finite and NaN injections each took all six expected GPU fallbacks and matched the full-GPU non-NaN values and NaN masks.

This is a single layer and synthetic activation distribution, not a measured full-model, answer-quality, or gameplay improvement. The DSP path uses approximate Q8 arithmetic and its safety threshold is empirical. A later [scratch model-runtime test](2026-11-10-wiki-parallel-pack-model.md) integrated the packer for the existing gate/up split; it did not integrate the down split. Testing actual chat latency, quality and active-player impact is required before deployment. The installed GPU and model paths remain unchanged.

Both guarded DSP runs restored Minecraft and passed its status query; CDSP was offline with zero reported crashes. Minimum sampled available memory was 5,026,216 KiB and peak sampled battery temperature was 27.1 °C. Standalone GPU screens kept Minecraft running; scratch binaries, copied weights, vendor image and logs were removed. No firmware, installed configuration or world data was changed.
