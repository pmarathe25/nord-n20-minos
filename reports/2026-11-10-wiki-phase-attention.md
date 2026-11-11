# One-token attention dispatch experiment

2026-11-10. The opt-in [scratch patch](../wiki/experiments/llama-b12110-n20-phase-attn.patch) keeps Flash Attention enabled for the KV-cache layout and chooses the ordinary attention graph on eligible single-token Qwen3.5 calls. The latter graph materializes a transposed value cache; its cost is included in the measurements. A process-frozen environment flag controls graph construction. The experiment does not alter the installed runtime.

The eligibility test is shape based: it also applies to a one-token prefill chunk, so it is not a semantic prefill/decode switch. Both test arms disable indexed KV rows and use the same Flash Attention context, F16 cache, 128-token microbatch, GPU projection kernels, and GPU/Hexagon gate/up hybrid. Graph-construction messages show that each branch was selected; they are not per-dispatch execution counters. This trial does not establish numerical equivalence for all logits, cache positions, sequence shapes, or models.

| Prompt | Control before, 16 decode steps (s) | Candidate (s) | Control after (s) |
| --- | --- | --- | --- |
| j03 | 6.4324 / 6.4288 | 6.0359 / 6.0359 | 6.4515 / 6.4540 |
| j10 | 6.4364 / 6.4431 | 6.0697 / 6.0713 | 6.4393 / 6.4330 |

Each worker used two runs and independently rebuilt the fixed system prefix. The initial full-logit hash and all 17 generated token IDs matched across the three workers on each prompt. This does not check intermediate logits or prove answer equivalence. The candidate's 16-step decode was approximately 5.7–6.4% faster than the adjacent controls, including the V-cache conversion. First-token timings did not show a comparable gain. Both branch-selection traces appeared; hybrid projection counters reported no fallback. The phone's lowest available memory was 2,678,036 KiB and highest battery temperature was 29.4 C. Minecraft's status query passed after restoration; CDSP was OFFLINE with zero crashes.

## Complete streaming requests

Eight frozen wiki-retrieval fixtures were run first on the control server and then on the candidate server, with a separate prefix warm-up for each. These are complete streaming calls through the chat generation path; server startup and retrieval were excluded. They are a single ordered comparison, so thermal, ordering, and measurement noise are not controlled by repetition.

| Case | Control (s) | Candidate (s) | Existing checker |
| --- | ---: | ---: | --- |
| j04 | 34.699 | 34.535 | fail in both |
| j05 | 45.949 | 45.137 | fail in both |
| j07 | 21.277 | 20.851 | pass in both |
| j12 | 25.310 | 25.137 | fail in both |
| j14 | 24.572 | 24.165 | pass in both |
| j17 | 31.792 | 31.386 | pass in both |
| j22 | 25.764 | 25.430 | fail in both |
| j40 | 32.434 | 31.771 | pass in both |

All eight response texts matched exactly, and both arms had four passes and no runtime errors under the existing checker. The median paired difference was -0.406 s; median control and candidate times were 28.778 and 28.408 s. The checker is not a comprehensive quality evaluation, and identical responses on eight cases do not guarantee numerical equivalence. Hybrid counters showed 620 projections dispatched and zero fallbacks in each worker. Minimum available memory was 2,612,192 KiB and peak battery temperature was 30.6 C; after the test, Minecraft's status query passed and CDSP was OFFLINE with zero crashes.

This is a small experimental improvement, not evidence of an order-of-magnitude opportunity or a production-ready patch. Before deployment, test teacher-forced per-step logits and margins, one-token prefill chunks, cache boundary and longer-context cases, repeated requests, GPU operator execution/placement, and live gameplay. Repeat paired request comparisons in alternating order. The user-facing latency and existing incorrect answers remain material limitations.

## Reversed-order streaming repeat

A repeat used the identical eight-case fixture and experimental stack, but ran the candidate worker first and the baseline second. Both workers warmed their prefix before the measured requests. As above, the interval includes the complete streamed model request and excludes retrieval, startup and game-chat delivery. This is a second sequential worker comparison rather than a randomized per-request crossover, so remaining order and thermal effects are possible.

| Case | Candidate first (s) | Control second (s) |
| --- | ---: | ---: |
| j04 | 34.387 | 34.982 |
| j05 | 44.948 | 46.568 |
| j07 | 20.784 | 21.457 |
| j12 | 25.075 | 25.556 |
| j14 | 24.034 | 24.957 |
| j17 | 31.262 | 32.084 |
| j22 | 25.350 | 26.009 |
| j40 | 31.631 | 32.776 |

The candidate was faster in all eight pairs. Median paired savings were 0.747 s, or 2.85% when taking the median of the per-case relative savings; aggregate savings were 2.83%. Both arms produced identical response text for all cases, and both passed 4/8 checks with no runtime errors. Both workers dispatched 620 hybrid projections with no fallback. The guard observed at least 2,614,184 KiB available and a maximum battery temperature of 30.0 C; Minecraft's status query passed after restoration and CDSP was OFFLINE with zero crashes.

The reversed comparison strengthens the case for a gain above 1% over this particular experimental stack, but the two runs are too small to quantify general user-facing improvement precisely. The original run's median paired relative gain was about 1.48%. Neither is a production deployment or a gameplay trial. Before selecting this path, test teacher-forced intermediate logits and margins, singleton prefill chunks and longer cache positions, repeated reset/reuse, and actual GPU execution of the relevant operators; also validate the complete bundle under active gameplay.

## Teacher-forced logits and context boundaries

