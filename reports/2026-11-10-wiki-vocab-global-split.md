# Global split reduction for the vocabulary projection

2026-11-10. This scratch experiment applies the existing [global-split decode kernel](../wiki/experiments/n20-q2-decode-global-split.cl) to the tied 248,320-by-2,560 vocabulary projection. The [incremental backend patch](../wiki/experiments/llama-b12110-n20-vocab-global-split.patch) compiles a separate 16-row, four-split program, allocates a 3,973,120-byte partial buffer, and leaves the tied embedding lookup unchanged. It runs only for the tested single-token vocabulary geometry and uses the ordinary GPU path on setup or enqueue failure. It is opt-in and not installed.

## Projection screen

The phone used the actual tied vocabulary weights and deterministic FP16-derived inputs. The baseline used the existing 16-row, four-split, 512-work-item kernel; the candidate used a 256-work-item first kernel and a second global reduction in the same split order. Each value is the median of five samples, including both candidate dispatches and scratch-buffer traffic; host time covers enqueue through completion. Initialization and weight loading are excluded.

| Arm | GPU-event span | Host enqueue through completion |
| --- | ---: | ---: |
| Baseline before | 36.484 ms | 36.853 ms |
| Candidate | 26.734 ms | 27.207 ms |
| Baseline after | 36.584 ms | 36.951 ms |

Across three input patterns, every output byte matched the baseline, including tests with 64-byte input/output offsets and output canaries. One input also checked all 248,320 outputs against a double-precision scalar reference, with normalized RMS error 7.74e-8; the other patterns checked 2,560 sampled outputs each. Workgroups of 64 and 128 were slower than 256 for this candidate. These deterministic checks do not prove parity for all activations or configurations.

## Model and streaming results

The full-model control/candidate/control test retained the previously tested global-split FFN kernel and GPU–Hexagon gate/up hybrid in both arms; only the vocabulary option differed. Each worker used a warm shader cache, 128-token microbatches, an independently built in-memory system prefix and two runs per question. First-token logits and all 17 generated token IDs matched for each question.

| Question | Control before, 16 decode steps | Candidate | Control after |
| --- | ---: | ---: | ---: |
| j03 | 6.6258 / 6.6211 s | 6.4565 / 6.4536 s | 6.5924 / 6.5997 s |
| j10 | 6.5883 / 6.5894 s | 6.4279 / 6.4289 s | 6.5807 / 6.5789 s |

The candidate saved about 0.14–0.17 s over 16 decode steps in this small bracketed screen. Both optimized FFN and vocabulary dispatch traces were present; teardown did not supply complete per-kernel counts. This is not a complete-answer or gameplay test.

An eight-question streaming comparison then used the server's actual Jinja template and checkout Wiki request/reply code, with the same frozen offline evidence and one measurement per arm in baseline-then-candidate order. Both workers kept the FFN optimization; only the candidate enabled the new vocabulary path. Both passed 4/8 grounded checks, produced identical raw and rendered replies, completed all requests and explicitly abstained on the private-state case. Two questions with an exact-answer route were deliberately forced through the model. Median request times were 29.033 s baseline and 29.040 s candidate; the median paired candidate-minus-baseline difference was **−0.019 s**. Server-reported median paired decode time fell 108 ms while prefill rose 113 ms, illustrating request-level variation. This single-order sample does not establish a robust end-to-end gain or broad quality parity; retrieval, startup, Minecraft chat delivery and gameplay were excluded.

The candidate worker emitted both optimized-kernel traces and reported 620 of 620 gate/up pairs reused, with no hybrid fallbacks. A fault-only backend forced 17 vocabulary failures after the first kernel had been enqueued; the ordinary vocabulary fallback matched the control's first-logit hash and 17 generated tokens. This checks one queue-ordering failure mode, not every OpenCL error.

The Android backend and harness compiled with warnings as errors, and the incremental patch reverse-applied cleanly against the tested source. The full-reference GPU screen observed at least 3,823,448 KiB available memory and at most 27.6 °C battery temperature. Across guarded model, streaming and fallback trials, the minimum sampled available memory was 2,604,952 KiB and the maximum battery temperature was 30.1 °C. Minecraft passed its status check after each restoration; CDSP returned offline with zero recorded crashes. Scratch weights, prompts, binaries, vendor-image copies and raw logs were removed; no installed model, service, firmware, partition or world was changed.
