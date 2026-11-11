# Hybrid microbatch and activation reuse

2026-11-10. These are scratch full-model prefill experiments using Bonsai 4B on the phone; neither path was installed. Both used the previously tested two-column Gated DeltaNet option, the tuned GPU texture path, two CPU threads and a 323-token held-out Wiki prompt with approximate chat framing. Timings end at the first token and exclude model load, later generation, retrieval and gameplay. One run per arm, except the first GPU process, is too little to establish a robust result.

| Arm | Microbatch | First-token seconds | Full-logit FNV hash |
| --- | ---: | ---: | --- |
| GPU, first process, warm second run | 256 | 29.5386 | 313943a17a80d567 |
| GPU + Hexagon, two RPCs when needed | 256 | 28.1334 | 484b5f9f3bc76cb9 |
| GPU, after | 256 | 29.4632 | 313943a17a80d567 |
| GPU + Hexagon | 128 | 27.9305 | 484b5f9f3bc76cb9 |
| Hybrid pair reuse disabled, before | 128 | 27.8639 | 484b5f9f3bc76cb9 |
| Hybrid pair reuse enabled | 128 | 27.6879 | 484b5f9f3bc76cb9 |
| Hybrid pair reuse disabled, after | 128 | 28.1790 | 484b5f9f3bc76cb9 |

The first GPU run was 37.9766 seconds with cold shader compilation and is excluded from the table. The 256-token hybrid was about 4.5–4.8% faster than its warm GPU controls in this trial; the 128-token hybrid was slightly faster than the 256-token hybrid, so the larger microbatch did not itself improve this request. All first tokens matched. Hybrid and GPU logits differ because the DSP arithmetic is approximate. The 256-token path reported 124 projection attempts, all dispatched without fallback; its 128-token run reported 186. It packs into two aligned ION buffers, executes one GPU shard alongside up to two 128-token FastRPC calls, and scatters both outputs.

The paired 128-token trial reuses packed activations, scales and the GPU texture only across strictly matched adjacent gate/up graph nodes. Its separate DSP output buffer prevents the second RPC from overwriting the first queued scatter. It reported 93 attempted and 93 reused pairs; the controls each dispatched 186 ordinary hybrid projections. First-token logits matched exactly among the paired and ordinary hybrid arms. The 0.18–0.49 second prefill difference is small, from a single bracketing run, and does not establish a consistent end-to-end gain.

A later scratch variant extended pair reuse to 256 tokens, using separate DSP output buffers for both 128-token halves of each projection. On the same 323-token prompt, the pair-enabled prefill took 27.7983 seconds versus 28.0523 seconds for the warm control after it; the preceding control was cold and took 38.7103 seconds, so it is not a useful timing bracket. All first-logit hashes and the first nine token IDs matched, and 62 of 62 eligible pairs reused their inputs. On a second prompt the pair-on and later pair-off prefills took 23.2875 and 23.5163 seconds and their first-logit hashes and token IDs matched. These are limited, single-order screens; repeated bracketing and failure-path checks are needed.

A warm-cache repeat on the 323-token prompt used two prefills per process in pair-off, pair-on, pair-off order. Pair-off first/second runs were 28.0847/27.7659 seconds before and 27.9827/27.6865 after; pair-on took 27.7283/27.4526. Each corresponding pair-on run was about 0.23–0.36 seconds faster than the controls, and all six first-logit hashes matched. The pair-on process reported 124 attempted and 124 reused pairs across both runs. This is still a small single-device prefill comparison, not a measured chat improvement.

For a targeted recovery test, a fault-only runtime forced the reused projection to fail after its first DSP call for batches over 128 tokens. It injected 31 failures; the backend recomputed those projections through the ordinary hybrid path and successfully reused the other 31 pairs. The first-logit hash and first nine token IDs matched ordinary-hybrid controls before and after. This covers that injected failure point on one prompt, not every possible queue, memory or RPC failure.

