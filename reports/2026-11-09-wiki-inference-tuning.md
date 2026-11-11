# Nord N20 Bonsai inference tuning, November 9, 2026

These trials used the installed Ternary Bonsai 2 4B group-64 Q2_0 checkpoint,
the Adreno 619, the C.24 vendor OpenCL driver, and llama.cpp b12110. Inference
ran with two threads restricted to CPU cores 0–2 at nice 15. The live
Minecraft server was online with zero players during the inference benchmarks.
The benchmarks used a separate OpenCL cache and a copy of the parsed Wiki
cache.

## Prompt batching

The 256-token synthetic prompt ran twice per setting. The first pass included
the first execution of each kernel in a new process. Each number is elapsed
prefill time from llama-bench.

| Microbatch limit | First pass | Second pass |
| --- | ---: | ---: |
| 64 | 48.22 s | 38.10 s |
| 128 | 46.91 s | 37.58 s |
| 256 | 44.94 s | 37.08 s |

## Projection kernels

The standalone harness used the packed layer 0 up and down weights and
deterministic FP16 activations. Times are median GPU event execution times
over five launches. The same activation buffer and Q2_0 layout were used
for each variant. Transfers, activation packing and OpenCL compilation are
outside these timings. The scalar checker sums dequantized weights and
the same activations in double precision.

| Kernel | Up, 32 tokens | Up, 128 tokens | Down, 32 tokens | Down, 128 tokens |
| --- | ---: | ---: | ---: | ---: |
| Pinned Adreno kernel | 19.02 ms | 75.00 ms | 19.51 ms | 75.42 ms |
| FP16 partial over 16 columns | 16.72 ms | 65.89 ms | 17.16 ms | 66.30 ms |
| FP16 partial over 32 columns | 16.03 ms | 62.65 ms | 16.33 ms | 62.99 ms |
| FP16 partial over 64 columns | 15.50 ms | 61.14 ms | 15.93 ms | 61.48 ms |
| FP32 partial over 32 columns | 19.43 ms | 75.80 ms | 19.48 ms | 76.06 ms |
| FP16 pair lookup in local memory | 41.90 ms | 164.35 ms | 42.03 ms | 164.94 ms |

