# Nord N20 Wiki runtime follow-up, November 9, 2026

The trial used the C.24 Adreno driver, Android ARM64 llama.cpp b12110,
and the Bonsai 2 4B Q2_0 checkpoint. The inference processes used separate
Unix sockets and shader caches. Minecraft was online for the Adreno
and chat-bridge measurements.

## Kernel fusion

The four-token prefill tile holds four activation lanes in registers.
It replaces the eight-token tile only when llama.cpp schedules exactly
four tokens. The benchmark used packed layer-0 weights from Bonsai,
synthetic input, and GPU event timing.

| Q2_0 projection, four tokens | Eight-lane tile | Four-lane tile |
| --- | ---: | ---: |
| Up, 9,216 × 2,560 | 4.034 ms | 2.556 ms |
| Down, 2,560 × 9,216 | 4.705 ms | 3.027 ms |

Normalized RMS error against the scalar reference was about 0.09–0.12%.
The FP16 partial accumulation was enabled in both configurations.

The register Hadamard kernel fuses two butterfly stages on each side
of the local-memory stages. It matched the scalar F32 reference exactly
for 4,096 synthetic rows at each width. The table shows median GPU
event times from seven runs.

| Transform width | Upstream | Register butterflies |
| --- | ---: | ---: |
| 64 | 4.041 ms | 3.786 ms |
| 128 | 12.265 ms | 13.869 ms |
| 256 | 46.467 ms | 43.029 ms |
| 512 | 39.880 ms | 44.437 ms |
| 1,024 | 38.737 ms | 36.210 ms |

The optional backend selects the new transform at the three winning
widths. The chat trace confirmed the four-token projection but did not
record a dispatch of the register transform. With both switches enabled,
and without a retained prefix, the three chat prefill measurements
improved by 1.8–2.6%.

