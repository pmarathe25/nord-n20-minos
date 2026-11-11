# GPU/DSP partition and kernel screen

2026-11-10. These are isolated phone projection measurements using real Bonsai layer-0 Q2 weights and deterministic synthetic activations. They include the operations described below, not model startup, retrieval, answer generation or gameplay.

## Row partition

The dynamic GPU–Hexagon up-projection harness included GPU reduction, Q8 packing, guard synchronization, GPU work, FastRPC and output assembly. At 256 tokens it used two sequential DSP calls. Each number is the median of five samples. The 3,072-row controls bracketed the other shard sizes.

| DSP rows | 128 tokens, ms | 256 tokens, ms |
| ---: | ---: | ---: |
| 2,048 | 48.689 | 97.920 |
| 2,560 | 45.229 | 91.691 |
| 3,072 before / after | 42.297 / 42.945 | 85.582 / 85.754 |
| 3,328 | 44.728 | 84.905 |
| 3,584 | 47.539 | 88.806 |
| 3,840 | 50.541 | 94.746 |
| 4,096 | 53.368 | 100.164 |

The best tested 128-token split was 3,072 rows. At 256 tokens, 3,328 was about 0.7–0.8 ms faster than the controls; the small isolated difference needs full-model and quality testing. All runs passed their GPU shard, Q8 packing, DSP sampled integer and scatter checks.

A scratch full-model follow-up combined 256-token gate/up activation reuse with either shard size. On one 323-token Wiki prompt, 3,072 rows took 27.7983 seconds to first token and 3,328 rows took 27.9218 seconds. The first token and next eight token IDs matched, but the logits hash changed because the larger DSP shard approximates additional rows. This single unbracketed comparison does not show a full-model benefit for 3,328 rows and does not establish quality equivalence; the existing 3,072-row choice remains the better-supported one.

## Hexagon tile width

The 10- and 12-token HVX tile variants were compared with the existing bounded 16-token variant, using shared ION and full output-buffer hashes. The table gives median FastRPC round-trip time over the last three of four 128-token calls, in milliseconds; output hashes matched the controls for each tested batch and shape and sampled scalar checks passed.

| Projection | Existing before | 10-token tile | 12-token tile | Existing after |
| --- | ---: | ---: | ---: | ---: |
| Up 9,216 × 2,560 | 100.163 | 100.210 | 105.769 | 100.469 |
| Down 2,560 × 9,216 | 321.360 | 323.807 | 329.330 | 321.709 |

Neither candidate improved these long-batch projections. The experimental sources are `v66-q2-hvx-10.c` and `v66-q2-hvx-12.c`; matching hashes do not prove equality for all inputs.

A subsequent four-row-tile panel traversal (`v66-q2-hvx-panel4.c`) also lost under GPU concurrency. For a 3,072-row DSP shard, the complete 128-token mixed projection took 53.337 ms versus 42.661/42.424 ms controls; at 256 tokens it took 100.261 ms versus 83.803/83.745 ms. The candidate passed full scalar integer checks for 393,216 and 786,432 DSP outputs respectively. Changing traversal also changes generated code and register allocation, so these timings do not isolate a cache cause.

## One versus two DSP calls at 256 tokens

An experimental module raised the software RPC token cap and processed all 256 tokens in one call; the comparison used the same raised-cap module for both arms. The one-call mixed projection took 221.494 ms versus 85.172/84.794 ms for the two-call controls, including GPU work, packing and scatter. In representative samples, DSP kernel time was 208.435 ms for one call and 67.406/67.446 ms total for the two-call arms. The cause was not isolated. All 786,432 DSP integers passed a full scalar check for one process in each mode, canonical full-buffer hashes matched, and an alternating-input test passed. The faster two-call design remains selected for the experimental model path.

## GPU conversion layout

An image-prefill kernel converted sixteen Q2 fields together rather than four separate four-lane conversions. Five-sample median GPU times, including the buffer-to-image copy, were 58.230 ms versus 55.588/55.712 ms for the 128-token up projection, and 116.276 ms versus 112.651/112.399 ms at 256 tokens. The down projection also slowed: 58.598 versus 56.697/56.581 ms at 128, and 115.549 versus 111.656/111.863 ms at 256. Complete output-buffer hashes and sampled scalar checks matched in these cases. The candidate is `n20-q2-image-convert16-prefill.cl` and is not selected. This result shows that fewer source-level conversions did not translate to a win on the tested driver.

The trials used the shared accelerator lock, a private FastRPC mount namespace, the existing idle-checked server stop and resource guard. Minecraft was restored and the CDSP returned offline with zero recorded crashes; no installed service or firmware was changed.
