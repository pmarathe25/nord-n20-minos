# Shared-buffer GPU and Hexagon projection experiment

2026-11-10. This is an experimental projection benchmark, not an installed model backend or an end-to-end chat measurement.

The benchmark splits the Bonsai layer 0 up projection (9,216 output rows by 2,560 input elements) between the GPU and Hexagon. It uses 128 synthetic FP16 tokens in [-1, 1], the real layer weights, and a shared ION buffer imported into OpenCL. The timed mixed path includes GPU Q8 input packing and its finite/range guard, synchronization, GPU and FastRPC dispatch, DSP computation, and GPU conversion and scatter into a single output. Weight initialization and packing, model loading, and retrieval are outside the timed interval. Each reported measurement is bracketed by GPU-only measurements with five samples per arm.

| Tokens | DSP rows | GPU before | Mixed | GPU after |
| --- | ---: | ---: | ---: | ---: |
| 128 | 3,072 | 55.811 ms | 43.562 ms | 55.796 ms |
| 128 | 3,328 | 55.788 ms | 45.608 ms | 55.790 ms |
| 32 | 3,072 | 14.465 ms | 11.975 ms | 14.509 ms |
| 16 | 3,072 | 7.357 ms | 7.379 ms | 7.374 ms |

For 128 tokens and 3,072 DSP rows, the mixed projection is about 22% faster than its GPU controls. GPU packing took 4.891 ms and guard readiness 5.210 ms; the overlapping GPU shard took 37.171 ms, the DSP kernel 35.924 ms, and final synchronization and scatter 1.862 ms. These component timings overlap and should not be added together. An earlier ordering queued the GPU shard before waiting for the guard and took 80.735 ms; moving the guard wait before that enqueue restored overlap. The observed delay is consistent with the driver waiting for later queued work, but the internal driver behavior was not established.

The full GPU shard matched the GPU baseline bit for bit. GPU-packed input bytes matched the CPU reference, shared-buffer visibility and scatter checks passed, and sampled DSP values matched the scalar integer reference. Against a sampled FP64 reference using the original FP16 inputs, normalized RMS error was 0.002302828 for the mixed result and 0.000977236 for the GPU result. This is an approximate fixed-point DSP path; scalar integer agreement does not establish model answer quality. Finite boundary inputs passed the checks. Deliberately out-of-range and NaN inputs took the GPU fallback and matched the baseline finite values and NaN masks; each fallback case was exercised six times including warmup.

The guard assumes finite activations in [-1, 1]; the real-model check below shows that assumption often fails. No full-model accuracy, prefill, decode, game tick, memory-residency, or chat latency benefit follows from this synthetic single-projection result. The experimental harness is `wiki/experiments/n20-q2-overlap-shared-bench.c`; it is not enabled in the installed service.

## Real activation range check

A later scratch-only run used the existing Bonsai 4B model, the activation-texture backend and three held-out Java Wiki questions with their cached evidence and the production system and user text. The chat framing was approximated, not rendered with the model's actual template. Prompt lengths were 281, 271, and 136 tokens, with microbatches of at most 256. A blocking OpenCL interposer scanned the actual FP16 texture inputs; the scan makes this run unsuitable for timing. Its known-input check reported the expected counts for ordinary and boundary inputs, one out-of-range value, and one NaN, with no scan errors.

| Projection shape | Observed calls | Calls containing values outside ±1 or nonfinite | Observed token instances | Token instances containing values outside ±1 or nonfinite |
| --- | ---: | ---: | ---: | ---: |
| 9,216 × 2,560 (gate/up) | 248 | 248 | 41,726 | 41,722 |
| 2,560 × 9,216 (down) | 124 | 65 | 20,863 | 6,019 |

No observed values were nonfinite. The gate/up inputs contained 204 values outside ±8 across the three prompts; the down inputs contained none. The 9,216-row total includes both gate and up calls, and counts reflect repeated model layers and microbatches, not independent prompts. This instrumentation sees only the two FFN texture shapes and microbatches of at least 16 tokens: it excludes decode, small tails, other shapes and fallback paths. The 15-token tail of the 271-token prompt, for example, is excluded. It does not establish a maximum activation range for future inputs.

Because the current guard rejects a whole projection call when any input falls outside ±1, it would reject every observed gate/up call and 65 of 124 observed down calls. The synthetic speedup therefore does not justify deploying this fixed-scale path. Per-token dynamic scaling is a candidate for further work, but its GPU reduction and packing cost, DSP output scaling, numerical error, answer quality, and full-model latency all need measurement.

The Minecraft server was stopped only after an idle check and restored afterwards. At the post-run check Java was running, the stop marker was absent, and the CDSP was offline with zero recorded crashes.
