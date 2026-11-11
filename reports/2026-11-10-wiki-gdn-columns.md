# Nord N20 Gated DeltaNet prefill layout, November 10, 2026

Using two state columns per lane group instead of four reduced two warmed
complete model requests by 1.1–1.9 seconds, approximately 3–4%, on the
Nord N20. Four further questions produced the same token sequences and
first-token logits and took 0.8–1.0 seconds less in one unbracketed pair
each. The [experimental backend patch](../wiki/experiments/llama-b12110-n20-gdn-columns.patch)
is opt-in and has not been deployed.

## What changed

The upstream Gated DeltaNet kernel keeps recurrent state in per-work-item
storage while advancing through prompt tokens. Its four-column prefill
configuration was tuned for a different Adreno generation. The candidate
retains the existing 128-wide state, eight lanes per column, Qualcomm
subgroup shuffle, arithmetic, input layout and single-token decode. It
compiles and dispatches the prefill kernel with two columns per lane
group, adjusting the workgroup count accordingly. The setting is frozen
when the OpenCL backend initializes, so kernel compilation and dispatch
cannot disagree if the process environment changes later. Other state
widths and the default setting retain upstream behavior.

Lower state-register pressure is a plausible explanation, but register
and spill counters were not measured. Qualcomm's [OpenCL optimization
guide](https://docs.qualcomm.com/doc/80-NB295-11/80-NB295-11_REV_C_Qualcomm_Snapdragon_Mobile_Platform_Opencl_General_Programming_and_Optimization.pdf)
describes how per-work-item workload and register footprint can affect
parallelism; it does not establish the cause of this device's result.

## Paired kernel tests

The C.24 Adreno 619 ran the upstream kernel with synthetic deterministic
F32 inputs in a standalone OpenCL harness. The representative shape used
128-wide state, 32 value heads and 16 query/key heads. Each process ran
five baseline, five candidate and five baseline GPU events; the table
shows their medians in milliseconds. GPU events exclude compilation,
host setup, buffer transfers and verification. The scratch process ran
on efficiency cores at nice 15 under the shared GPU benchmark lock.

| Prompt tokens | Four columns, before | Two columns | Four columns, after |
| ---: | ---: | ---: | ---: |
| 16 | 9.736 | 4.287 | 9.677 |
| 64 | 32.996 | 14.733 | 32.951 |
| 128 | 62.955 | 28.200 | 63.010 |
| 256 | 124.145 | 54.756 | 125.071 |

Every float in the output and final recurrent-state buffers was
byte-identical between the two layouts for these fixtures. Additional
tests matched the entire buffers with four state snapshots at 2, 16 and
128 tokens, and with vector-valued gating at 16 and 128 tokens. A
two-token/four-snapshot check also verified that unwritten older snapshot
slots remained zero. A sampled scalar CPU reference differed from the
baseline by up to 0.001369 absolute across the listed batches; that is a
limited numerical check, not a proof for all states. The retained
[harness](../wiki/experiments/n20-gdn-columns-bench.c) compares output and
snapshot bytes and checks representative values against that reference.

The driver advertised Qualcomm subgroup shuffle and the full worker trace
confirmed it used that path. A separate attempted local-memory reduction
was slower and produced different output for a one-token fixture; it was
rejected. It is not part of the column-layout patch.

## Complete model requests

The scratch Android ARM64 b12110 server used Bonsai 2 4B Q2_0, the C.24
OpenCL driver, the tuned Q2_0 and activation-texture paths, F16 KV, a
1,536-token context, a 256-token microbatch and two efficiency-core
threads at nice 15. A short warmup primed the in-memory system prefix.
Each measured request used the current grounded prompt and offline cached
Wiki passages, with 106 system tokens reused. It was sent directly to a
private local model endpoint; the clock covers the complete HTTP request
and response, excluding retrieval and Minecraft chat delivery. Both
questions were checked to defer from the bridge's exact-answer route.

| Question | Four columns before | Two columns | Four columns after |
| --- | ---: | ---: | ---: |
| Shulker box contents | 42.681 s | 41.008 s | 42.889 s |
| Current private brewing-stand contents | 30.782 s | 29.718 s | 31.010 s |

The candidate returned the same cited Shulker answer and private-state
abstention as both controls. Generated token IDs, normal stop reasons and
SHA-256 hashes of the full first-token vocabulary logits matched for each
question. Direct instrumentation confirmed four columns in the controls,
two in the candidate, and Qualcomm shuffle in both. Each table entry is
one request; the benchmark did not measure active-player tick latency.

An additional unbracketed baseline/candidate pair per question gave:

| Question | Four columns | Two columns |
| --- | ---: | ---: |
| Ender chest without Silk Touch | 30.124 s | 29.279 s |
| Piston pushing thirteen blocks | 40.931 s | 40.013 s |
| Current private respawn-anchor charge | 35.746 s | 34.783 s |
| Observer recipe and detection | 48.043 s | 47.101 s |

Those four pairs also matched generated token IDs, normal stops and
first-token logits. The multi-part observer response preserved an existing
citation limitation: the support text cited two passages, but the bridge
attached only the first to its final answer. These six examples are not a
general quality or numerical-equivalence guarantee.

After refactoring the experiment so the layout is frozen at initialization,
a separate Shulker pair took 42.748 s and 40.555 s with the same answer,
token IDs and first-token logits. An invalid `N20_GDN_CPL=2x` setting
retained the four-column default. Minimum sampled available memory across
the refined pair was 2,240,240 KiB and the maximum sampled battery
temperature was 26.6 °C.

## Reproduction and limits

Apply the patch to a scratch b12110 backend, build for Android ARM64 with
the C.24 OpenCL driver, and set `N20_GDN_CPL=2` for the candidate. Unset
it for the upstream four-column prefill; `N20_GDN_TRACE=1` emits a one-time
scratch trace of the selected column count and shuffle flag. The patch
also accepts `1` for experiments, but only `2` was tested on complete
model requests. The option is scoped to Adreno and 128-wide prefill.
The patch applied cleanly against stock b12110 and after each of the
existing N20 OpenCL, activation-texture and Flash Attention-cache patches.
The benchmark harness compiled with Android NDK r30 using
`-O3 -Wall -Wextra -Werror` and passed its output/state checks on the phone.

Use a separate shader cache and bind the option, backend and model to any
prefix-snapshot fingerprint. The launcher now includes a guarded opt-in
profile; the tested backend and its marker have not been deployed.
Long-running operation, active-player gameplay and other model geometries
remain untested; the Wiki service remains disabled on the live server. Phone
scratch was removed, workstation scratch was moved to the Recycle Bin, and
Minecraft's Java process remained running.