The 64-column kernel sums each unscaled block in FP16, then applies the
block's scale and accumulates in FP32. At 128 tokens its normalized RMS
error was approximately 0.10% on both matrices over 2,337 sampled outputs.
At 32 tokens it ranged from 0.10% to 0.15%. The result preserves the GGUF
weight format and the existing activation transpose. The local-memory
lookup was slower on both shapes. The trial did not isolate the causes of
its higher latency.
[Pinned OpenCL backend](https://github.com/ggml-org/llama.cpp/blob/b12110/ggml/src/ggml-opencl/ggml-opencl.cpp)
[Qualcomm OpenCL optimization guide](https://docs.qualcomm.com/doc/80-NB295-11/80-NB295-11_REV_C_Qualcomm_Snapdragon_Mobile_Platform_Opencl_General_Programming_and_Optimization.pdf)

## Single-token projections

The decode kernel accumulates the unsigned two-bit codes against the
activation, subtracts the unweighted activation sum once per 64-column
block, and applies that block's scale. The tested fast configuration
computes eight rows per work item. The two feed-forward shapes use a
512-thread workgroup and 16 splits across the input columns. The vocabulary
projection uses a 256-thread workgroup and eight splits. The model ties
that projection to the token embedding weights.

| One-token projection | Pinned layout and kernel | Tuned layout and kernel |
| --- | ---: | ---: |
| Up, 9,216 × 2,560 | 1.569 ms | 1.195 ms |
| Down, 2,560 × 9,216 | 1.652 ms | 1.206 ms |
| Vocabulary, 248,320 × 2,560 | 46.990 ms | 41.947 ms |

These are medians of five GPU events per configuration. The largest
normalized RMS error for the three tuned shapes was 1.21 × 10⁻⁷ over
2,683 checked outputs. FP16 partial sums made these single-token
projections slower.

## Complete model

The standalone model harness calls the pinned model library directly. It
uses the real grounded Wiki instructions and retrieved evidence for a
266-token lightning-rod question. Each run clears context state, measures
through the first sampled token, then measures 16 more decode steps. It
uses the same 1,536-token context, 256-token microbatch and host greedy
sampler for each configuration.

The direct-copy experiment replaces a gather when the F32 source is a single
contiguous row and there is a single row index. Every valid index is zero
in that shape. It uses the same OpenCL queue and falls back to the standard
gather if a buffer copy cannot be enqueued. The path was exercised on the
recurrent state of the full model.

| Configuration | First pass to first token | Second pass to first token | Second-pass decode, 16 steps |
| --- | ---: | ---: | ---: |
| Installed backend | 47.73 s | 39.76 s | 8.55 s |
| FP16 partial over 64 columns | 38.66 s | 30.10 s | 8.60 s |
| Direct single-row copy | 48.00 s | 40.08 s | 8.05 s |
| FP16 prefill and direct copy | 39.02 s | 30.39 s | 8.08 s |
| FP16 prefill, direct copy and tuned decode | 39.02 s | 30.50 s | 7.38 s |

Each configuration above produced the same 17 token IDs in both passes.
Model and context initialization took 7.9–8.3 seconds and is outside the
table. The combined changes cut second-pass time to first token by 23%
and decode time by 14%. These are two passes per configuration over one
prompt.

## Wiki worker

The HTTP worker used the standard chat template, request formatting, and
blank-line stop. Each call retrieved evidence from the copied cache and
disabled prompt reuse for timing. The tuned worker in this comparison used
FP16 prefill and direct copy. All three answers and generated token counts
matched the installed backend.

| Question | Answer | Stock prefill / decode | Tuned prefill / decode |
| --- | --- | ---: | ---: |
| Lightning rod ingredients, 266 tokens | 3 [1] | 42.55 / 8.14 s | 31.91 / 7.54 s |
| Observer signal strength, 338 tokens | 15 [1] | 53.41 / 14.75 s | 40.00 / 13.63 s |
| Private server state, 228 tokens | Abstained | 35.91 / 10.84 s | 26.86 / 10.00 s |

The direct model harness also found 115 shared leading tokens between the
lightning-rod and observer prompts. It prefetched that exact prefix and
serialized the sequence state into a 56.5 MB in-memory snapshot. Building
the snapshot took 22.90 s in the new process, including first-use kernel
work. Restoring it and evaluating the rest of the lightning-rod prompt
took 17.18 s and 17.14 s through the first token, with matching token IDs.
The comparable warm run without the snapshot took 30.39 s. The existing
worker has its own live prompt reuse. This experiment measured a reusable
snapshot within one process.

## In-game trial

The complete tuned backend answered two `@wiki` messages through the real
chat log, offline Wiki retriever, Unix socket, and console FIFO. A protocol
client stayed stationary in a disposable Java 26.3 world at view distance
12 and simulation distance 10. The model was already loaded. Each
baseline used 22 seconds immediately before the corresponding request.
The second question reused the common prompt prefix through the worker's
normal slot cache. The production server was saved and temporarily stopped
for this trial.

| Window | Ticks | p50 | p95 | p99 | Max | Over 50 ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Lightning rod baseline | 440 | 22.7 ms | 31.7 ms | 40.5 ms | 55.5 ms | 1 |
| Lightning rod request | 795 | 13.7 ms | 30.7 ms | 37.0 ms | 48.9 ms | 0 |
| Observer baseline | 440 | 23.0 ms | 30.0 ms | 35.1 ms | 51.5 ms | 1 |
| Observer request | 817 | 20.4 ms | 31.4 ms | 34.8 ms | 71.2 ms | 2 |

The lightning-rod answer was `3` with its crafting link in 39.7 s. The
observer answer was `15` with its behavior link in 40.9 s. Both figures
include chat polling, retrieval, generation, and delivery. The p99 did
not rise in either window. These measurements cover one stationary
client and two requests. The comparison excludes an initial request
that failed before inference because of the test socket's permissions.
The tuned backend remains an opt-in experiment.
