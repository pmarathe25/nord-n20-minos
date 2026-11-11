# Nord N20 Wiki chat end-to-end trial, November 9, 2026

The phone answered a real Minecraft Java 26.3 chat question with a private
reply and a clickable Minecraft Wiki section. The Rust retriever read
cached Java Edition excerpts, and the Adreno ran Ternary Bonsai 2 4B
`Q2_0` through llama.cpp b12110 and the C.24 OpenCL driver.

## Phone measurements

| Path | Result |
| --- | --- |
| Rust/SQLite retrieval, cached pages | 25–40 ms in sampled one-shot requests |
| 2B CPU, short prompt, actual chat | 19.0 s, correct ingredient count and citation |
| 2B CPU, full grounded prompt | Exceeded the original 45 s bridge timeout |
| 4B OpenCL, cold prompt, model already loaded | 49.6 s with the stop rule, including 41.4 s prefill and 8.3 s decode |
| 4B OpenCL, first chat request after worker slept | 63.8 s, including approximately 13 s to reload the model |
| Same chat question with identical evidence | 0.28 s from the reply cache, including retrieval and chat polling |

The chat trial ran a disposable loopback-only server with view distance 12,
simulation distance 10, and one stationary protocol client. The baseline
began after the initial chunk stream settled. The cold request went through
the vanilla chat log, bridge, real Rust retriever, GPU model and console
FIFO. Its reply was `[Wiki] 3 [source]`, linking to the Lightning Rod crafting
section. The same client repeated the question after the cooldown.

| Tick window | Ticks | p50 | p95 | p99 | Max | Over 50 ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Baseline | 400 | 17.6 ms | 24.7 ms | 34.7 ms | 70.1 ms | 2 (0.50%) |
| First chat request | 1,276 | 14.7 ms | 28.1 ms | 47.9 ms | 101.4 ms | 10 (0.78%) |

Tick duration is the active time reported by the server's tick samples.
This single paired window includes model reload, prefill, and decoding. It
cannot isolate the cause of each long tick or establish performance during
chunk generation or multiplayer. The tail rose during the request, so the
live server's enable marker stayed absent.

## Changes and quality checks

- The stock Snapdragon binary hit an illegal instruction on this CPU.
  Rebuilding for ARMv8.2-A with FP16 and dot product allowed the server to run
  under the existing Bionic linker. The C.24 driver required an OpenCL 2.0
  build and a compatibility implementation of a packed signed dot-product
  intrinsic. See the [worker setup](../docs/wiki-chat.md#gpu-worker-setup).
- The bridge keeps the grounded prompt and lets the model server reuse
  matching tokens. The sampled phone sequence reused 106 tokens on distinct
  questions. It also stops at the first blank line and reads the answer line
  case-insensitively. Before that stop rule, a bookshelf answer generated 88
  tokens and spent 44.1 s in decode. Its first answer line already contained
  the correct count.
- The bounded reply cache skips generation for an exact question and matching
  evidence. Rerunning retrieval and comparing evidence keeps a changed revision or
  parsed passage from reusing an old reply. Unknown answers are left out.

The desktop evaluation used the 44 existing fresh questions with cached Java
Edition evidence, temperature zero, and the same 4B GGUF. The full grounded
baseline scored 29/36 factual questions and 8/8 abstentions, with a median of
325 ms. Prefix reuse and the blank-line stop preserved both scores, reduced
the median evaluated prompt from 310 to 204 tokens, and brought the median to
308 ms. Limiting the original prompt to two or three excerpts scored 27/36 and
28/36 factual questions respectively. A shorter instruction prompt scored
17/36. Bonsai 2B with a short direct prompt scored 23/36 facts and 2/8
abstentions. LFM3 1.7B with the full grounded stop profile scored 22/36 and
6/8. These comparisons kept the 4B grounded prompt and its original evidence
budget.

The repository benchmark reproduced the score with `--answer-style chat`.
It recorded median generation of 300.4 ms and median retrieval of 2.6 ms
with the persistent desktop retriever.

## Speculative decoding

Desktop prompt lookup used `ngram-simple`, a 3-token match, an 8-token lookup
window, and a 6-token draft limit. It generated the same scored answer on
all 44 cases and accepted 547 of 987 drafted tokens. Median decode increased
from 181 to 204 ms and median total latency increased from 325 to 338 ms.
This method drafts from token history and uses the target model to verify it.
[llama.cpp speculative decoding documentation](https://github.com/ggml-org/llama.cpp/blob/b12110/docs/speculative.md)

The phone A/B used the same crafting question and the blank-line stop, with
one cold prompt and one exact repeat per setting. Both settings generated
identical text. The repeat called the model directly to measure decode even
though the chat bridge would normally serve the cached reply.

| Phone setting | First prefill | First decode | Repeated prefill | Repeated decode | Accepted drafts |
| --- | ---: | ---: | ---: | ---: | ---: |
| Plain generation | 41.35 s | 8.25 s | 1.84 s | 8.25 s | — |
| Prompt lookup | 41.56 s | 13.68 s | 1.83 s | 13.62 s | 6/16 each |

The prompt-lookup setting stayed off. The cold phone prompt spent about
five times as long in prefill as in plain decoding. The trial used a single
paired question on the phone. It did not test a learned draft model.

A separate cold run paired `--ubatch-size 128` with `--no-warmup`. It gave
the same answer with 41.68 s prefill and 49.85 s total request time, versus
41.35 s and 49.65 s with the defaults. The launch-to-reply time was 71.0 s.
This combined test found no request-time gain, so the launcher keeps the
defaults.