On this driver, a workgroup can synchronize its local tile. A portable
whole-model OpenCL kernel would need device-wide synchronization at
multiple graph boundaries, and OpenCL gives no workgroup scheduling
guarantee that would make a software spin barrier safe. The driver
advertised Qualcomm subgroup shuffle and ION-backed host pointers but
no OpenCL command-buffer extension. Vulkan command buffers can submit
ordered dispatches with explicit memory dependencies, a distinct
experiment from fusing the entire graph into one dispatch.
[OpenCL programming model](https://registry.khronos.org/OpenCL/specs/unified/html/OpenCL_API.html)
[OpenCL command-buffer extension](https://registry.khronos.org/OpenCL/specs/unified/refpages/man/html/cl_khr_command_buffer.html)

## System-prefix cache and chat latency

The opt-in server patch retained the full state for the exact 106-token
system prefix before the first user turn. It occupied 56,167,716 bytes.
It restored the prefix after idle unload for a different Wiki question
and for a repeat of the first question. Each wake test had a five-second
idle threshold and waited nine seconds before sending the next request.

| Request to first eight output tokens | Previous tuning | New kernels | New kernels and retained prefix |
| --- | ---: | ---: | ---: |
| First observer request after health check | 23.92 s | 23.28 s | 22.88 s |
| Torch request after idle unload | 48.55 s | 46.83 s | 31.80 s |
| Observer request after idle unload | 31.78 s | 30.89 s | 16.35 s |

Every configuration emitted the same first eight tokens for each
request. The cache avoids re-evaluating the system instruction after
sleep. The model still needs to load its weights, which took about five
seconds in the restored runs. These are individual timings with output
capped at eight tokens. They do not measure complete answer latency.

Separately, the chat worker now wakes for Linux file notifications and
for worker completion. It retains the existing timer fallback and
console safeguards. Twelve simulated log-to-console requests per
configuration exercised the real offline Wiki cache, an absent model
socket, three exact-answer question forms, and one log rotation. Median
latency fell from 372 ms with polling to 31 ms with event notifications.
This test used a private log and FIFO and did not measure server ticks.

## Alternative models

All desktop tests used the same retrieved passages from the 44-question
Java Edition set. The factual scorer required the expected answer and
a citation to an available excerpt. The private questions tested
whether the model declined to infer facts about the player's world.
Bonsai's score is the earlier run on the same set.

| Model and output format | Factual | Correct private abstentions |
| --- | ---: | ---: |
| Bonsai 2 4B Q2_0, support then answer | 31/36 | 8/8 |
| LFM2.5 1.2B QAD Q4_0, support then answer | 0/36 | 5/8 |
| LFM2.5 1.2B QAD Q4_0, JSON schema | 0/36 | 5/8 |
| Qwen3.5 0.8B Q4_0, support then answer | 6/36 | 8/8 |
| Qwen3.5 0.8B Q4_0, JSON schema, thinking disabled | 7/36 | 7/8 |
| LFM2 350M Extract Q4_0, cited verbatim span | 2/36 | 3/8 |
| LFM2 350M Extract Q4_0, schema also in the instruction | 3/36 | 5/8 |
| MiniLM SQuAD2, span with null margin ≥ 0 | 19/36 | 1/8 |
| MiniLM SQuAD2, span with null margin ≥ 5 | 13/36 | 4/8 |

The two generators used temperature zero and the same answer format
in each matched run. Some LFM replies contained the right value
without a usable citation. The MiniLM CPU probe chose the highest
scoring short extractive span and attached the passage number. Its
median runtime was 63 ms on desktop. The NumPy probe used the published
weights and approximate GELU. MiniLM frequently extracted plausible
text on private-state questions. Its null margin was tested at fixed
thresholds on this set and was not calibrated on a separate set.
[LFM2.5 model](https://huggingface.co/LiquidAI/LFM2.5-1.2B-Instruct-GGUF)
[Qwen3.5 checkpoint](https://huggingface.co/ggml-org/Qwen3.5-0.8B-GGUF)
[LFM Extract model](https://huggingface.co/LiquidAI/LFM2-350M-Extract)
[MiniLM SQuAD2](https://huggingface.co/deepset/minilm-uncased-squad2)

The Qwen checkpoint aborted on the phone when the Q4_0 Adreno prefill
path attempted to use a kernel handle that was unavailable on this
driver. The successful desktop run used Vulkan. On a subset of eight
phone questions with grammar-constrained output, LFM2.5 1.2B took a
median 7.78 s and answered 0/6 factual questions. LFM2 350M Extract took
3.81 s and answered 1/6. They correctly declined 2/2 and 1/2 private
questions, respectively. MiniLM ran on desktop only.

## A coarser GPU–Hexagon partition

Bonsai's 248,320-row vocabulary projection is tied to its token
embedding. Output rows are independent once the final activation is
ready. Hexagon can keep 16,384 packed rows resident, consume one
quantized activation, and return the local winning row and score.
The 8-byte winner replaces 65,536 bytes of shard logits for greedy
decoding. Timing metadata is separate. The benchmark verified the
winner against the full fixed-point shard output on every call. The
fixed-point output differed from the floating-point reference by
0.42% normalized RMS on the one-token test.

| One-token vocabulary projection | Median |
| --- | ---: |
| Full head on Adreno, 248,320 rows | 42.55 ms |
| Adreno remainder, 231,936 rows | 40.63 ms |
| Same 16,384-row shard on Adreno | 2.92 ms |
| Hexagon shard, full output round trip | 8.44 ms |
| Hexagon shard, top-1 round trip | 8.18 ms |

The Adreno column uses GPU events with a 256-thread group and eight
input splits. Hexagon includes FastRPC but excludes host activation
quantization. Both used real vocabulary weights and synthetic
activations, with different deterministic generators. The runs
measured the two devices separately. With perfect overlap, the
measured remainder leaves about 1.9 ms of potential savings per
decode step before synchronization, quantization, or shared-memory
contention. That is under 1% of the measured Bonsai decode step.
The shard is a poor trade at this size. The top-1 interface applies
to greedy decoding and would need a broader reduction for samplers
that consume multiple logits.

The earlier gate/up split crossed the device boundary in each layer
and slowed full prefill. Keeping a complete attention or recurrent
block on one processor would reduce those crossings, but would need
separate measurements for its compute and shared-memory costs.
The vocabulary trial used the stock C.24 FastRPC stack and existing
firmware. Minecraft was stopped through its idle maintenance guard
for the DSP run and the CDSP returned to `OFFLINE` with zero crashes.
