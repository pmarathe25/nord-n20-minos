# Streaming Wiki comparison with the down projection

2026-11-10. Eight additional questions from `wiki/benchmark-fresh.json` were run through the checkout's actual streaming Wiki request and reply formatter against two separate scratch `llama-server` workers. The server used its embedded Jinja chat template and the production request's `enable_thinking: false`, 128-token microbatches, two efficiency-core threads, the parallel packer and gate/up pair reuse. The candidate additionally enabled the experimental 512-row Hexagon down projection. Both workers received the same frozen offline passages in the same order and warmed with the same separate question. The measured interval is the complete streaming model request and reply formatting; retrieval, worker startup, Minecraft chat delivery and gameplay are excluded. The workers ran baseline first, candidate second, with one measurement per question and no trailing control.

| Case | Baseline | Down enabled | Grounded check, baseline / candidate | Exact-answer route available |
| --- | ---: | ---: | --- | --- |
| j04 | 35.105 s | 35.107 s | fail / fail | yes |
| j05 | 47.190 s | 47.229 s | fail / fail | no |
| j07 | 21.747 s | 21.882 s | pass / pass | no |
| j12 | 25.671 s | 25.822 s | fail / fail | no |
| j14 | 25.087 s | 24.893 s | pass / pass | yes |
| j17 | 32.370 s | 32.617 s | pass / pass | no |
| j22 | 26.269 s | 26.313 s | fail / fail | no |
| j40 | 33.269 s | 33.324 s | pass / pass | no |

Both workers passed four of eight checks, with identical rendered replies and no transport errors or length stops. The private-state case explicitly abstained in both. The median paired candidate-minus-baseline request difference was **+0.050 s**; baseline and candidate medians were 29.319 and 29.465 s, respectively. This single-order sample does not establish a speedup or a meaningful slowdown. Server timing logs recorded a median paired prefill difference of −55 ms and a median paired decode difference of +97 ms; timings can vary and are not independent causal measurements.

The test deliberately forced model inference even when the exact-answer route was available. A separate replay of that route passed its existing checks for both j04 and j14, so the normal bridge would route those two cases differently; the table is not a measurement of their normal end-to-end player latency. The scorer checks expected text and the cited page title, not whether a passage actually entails the claim.

The failures also limit the usefulness of this runtime independently of speed. For j04, the model said 80 rather than eight and its malformed support line caused the bridge to return the unknown response; the exact-answer route would answer it correctly. For j05 it said yes but cited the Dispenser excerpt rather than an allowed Campfire source. For j12 it gave seven ticks and the bridge returned unknown because its support line lacked the required citation form. For j22 it gave one chain with malformed support; the retrieved Hanging Sign passages did not match the benchmark's expected evidence pattern. Those pattern checks are a diagnostic, not a complete human audit of all evidence or a definitive retrieval/model classification.

The baseline reported 64 resident slots and 620 of 620 eligible gate/up pairs reused; the candidate reported 96 slots, 620 of 620 pairs reused and **620 of 620 down projections dispatched**, with no recorded fallbacks or upload failures. These aggregate counters include the warmup and do not prove coverage or accuracy for every request. Both arms used the approximate Q8 gate/up path; the candidate additionally quantized down projections. Eight cases are insufficient to establish broad answer-quality equivalence. Astra's independent static review found no new concrete bug in the shape dispatch, dual-buffer bounds, pair readiness or queue-ordered fallback, but did not replace device testing.

The guard observed at least 2,600,880 KiB available memory and at most 29.7 °C battery temperature. Minecraft's status query passed after restoration and CDSP returned offline with zero reported crashes. No installed service, model, firmware, partition or world changed; private fixtures, results, copied vendor image, binaries and raw logs were removed.
