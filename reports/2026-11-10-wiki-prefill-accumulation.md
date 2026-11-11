# Nord N20 prefill accumulation trial, November 10, 2026

Reducing the outer accumulator to FP16 improved measured whole-model warmed
prefill by 3.3�5.5%, with unchanged greedy output on four tested prompts.
It changes logits and increases numerical error; the limited quality suite
also reproduced existing factual and citation failures. Nothing was deployed.

The C.24 Adreno 619 driver ran new accumulation variants with real layer-0
Bonsai 2 4B Q2_0 up and down weights. The standalone harness used the
existing deterministic FP16 inputs and sampled double-precision checker.
All GPU work used `/tmp/n20-q2-gpu-benchmark.lock`, CPU cores 0–2, nice 15,
and a private cache. Timings below are medians of five GPU events and
exclude compilation, packing, upload and verification.

The baseline retains the tuned four-row/eight-token register tile and
64-column FP16 partial sums with FP32 outer accumulation. Unlike the earlier
local-memory and expanded-weight experiments, this trial changes only
accumulator lifetime or precision. Qualcomm describes both the higher
16-bit ALU throughput and the register-footprint limit on larger work-item
loads in its [Adreno optimization article](https://www.qualcomm.com/news/onq/2018/10/opencl-optimization-developers-accelerating-sobel-filter-adreno-gpu).
These motivate the experiments; no register allocation or spill counters
were measured on this device.

## Independent partial-sum chains

The two-chain kernel alternates even and odd input columns into independent
FP16 vectors and combines them at each 64-column boundary. Four chains use
one accumulator per column position in each packed byte. Both preserve
FP32 outer accumulation and the existing memory layout. On the 128-token up
projection the baseline took 61.265–61.958 ms, two chains took 74.841 ms,
and four chains took 129.841 ms. Sampled normalized RMS errors were
0.001007, 0.000748 and 0.000627 respectively. Additional independent
accumulators improved this error measure but slowed the projection.

The unchanged baseline also ran with 32, 64, 128 and 256 work-items per
group: 119.716, 62.539, 61.691 and 61.463 ms. Changing the Qualcomm
required subgroup qualifier from `full` to `half` gave 119.611, 61.511,
61.213 and 61.294 ms respectively. This did not produce a substantial
improvement; the driver compiler's realized subgroup size was not measured.

## FP16 outer accumulation

[The experimental kernel](../wiki/experiments/n20-q2-halfacc-prefill.cl)
also holds the outer accumulator and block scale in FP16. It converts the
final result to FP32 only when storing. It therefore changes rounding and
reduces numerical range, and is not a bit-identical optimization.

| Projection | Tokens | Baseline before/after ms | FP16 outer ms | Baseline NRMSE | FP16 outer NRMSE |
| --- | ---: | ---: | ---: | ---: | ---: |
| Up | 16 | 7.902 / 7.839 | 7.346 | 0.000985 | 0.001334 |
| Up | 128 | 61.591 / 61.373 | 57.332 | 0.001007 | 0.001347 |
| Down | 16 | 8.347 / 8.372 | 7.885 | 0.000972 | 0.002071 |
| Down | 128 | 61.586 / 61.573 | 57.781 | 0.001008 | 0.002340 |

All sampled results were finite. The improvement is approximately 6–7%
for these projection shapes, while the error increases, especially for
the longer down-projection reduction. Synthetic input checks alone do not
establish accuracy or range safety throughout the complete model.

## Complete-model check

An isolated llama.cpp b12110 checkout received the existing N20 OpenCL
patch. Only the Q2_0 prefill source differed between the two model runs;
both used tuned decode, the four-token tail, FWHT kernels, direct row copy,
weight packing and wide vocabulary projection. The installed service and
backend were not changed. The real installed 4B model ran a 225-token
rendered prompt containing lightning-rod evidence and asking the copper
ingot count, with a 128-token microbatch and 1,536-token context. Each
process cleared its context for two passes and generated 33 greedy tokens.
The fixed-step harness continues after end-of-message; answer correctness
was assessed before that delimiter, while token comparison includes all
33 generated tokens.

The first 128-microbatch comparison took 32.899/23.089 s to the first
token for the baseline's first/second passes and 31.818/21.919 s for the
FP16 outer accumulator. The 32 subsequent decode steps took
12.492/12.497 s and 12.414/12.414 s respectively. All 33 token IDs matched
on both passes. The answer was "You need three copper ingots to craft a
lightning rod [1]." The initial baseline populated the private shader
cache and paid 109 s of load/context setup; the candidate's setup took
6.1 s. Those setup costs are not comparable and are excluded from the
first-token figures. First passes also include lazy kernel compilation;
second passes are the useful steady-state comparison.

All 248,320 first-token logits were finite for both kernels. Relative to
the baseline, the candidate's RMS logit difference was 0.0098495, maximum
absolute difference 0.0498943, and normalized RMS difference 0.0029442.
The first-token top-ten membership was unchanged, with positions five
and six exchanged. Matching greedy output is therefore not equivalent
to matching logits or a proof of unchanged quality.

A second baseline process after the candidate gave 32.944/22.967 s to the
first token and identical token IDs. The candidate's warmed prefill gain
was thus 4.6–5.1% against its two bracketing baselines.

## Grounded checks at the normal 256-token microbatch

Three additional prompts used the production `GROUNDED` instruction and
actual unmodified passages from the phone's offline Wiki retriever. The
harness stopped at the model's end-of-generation token, with a 96-token
maximum. It ran each prompt twice from cleared state. Reported times are
the second pass, excluding model loading and first-pass lazy compilation.
Each candidate followed its baseline; this small suite was not randomized
or thermally controlled.

| Question | Input tokens | Baseline first-token s | FP16 outer first-token s | Baseline decode s | FP16 outer decode s |
| --- | ---: | ---: | ---: | ---: | ---: |
| Whether the campfire at my base is currently lit | 230 | 23.3538 | 22.5749 | 7.3497 | 7.3125 |
| Ender chest broken with a pickaxe without Silk Touch | 280 | 28.4865 | 27.0806 | 14.6476 | 14.6484 |
| Turtles bred with seagrass away from their home beach | 347 | 35.4800 | 33.5422 | 39.3834 | 39.3213 |

All complete generated token sequences and parsed replies matched between
kernels on both passes for all three questions. The first-token improvement
was 3.3%, 4.9% and 5.5% respectively; decode remained effectively unchanged.
These are prefill improvements, not equivalent improvements to the entire
reply, especially for the longer turtle answer.

Strict scoring required both the requested fact and citation support for
its decisive condition, or correct abstention on private state. Both
kernels scored **1/3**, with no difference between passes:

- The campfire reply was `Yes [1]`, relying on the fact that campfires are
  lit when placed. This cannot establish a particular server's current
  state and fails the required abstention.
- The ender-chest reply was `8 obsidian blocks [2]`, correctly supported by
  the passage about breaking the chest without Silk Touch.
- The turtle reply correctly described breeding and returning to the home
  beach to lay eggs. However, it cited passage [3], which describes breeding
  and pregnancy. Passages [2] and [4] provide the decisive home-beach rule,
  so the cited support was incomplete. This is a citation failure, not a
  claim that the underlying answer was factually false.

The private-state and citation failures existed with the tuned baseline.
Identical replies here provide no broad quality guarantee and do not make
those replies acceptable. This test directly exercised the model, without
retrieval shortcuts or other bridge safeguards.

Every first-token logit was finite for all pairs. Differences cover all
248,320 vocabulary entries, using the baseline as reference:

| Question | RMS difference | Maximum absolute difference | Normalized RMS difference |
| --- | ---: | ---: | ---: |
| Campfire private state | 0.008189 | 0.058675 | 0.003483 |
| Ender chest conditional | 0.010855 | 0.080474 | 0.004212 |
| Turtle multiple conditions | 0.010365 | 0.073529 | 0.004170 |

The accumulator can overflow or lose contributions for inputs outside the
checked cases. This modest speedup remains an experiment, with FP32 outer
accumulation retained in the normal tuned kernel.

For reproduction, use the existing projection harness with the candidate
source and `-DN20_PARTIAL=64`. The model comparison uses the existing N20
OpenCL patch with `N20_Q2_PREFILL_SOURCE` pointing at either the tuned
baseline (also with its default partial set to 64) or the candidate.
[The host-only model instrumentation patch](../wiki/experiments/n20-model-logits-trial.patch)
dumps first-token FP32 logits to `N20_LOGITS_OUT`, prints decoded text,
and stops at the model's end-of-generation token. Apply it to a scratch
copy of the harness with `git apply --ignore-space-change` when its source
has Windows line endings. Each repeat overwrites the logit dump, so the
reported comparisons use the final pass. The dump's time is part of the
decode interval, not the first-token interval. Both instrumented harnesses
built with the Android ARM64 toolchain at `-O3 -Wall -Wextra -Werror`.

Scratch model checkouts, binaries, raw tensor copies, prompt/excerpt files,
logit dumps, logs and private shader caches were removed after recording
the results. Only the experiment kernel, instrumentation patch and this
report were retained.
