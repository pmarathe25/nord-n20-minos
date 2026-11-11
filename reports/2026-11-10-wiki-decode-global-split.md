# Global split reduction for Q2 decode

2026-11-10. This scratch GPU experiment separates the feed-forward Q2 matrix-vector projection from its split reduction. The first [kernel](../wiki/experiments/n20-q2-decode-global-split.cl) writes FP32 partials without the existing 512-work-item local-memory barrier; a second kernel sums them in the same split order. The [incremental backend patch](../wiki/experiments/llama-b12110-n20-decode-global-split.patch) is opt-in, allocates an in-order-queue scratch buffer and limits dispatch to the two tested one-token FFN shapes. Other shapes retain the ordinary kernel. The patch is not installed.

## Projection screen

The phone used real layer-0 Bonsai weights and deterministic FP16-derived activations. The table shows medians of five samples, including both candidate dispatches; host times run from enqueue through completion. Baseline measurements bracketed each candidate. These timings exclude model loading, buffer setup and compilation. The candidate used 64-work-item groups and 16 splits; the baseline used its tuned 512-work-item group and 16 splits.

| Shape | Baseline before GPU / host | Candidate GPU / host | Baseline after GPU / host |
| --- | ---: | ---: | ---: |
| Up 9,216 × 2,560 | 1.193 / 1.408 ms | 1.105 / 1.361 ms | 1.195 / 1.412 ms |
| Down 2,560 × 9,216 | 1.196 / 1.451 ms | 1.084 / 1.349 ms | 1.198 / 1.412 ms |

The GPU-event span was about 7–10% shorter. Workgroups of 32 and 96 were slower, and 128 was slower on down; these observations are device- and shape-specific. Complete output hashes matched the baseline for three deterministic input patterns on each shape. All 9,216 up and 2,560 down outputs were checked against a double-precision scalar reference; normalized RMS errors were below 1.1e-7. Separate 64-byte input/output offset tests preserved output canaries and matched full-output hashes for all three patterns. These synthetic tests do not establish bitwise parity for every possible activation or captured model input.

## Full-model and streaming checks

A warm control/candidate/control comparison used the gate/up GPU–Hexagon hybrid with its parallel activation packer, the same checkpoint, two Wiki questions and locally reconstructed no-thinking prompt framing. Each worker independently prepared a 106-token in-memory prefix and ran two repeats. For j03, 16 decode steps took 6.7423/6.7338 s before, 6.5823/6.5775 s with the candidate and 6.7443/6.7377 s after. For j10 the corresponding times were 6.7738/6.7710, 6.5966/6.6021 and 6.7365/6.7302 s. All first-logit hashes and 17-token sequences matched for each question. The candidate emitted a one-time dispatch trace; the backend teardown counter was not observed, so this does not establish a complete dispatch count. Model loading, prefix construction, retrieval and game chat were excluded.

The candidate was then tested through the actual server's Jinja template and the checkout Wiki bridge's streaming request/reply functions on the same eight frozen questions and passages as the [down-projection comparison](2026-11-10-wiki-hybrid-down-streaming.md). Both arms retained the gate/up hybrid; down offload was disabled. The first candidate attempt was interrupted after server logging filtered its dispatch diagnostic; the baseline results were retained, the scratch diagnostic was changed to unfiltered stderr, and a fresh candidate worker was rerun after the server and Minecraft were restored. Both workers warmed with the same separate question. There was no trailing control, and the interruption and single-order measurement limit the timing conclusion.

| Case | Baseline | Global split | Grounded check, baseline / candidate |
| --- | ---: | ---: | --- |
| j04 | 35.364 s | 35.043 s | fail / fail |
| j05 | 47.380 s | 46.980 s | fail / fail |
| j07 | 21.888 s | 21.632 s | pass / pass |
| j12 | 25.916 s | 25.622 s | fail / fail |
| j14 | 25.154 s | 24.972 s | pass / pass |
| j17 | 32.652 s | 32.270 s | pass / pass |
| j22 | 26.429 s | 26.154 s | fail / fail |
| j40 | 33.422 s | 33.071 s | pass / pass |

The median paired streaming-request difference was **−0.307 s**, with 29.541 s baseline and 29.212 s candidate medians. Server-reported median paired prefill and decode differences were −112 ms and −156 ms; the kernel targets decode, so the prefill difference illustrates timing variation rather than an attributed gain. All eight raw and rendered replies matched, both scored 4/8, and the private-state case explicitly abstained in both. Both arms bypassed the exact-answer route on two cases where it was available. These measures exclude retrieval, startup, actual Minecraft chat delivery and gameplay; they do not establish broad answer-quality equivalence or a robust player-visible improvement.

The candidate server emitted the unfiltered dispatch trace and reported 620 of 620 gate/up pairs reused, without hybrid fallback. A fault-only backend forced 1,539 failures after the first global-split enqueue; ordinary GPU fallback produced the same first-logit hash and 17 generated tokens as the control. This exercises one queue-ordered recovery path, not every OpenCL error or concurrent-use scenario.

The ARM64 Android backend and harness compiled with warnings as errors, and the patch reverse-applied cleanly against the tested source. During guarded model, streaming and fault trials the minimum sampled available memory was 2,063,588 KiB and maximum sampled battery temperature was 30.2 °C. Minecraft passed its status query after each restoration; CDSP returned offline with zero recorded crashes. Scratch prompts, weights, binaries, vendor-image copies and raw logs were removed. No installed model, service, firmware, partition or world was changed.
