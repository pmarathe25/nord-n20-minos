# Parallel packer in the scratch hybrid model

2026-11-10. The experimental `n20-hybrid-pair-256-runtime.cpp` now offers `N20_HYBRID_PARALLEL_PACK` to select the new parallel packer for its existing 9,216-by-2,560 gate/up split. The option is read at runtime initialization, reports a selection trace, and retains the two ION input buffers, reduction and guard ordering, two independent pair-output buffers and existing fallback behavior. The candidate writes the first 128 tokens to one buffer and the remainder to the second. The tested [down-projection split](2026-11-10-wiki-parallel-activation-pack.md) is not integrated into the model runtime. The option and runtime are scratch-only and were not installed.

An Android OpenCL screen compared every packed word and maximum against the CPU reference, alongside the existing GPU packer, at 16, 17, 127, 128, 129, 135, 255 and 256 tokens. The new dual-buffer path matched for these deterministic finite inputs and preserved canaries in each unused buffer tail. This does not cover all rounding boundaries, successive changing batch sizes within one worker, or all possible inputs. The runtime and screen compiled with Android NDK r30 and warnings as errors; the screen suppresses unused-function warnings from its included harness.

The guarded model test used Bonsai 4B, two held-out Wiki questions and offline evidence with approximate chat framing, 256-token microbatches and two efficiency-core threads. Each of six fresh workers computed the 106-token system prefix and saved it in memory before two measured requests. The table records first-token inference including in-memory state restore, followed by eight additional greedy decode steps. Model load, prefix construction, retrieval and Minecraft chat delivery are excluded. The shader cache was warm; the candidate was bracketed by controls for each question.

Later inspection of the model's embedded template showed that this approximate framing omitted the suffix used when `enable_thinking` is false. The nine generated tokens in this test were the opening of a thinking trace, not a player answer. These timings and parity results remain valid for that approximate input, but should not be treated as representative of production answer quality or exact production framing. See the corrected [down-projection model trial](2026-11-10-wiki-hybrid-down-model.md).

| Question and arm | First token, runs 1 / 2 | Eight decode steps, runs 1 / 2 |
| --- | ---: | ---: |
| j03 control before | 18.6680 / 18.6275 s | 3.5172 / 3.5157 s |
| j03 parallel pack | 18.5906 / 18.5409 s | 3.5633 / 3.5559 s |
| j03 control after | 18.6990 / 18.6651 s | 3.5342 / 3.5337 s |
| j10 control before | 14.1954 / 14.1486 s | 3.5520 / 3.5465 s |
| j10 parallel pack | 14.0604 / 14.0162 s | 3.5808 / 3.5757 s |
| j10 control after | 14.1294 / 14.0857 s | 3.5315 / 3.5303 s |

The candidate's first-token times were roughly 0.07–0.13 s below corresponding controls; the measured decode intervals varied in the other direction. This limited comparison does not establish a robust complete-answer or chat-latency improvement. Within each question, all runs had identical first-logit hashes and nine generated token IDs. Each worker reported 93 of 93 hybrid projections dispatched, no fallbacks and 93 of 93 eligible pairs reused; each candidate emitted its packer-selection trace. Those are aggregate counters and short-output parity, not broad answer-quality or gameplay validation.

A separate fault-only build forced each reused projection with more than 128 tokens to fail after its first DSP call. In two j03 runs, 62 injected failures were followed by successful recomputation through the ordinary hybrid path; 31 other pair attempts were reused, and all 155 ordinary hybrid attempts dispatched without fallback. First-logit hashes and all nine generated token IDs matched the normal candidate. This targets one queue-recovery path; it does not prove every dual-buffer failure mode.

The first attempted model comparison accidentally used a runtime linked against an unavailable `libc++_shared.so` and fell back to GPU-only. It was interrupted, excluded from the table, and the worker was rebuilt with its C++ runtime linked in. The valid trial verified dispatch after each worker. Across the valid comparison and fault trial, the guard observed at least 2,560,724 KiB available and at most 28.9 °C; Minecraft's status query passed after each restoration and CDSP returned offline with zero crashes. No installed service, model, firmware or world was changed. Private prompts, cache copies, binaries and raw logs were removed.
