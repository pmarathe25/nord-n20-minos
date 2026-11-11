# Bonsai 2B as a draft model for Bonsai 4B, November 10, 2026

Using the 2B checkpoint as an autoregressive speculative drafter made the
tested phone request about **2.1 times slower**, even though it preserved
the answer. Both models ran on Adreno. The result does not justify this
configuration for the Wiki worker. It is distinct from the earlier
[prompt-lookup and trained-head tests](2026-11-09-bonsai-speed.md#speculative-decoding).

## Desktop screen

The b12110 server ran Bonsai 2 4B Q2_0 on Vulkan0. The draft arm also
loaded the Bonsai 2 2B Q2_0 checkpoint on the GPU, with `draft-simple`
and up to four proposed tokens. Both configurations used one slot, a
2,048-token context, six CPU threads, temperature zero, a 140-token cap,
the current grounded prompt and no blank-line stop or prompt reuse.
The Rust retriever used a private offline copy of the phone's Wiki cache.
Twelve questions covered factual, private-state and multi-part cases.
Each process received a warmup, and single-model runs bracketed the draft
run. Each row reports the median of 12 complete local HTTP requests;
retrieval and server startup are excluded.

| Mode | Complete request | Prompt evaluation | Decode | Accepted / proposed |
| --- | ---: | ---: | ---: | ---: |
| 4B before | 297 ms | 101 ms | 181 ms | — |
| 4B + 2B draft | 695 ms | 169 ms | 532 ms | 252 / 384 |
| 4B after | 281 ms | 102 ms | 178 ms | — |

All twelve returned texts and finish reasons matched across modes. The
matching output is limited to these deterministic examples; token IDs
were not separately recorded. Desktop timing does not predict Adreno
performance, and the sample is not a broad quality evaluation.

## Nord N20 comparison

The phone ran a scratch Android ARM64 b12110 server and the C.24 Adreno
OpenCL driver with the tuned Q2_0, activation-texture and lazy Flash
Attention-cache patches. Direct instrumentation confirmed texture
dispatch. Both models were GPU-offloaded in the draft arm. The target
used F16 KV, one 1,536-token slot, a 256-token microbatch and two
efficiency-core threads at nice 15. The same model options and kernel
sources were used in all three arms, with the 2B draft and four-token
proposal limit added only for the middle arm. The draft GGUF's SHA-256
matched the previously recorded local checkpoint identity.

Each process received one warmup followed by one measured request. All
requests used the same four cached passages and asked what redstone
signal strength an observer outputs. The grounded prompt had no blank-line
stop, and prompt reuse was disabled. The clock runs from submitting the
HTTP request through the complete reply, excluding retrieval, startup and
game chat delivery. The measured and warmup requests each generated 35
tokens and returned the same cited answer, `15 [1]`.

| Mode | Complete measured reply | Prompt evaluation | Decode | Accepted / proposed |
| --- | ---: | ---: | ---: | ---: |
| 4B before | 47.626 s | 33.713 s | 13.897 s | — |
| 4B + 2B draft | 97.651 s | 49.126 s | 48.507 s | 15 / 44 |
| 4B after | 47.371 s | 33.510 s | 13.843 s | — |

The draft warmup took 97.703 s with the same 15/44 acceptance; the two
baseline warmups took 47.602 and 47.356 s. In this case the draft arm's
prompt evaluation alone exceeded the entire baseline reply time. That is
not a general lower bound for other questions, draft lengths, cache reuse,
or a different implementation; it is evidence against this tested path.

The trial held the shared GPU lock while Minecraft remained running.
A separate guard sampled system available memory and battery temperature
every two seconds and would stop the scratch worker below 1,200 MiB or
at 40 °C. The sampled minimum available memory was 1,288,368 KiB for the
draft arm versus 2,225,008 and 2,226,516 KiB for the baseline arms. The
draft arm's maximum sampled battery temperature was 25.7 °C. The guard
did not trigger. These are system-available-memory samples, not direct
measurements of the draft model's allocated memory.

The [pinned speculative decoding documentation](https://github.com/ggml-org/llama.cpp/blob/b12110/docs/speculative.md)
explains the target-verification approach. This trial neither used Hexagon
nor tested parallel CPU/GPU drafting, and it does not rule out a smaller
or specifically trained drafter. No live model, service, firmware or
configuration was modified. Scratch workers stopped, the phone scratch
directory was removed, and workstation scratch was moved to the Recycle
Bin. Minecraft's Java process remained running.