A scratch [logit harness](../wiki/experiments/n20-phase-logits-bench.c) generated a 17-token greedy continuation for each control, then fed the candidate exactly those same token IDs while saving all 248,320 logits for the initial prediction and each of 16 subsequent predictions. The [comparison](../wiki/experiments/n20-phase-logits-compare.py) checked every logit for finiteness and compared each step's maximum absolute difference, overall normalized RMS difference, and greedy top token. The experiments used the same experimental GPU/Hexagon stack, F16 KV, 128-token microbatches and disabled indexed KV rows. Each case used a fresh worker and cleared context; unlike the streaming comparison, these model-worker tests did not restore the system prefix.

j03 and j10 use real grounded Wiki prompts; j03 has 327 tokens. The 257-token test truncates the j03 token sequence; the 1,024-token test repeats tokens from that prompt to reach the target length. These synthetic inputs test execution boundaries, not answer quality. With a 128-token microbatch, the 257-token case ends prefill with one token, and therefore exercises the shape-based non-FA selection during prefill too.

| Input | First-step max absolute difference | Largest difference, all steps | Overall normalized RMS | Top-token agreement | Smallest control top-two margin |
| --- | ---: | ---: | ---: | ---: | ---: |
| j03 | 0 | 0.000190 | 3.78e-6 | 17/17 | 0.362 |
| j10 | 0 | 0.000280 | 6.57e-6 | 17/17 | 0.129 |
| 257 tokens | 0.0000381 | 0.000166 | 4.99e-6 | 17/17 | 0.729 |
| 1,024 tokens | 0 | 0.0000434 | 5.66e-7 | 17/17 | 4.136 |

Both graph-selection traces appeared for each candidate. All checked logits were finite and no tested top token changed. This is approximately 16.9 million logits per arm; it does not prove arbitrary prompt or model equivalence, actual GPU placement of every operator, repeated context-reset correctness, or gameplay behavior. The guard recorded at least 2,722,764 KiB available and at most 29.9 C battery; after the trial Minecraft passed its status query and CDSP was OFFLINE with zero crashes. Raw logit vectors and copied prompts were removed from the phone.

## GPU placement and repeated resets

A separate instrumented scratch build applied the experimental attention patch and a [temporary trace patch](../wiki/experiments/llama-b12110-n20-phase-attn-trace.patch) to the same backend. It named the eligible graph nodes and logged when each entered the OpenCL backend dispatch function. For each of the two grounded Wiki prompts, the candidate logged all five expected operations: Flash Attention for multi-token work, query–key multiplication, softmax, contiguous value-cache conversion, and the final value multiplication for one-token work. A dispatch trace is evidence that these named nodes reached the GPU backend; it is not a hardware utilization measurement or a count proving that every layer and every call used the GPU.

In a separate control worker and a candidate worker per question, the harness built the fixed 106-token prefix once, then cleared the context and restored that prefix before each of three requests. For each question, all three runs in both arms had identical initial full-logit hashes and identical 17-token greedy sequences. This checks the two prompts and the tested reset/restore cycle, not arbitrary session histories or intermediate logit equality. No trace build was installed. The resource guard recorded at least 2,674,652 KiB available and at most 29.8 C battery; Minecraft's status query passed after restoration and CDSP was OFFLINE with zero crashes.

## Disposable gameplay smoke attempt

A later disposable-world run used the experimental GPU–Hexagon stack and the baseline attention path, with the candidate planned as a second arm. A loopback-only creative client joined a prewarmed world with view distance 24 and simulation distance 10. Two actual `@wiki` player-chat questions received correct cited Observer replies: the stationary strength question in 31.268 s and the movement duration question in 29.046 s. The stationary request's matched no-query/query p99 tick times were 30.13/30.02 ms with no ticks above 50 ms in either window. These are individual windows, not evidence of no gameplay impact.

The run stopped before the candidate arm: an old assertion expected an activation-texture dispatch message that the newer backend's default logging did not emit. Therefore no gameplay A/B result was obtained. At worker shutdown, hybrid counters recorded 155 attempted and dispatched projections with zero fallbacks; this does not validate the candidate attention path in gameplay. Minimum available memory during measured baseline gameplay was approximately 1,163 MiB, and peak model and disposable-game RSS were approximately 2,107 and 1,468 MiB. A prior attempt had also stopped at startup due to an obsolete GPU-identification log-string assertion; actual mapped backend and driver and the Adreno619v2 sysfs identifier were checked before the second attempt.

After both attempts, the production server returned ready with zero players, and its whitelist digest, non-comment settings, world directory identity, level marker, and disabled Wiki marker passed the existing restoration checks. CDSP was OFFLINE with zero crashes. The comparison was not repeated after the user requested an end to further optimization.

## Post-commit candidate chat smoke test

After correcting the scratch harness to check the newer backend's actual dispatch marker, a separate run tested the candidate attention path end to end in a disposable loopback world. The `@wiki` strength question returned the cited answer `15` in 30.951 s while stationary; the pulse-duration question returned cited `2` in 28.726 s during movement. The candidate emitted both attention-selection traces, and the hybrid reported 155 projections dispatched with zero fallbacks. This validates those two chat paths in the scratch bundle; it does not mean the installed, disabled Wiki service has been deployed or validated as a matching bundle.

For the stationary request, matched no-query/query p99 tick times were 31.29/38.60 ms, with 2/0 ticks above 50 ms. For the movement request, p99 was 48.63/41.78 ms, with 4/3 ticks above 50 ms. The two question windows are single samples and do not establish gameplay safety or general answer quality. The memory guard of 1,000 MiB did not trip; the minimum observed in the movement query was 1,013.9 MiB, leaving little headroom in that scenario. After the run, the production server returned ready with zero players; whitelist digest, settings, world identity and disabled Wiki marker passed the existing checks. CDSP state and crash count were checked after shutdown.
