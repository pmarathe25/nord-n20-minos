# Additional small-model screen, November 10, 2026

This investigation compared retained Qwen3.5 2B Q4_K_M and the official
LFM2.5 2.6B QAD Q4_0 against Bonsai 2 4B Q2_0 on desktop. It also checked
current mobile inference alternatives. No phone state or installed model
was changed.

## Method

The screen used llama.cpp b12110, Vulkan0, all layers on GPU, six CPU
threads, one slot and a 2,048-token context. The Rust retriever ran offline
on an isolated extraction of the existing public cache archive. Each
model received the same retrieved passages for the 44- and 60-case sets.
The archive is older than the cache used in the November 10 Bonsai 2B
report, so this run includes its own matched 4B baseline.

The production-compatible screen used the existing grounded instruction,
temperature zero, a 140-token output cap, thinking disabled and the
blank-line stop. Global and per-request prompt reuse were disabled.
An additional instruction requested actual bracketed source numbers,
using the same wording as the previous Bonsai 2B screen. Factual passes
required all expected answer patterns, no rejected pattern, a valid
excerpt citation and a normal stop. Private passes used the rendered
standard abstention; parser fallback can produce it without a genuine
model abstention. These regression scores do not establish entailment.

LFM2.5 2.6B is a reasoning-only model according to its publisher. Its
second screen therefore retained the native thinking template, removed
the blank-line stop and allowed 512 generated tokens, with explicit
bracket instructions. This is an alternate request profile, not a
drop-in production replacement. Desktop timing is not reported because
other desktop GPU work overlapped.

## Current architecture alternatives

[LFM2.5 2.6B](https://huggingface.co/LiquidAI/LFM2.5-2.6B) has 22 short
convolution blocks and eight grouped-query attention blocks, with a
128,000-token vocabulary. Its official
[QAD Q4_0 checkpoint](https://huggingface.co/LiquidAI/LFM2.5-2.6B-GGUF)
is 1,593,894,944 bytes. It also has a 328M speculative drafter, but the
publisher's speed claim is for SGLang and Apple Metal. Neither that
claim nor desktop Vulkan establishes a speedup on Adreno 619. The
reasoning tokens are part of the end-to-end decode cost.

[Gemma 4 E2B's LiteRT model](https://huggingface.co/litert-community/gemma-4-E2B-it-litert-lm)
uses mixed 2-, 4- and 8-bit weights and memory-mapped per-layer embeddings.
The stated text weight footprint can be about 0.8 GB plus 1.12 GB of
memory-mapped embeddings; this is not the model download size. Published
files include a 2.01 GB GPU artifact and a 2.59 GB general artifact.
Its published warm benchmarks exclude model loading and use newer
hardware. The available Qualcomm NPU artifact explicitly targets
QCS8275/Hexagon v75, not this phone's v66.

[LiteRT's platform matrix](https://github.com/google-ai-edge/LiteRT/blob/main/README.md)
lists OpenCL/OpenGL for Android and WebGPU for Linux. The
[LiteRT-LM build guide](https://github.com/google-ai-edge/LiteRT-LM/blob/main/docs/getting-started/build-and-run.md)
provides an Android native build path. A native Android runtime using
the existing C.24 GPU driver is therefore a concrete investigation,
but driver compatibility, total phone memory, grounded quality and
end-to-end latency remain unmeasured. It is not a ready replacement.

[LFM2.5 8B-A1B](https://huggingface.co/LiquidAI/LFM2.5-8B-A1B)
has 8.3B total parameters despite 1.5B active. Its lower active count
does not remove the resident expert weights. It is not an attractive
first experiment for this phone while Minecraft shares its memory.

## Model identity

LFM2.5 2.6B QAD Q4_0 SHA-256:
`a247afd6414918eac8e520a9e6137dc271235461ecbe1180462221d5b8d40b03`.

Retained Qwen3.5 2B Q4_K_M SHA-256:
`aaf42c8b7c3cab2bf3d69c355048d4a0ee9973d48f16c731c0520ee914699223`.

The retained file hash records local identity, not independent publisher
provenance. The downloaded LFM model came from LiquidAI's official repository.

## Full regression results

| Model/profile | Facts, 44 cases | Private, 44 cases | Facts, 60 cases | Private, 60 cases |
| --- | ---: | ---: | ---: | ---: |
| Bonsai 4B, existing grounded | 29/36 | 8/8 | 38/48 | 12/12 |
| Qwen3.5 2B Q4_K_M, existing grounded | 10/36 | 6/8 | 19/48 | 12/12 |
| Qwen3.5 2B, explicit brackets | 20/36 | 7/8 | 21/48 | 12/12 |
| LFM2.5 2.6B QAD, existing grounded | 0/36 | 8/8* | 0/48 | 12/12* |
| LFM2.5 2.6B QAD, explicit brackets | 0/36 | 8/8* | 0/48 | 12/12* |

The LFM rows are request-profile failures: the blank-line stop terminated
responses during reasoning, leaving empty final content. The starred
private scores are parser fallback, not genuine model abstention. They
must not be interpreted as evidence of private-state judgment.

Qwen's explicit citation instruction improves formatting but leaves clear
factual failures: it permits blast-furnace sand smelting, says a bookshelf
requires one book, and answers that the campfire at the player's base is
currently lit. Some other failures are correct-looking answers without a
citation, which the current chat contract cannot use. Qwen's median output
across its two profiles was 19 tokens versus 34 for the matched 4B. Shorter
output does not compensate for the observed quality loss.

A full native-thinking LFM run was stopped cleanly because generation
frequently approached hundreds of tokens. A separate diagnostic run uses
only the first eight factual questions and all eight private questions in
the 44-case set. It is not a substitute for a full-set comparison.

In that 16-case native-thinking diagnostic, LFM passed 3/8 factual and
3/8 private cases, against 5/8 and 8/8 for the matched Bonsai baseline.
Nine LFM responses exhausted the 512-token cap; eight had no final answer.
Median generated tokens were 512 for LFM versus 29.5 for Bonsai on this
subset. One further private answer correctly explained that the wiki
could not establish the state but added text to the required abstention,
so its strict-format score failed. This does not establish LFM's unlimited
reasoning accuracy; it establishes that even a much larger output budget
did not provide a usable low-latency replacement in this diagnostic.

The temporary model servers exited. The new download, isolated cache,
raw answers, scripts and logs were removed after recording these results.
Preexisting runtime, model and cache assets were retained. No phone test
or deployment followed this quality screen.
