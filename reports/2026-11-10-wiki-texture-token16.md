# Sixteen-token activation-texture tile

2026-11-10. A GPU-only scratch experiment extended the existing four-row activation-texture kernel from eight to sixteen tokens per work item. It reads four RGBA FP16 image pixels per input value and uses 16-lane partial and outer accumulator vectors. Quantized weights, 64-column FP16 partial sums, FP32 outer sums and the arithmetic order per output remain the same. The experiment changes the work-item tile, not the model or its quantization.

The phone used the existing C.24 Adreno driver, real layer-0 Bonsai 4B Q2_0 weights and deterministic synthetic FP16 inputs. Each case ran an eight-token baseline, the sixteen-token candidate, and the baseline again under the shared accelerator lock on CPUs 0–2 at nice 15. Each value is a median of five GPU event spans, including the buffer-to-image copy and projection, but excluding compilation, packing, allocation, uploads and verification. Minecraft remained running; gameplay was not measured.

| Shape | Tokens | Baseline before | 16-token candidate | Baseline after |
| --- | ---: | ---: | ---: | ---: |
| Up 9,216 × 2,560 | 15 | 7.118 ms | 15.923 ms | 7.187 ms |
| Up | 16 | 7.130 ms | 16.929 ms | 7.147 ms |
| Up | 128 | 55.552 ms | 123.051 ms | 55.555 ms |
| Up | 256 | 110.946 ms | 257.730 ms | 112.053 ms |
| Down 2,560 × 9,216 | 15 | 7.697 ms | 18.850 ms | 7.616 ms |
| Down | 16 | 7.624 ms | 18.636 ms | 7.645 ms |
| Down | 128 | 56.573 ms | 126.891 ms | 56.688 ms |
| Down | 256 | 111.531 ms | 242.443 ms | 111.592 ms |

The candidate was over twice as slow in every case, so it was not integrated into the model or measured as a full chat request. Complete output-buffer FNV hashes matched both controls in every case; sampled scalar-reference normalized RMS errors also matched. The 15-token case checked a partial token tile. These checks cover the tested inputs only and do not establish correctness for arbitrary shapes or values. Register pressure or spills might explain the result, but hardware counters were not measured.

The experimental source is `wiki/experiments/n20-q2-activation-image-token16.cl`. The guard observed at least 4,307,652 KiB available memory and at most 26.4 °C battery temperature. No installed worker, firmware, model, server configuration, or world was changed; scratch weights, binaries and raw logs were removed.