A separate scheduling variant queued both gate/up GPU shards before the sequential DSP calls, then scattered the two results. On the 323-token prompt it took 27.8966 seconds to first token versus 27.8486 and 27.7971 seconds for serial-pair controls. All first-logit hashes and the first nine token IDs matched. The candidate reported 62 batched dispatches and no fallbacks. This single screen did not show a benefit, so the new schedule was not selected; its five fault-injection points were not run on the phone. The scratch sources are `n20-hybrid-batched-pair-runtime.cpp` and `llama-b12110-n20-hybrid-batched-pair.patch` in `wiki/experiments/`.

An eight-question matched request comparison then used separate pair-off and pair-on workers with the same frozen offline evidence, model, 256-token microbatches and initially empty prompt caches. Both passed 5 of 8 automated checks, with no scored regressions or improvements and identical rendered text in all cases. One response in each arm reached the generation-length limit and was counted as a failure; both private-state cases explicitly abstained. The median paired request difference was −0.214 seconds. The pair-on worker reported 341 attempted and 341 reused pairs, while its ordinary hybrid counter recorded 341 dispatched first projections with no fallbacks. These aggregate counters do not prove per-request coverage, and the single-order sample is too small to establish a robust chat improvement. The test measures checkout streaming model requests, excluding retrieval, startup, actual Minecraft chat and gameplay; it also forces the model for one case where the exact-answer route was available. The guard observed at least 2,582,556 KiB available and at most 30.5 °C battery temperature, then restored Minecraft and verified its status; CDSP returned offline with zero crashes.

The 256-token backend/runtime and pair runtime have empirical magnitude guards, constrained shape and serialized-execution assumptions. They are not production-ready. Sources are `llama-b12110-n20-hybrid-256-trial.patch`, `n20-hybrid-runtime-256.cpp`, `n20-hybrid-aux-256.cl`, `llama-b12110-n20-hybrid-pair.patch` and `n20-hybrid-pair-runtime.cpp` in `wiki/experiments/`.

## Two-call fallback check

A separate full-model test used 256 repeated synthetic token IDs and an opt-in fault-only runtime that forces a failure immediately after the first successful DSP call for each batch over 128 tokens. All 62 eligible projections took the ordinary GPU fallback; the resulting first-token full-logit hash matched the GPU-only control (`750b04aa776fc746`). The non-fault hybrid dispatched all 62 and produced a different hash, as expected for approximate quantization. Runs at 129 and 255 synthetic tokens also completed with all 62 eligible projections dispatched and no fallbacks; this checks execution at those boundaries, not full numerical parity. The fault test covers this particular failure point and graph, not all RPC, driver, or cleanup failures.

## Matched Wiki requests

Eight held-out questions used the same frozen offline evidence and order in separate GPU-only and 256-token hybrid workers. Both had the tuned GPU settings and confirmed `N20_GDN_CPL=2`, a fresh worker and an initially empty prompt cache. This measures model request time through the checkout's streaming Unix-socket bridge; it excludes retrieval, startup, Minecraft chat and gameplay. It deliberately exercises the model even for one case where the exact-answer route was available.

| Case | GPU seconds | Hybrid seconds | Automated check, GPU / hybrid |
| --- | ---: | ---: | --- |
| j03 | 61.706 | 60.185 | fail / fail |
| j10 | 23.996 | 23.115 | pass / pass |
| j13 | 28.366 | 27.224 | fail / fail |
| j18 | 35.417 | 34.410 | pass / pass |
| j25 | 84.943 | 83.328 | fail / fail |
| j32 | 35.060 | 32.589 | pass / pass |
| j37 | 40.091 | 39.005 | pass / pass |
| j39 | 21.969 | 21.318 | pass / pass |

Both arms passed 5 of 8 checks, with no scored regressions or improvements. One rendered answer changed text but passed in both arms; two private-state cases explicitly abstained in both arms. One response in each arm reached the generation-length limit and was counted as a failure. Neither arm had transport errors. The median paired time difference was −1.114 seconds; the hybrid worker reported 682 dispatched projections and zero recorded fallbacks across the run. Counters are aggregate, and the small single-order sample does not establish robust speed or answer-quality equivalence.

The guard recorded at least 2,614,112 KiB available memory and at most 29.6 °C battery temperature. It restored Minecraft and verified its status; CDSP returned offline with zero recorded crashes. Private fixtures, answers and raw logs were kept in scratch only.
