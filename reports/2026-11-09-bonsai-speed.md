# Nord N20 Bonsai speed experiments, November 9, 2026

The reference model was Bonsai 2 4B Q2_0. Phone tests used the C.24 Adreno
619 OpenCL driver, llama.cpp b12110 with the earlier MinOS patches, and two
threads on efficiency cores at nice 15. The benchmark used a copy of the
installed Java Wiki cache, a private socket, and a separate shader cache.
The main prompt tests disabled prompt reuse to measure the work for new
questions. Model times exclude Rust retrieval and game networking.

## Fewer input and output tokens

Desktop Vulkan used fixed retrieved passages for the repository's
44-question and 60-question sets. The scorer required a matching answer
and a valid excerpt number for factual questions. The private questions
tested abstention. These are the existing regression sets.

| Prompt and evidence | Factual, 44-case set | Factual, 60-case set | Private abstentions | Median output tokens |
| --- | ---: | ---: | ---: | ---: |
| Existing support then answer | 31/36 | 38/48 | 20/20 | 32–35 |
| Existing prompt, two sentences per excerpt | 30/36 | 37/48 | 20/20 | 33–34 |
| Citation-only support line | 30/36 | 34/48 | 20/20 | 11 |
| Short constrained output, one sentence per excerpt | 25/36 | 23/48 | 20/20 | 4 |
| Four examples, short constrained output, full evidence | 28/36 | 31/48 | 19/20 | 4 |
| Existing prompt with four-word support limit | 28/36 | 37/48 | 20/20 | 19–20 |
| Existing prompt with eight-word support limit | 30/36 | 37/48 | 20/20 | 23–24 |

The one-sentence prompt cut median input from 310–316 to 192–199
tokens. The two-sentence variant cut just 2.5–6.5 tokens. The retriever
already supplies short excerpts. The citation-only support line cut
desktop median request time from 359–375 ms to 203 ms, with five fewer
correct factual answers in total. On the 33-question subset beginning
with a yes/no verb, a constrained yes/no and source number scored
22/33 overall versus 26/33 for the existing prompt.

On the phone, the one-sentence constrained prompt took 32.14 s for
the dried-kelp fuel question and 24.37 s for the torch light-level
question. The existing prompt took 56.05 s and 45.84 s respectively
and answered both correctly. The shorter prompt returned an
abstention on both, even though it answered both on desktop.

A layer-drop experiment retained the first eight and last eight of
32 blocks and left their weights unchanged. It kept whole four-layer
attention cycles. Desktop prompt and per-token forward passes were
faster, but it answered 0/36 factual questions, abstained correctly
on 4/8 private questions, and reached the 140-token output cap in
31 of 44 runs.

## Accelerator kernels

For the two layer-0 feed-forward shapes, the projection probe compared
direct image reads with staging activations once per workgroup in local
memory. With 128 tokens, the 256-column staging tile cut median GPU
time from 61.29 to 59.71 ms for up and 61.60 to 60.23 ms for down.
The 1,024-column staging tile took 89.13 and 88.36 ms. Normalized RMS
errors matched the original kernel. A simpler integer-to-half weight
conversion alone was slightly slower.

The 512-wide Hadamard probe used Qualcomm subgroup shuffles for
within-wave butterfly stages and registers for the last two stages.
It matched the scalar result on all checked F32 values. With 64
synthetic rows it ran in 1.08 ms versus 1.14 ms for the existing
kernel. At 4,096 rows it took 69.26 ms versus 50.34 ms. The
projection staging and subgroup kernels stayed in the benchmark.
[Qualcomm OpenCL programming guide](https://docs.qualcomm.com/doc/80-NB295-11/80-NB295-11_REV_C_Qualcomm_Snapdragon_Mobile_Platform_Opencl_General_Programming_and_Optimization.pdf)

The four-lane Q2_0 tile now handles batches of two or three tokens
under the existing opt-in switch. It retains the backend's eight-token
input padding. With two or three tokens and real Bonsai weights, it cut
median GPU event time by 35–36% on both tested feed-forward shapes.
The two tile widths gave the same normalized RMS errors against the
double-precision reference. In the whole-model three-token prompt
probe, warm time to the first token fell from 1.296 to 1.050 s.
All six runs produced the same first token and full logits hash.
The first run took about 12 s for either width with first-use costs.

## Speculative decoding

The draft-head trial used the Q8_0 prediction block from PrismML's
published PQ2_0-MTP checkpoint. The scratch GGUF retained the base
Q2_0 tensor bytes and appended the 15 draft tensors. The merge checked
the tokenizer, architecture parameters, Hadamard settings and shared
tensor shapes against the publisher's checkpoint before writing the
file. The test used upstream b12110's Qwen3.5 MTP support.

On desktop, two-token MTP accepted 180 of 228 proposed tokens. All ten
responses matched the same scratch checkpoint with drafting disabled.
Median decode time fell from 180 to 153 ms. Median total time went from
305 to 313 ms with the additional draft prefill work.
[PrismML Bonsai 2 4B and MTP](https://huggingface.co/prism-ml/Ternary-Bonsai-2-4B-gguf)
[Pinned llama.cpp speculative decoding](https://github.com/ggml-org/llama.cpp/blob/b12110/docs/speculative.md)

On Adreno, prompt lookup used three matching tokens and drafted up
to four. MTP drafted up to two. Each row compares the same requests
under ordinary greedy decoding and the indicated drafter. Seconds
include the prompt and complete answer, with the server already
listening. The outputs matched ordinary decoding in every sampled
case.

| Drafter | Cases | Accepted tokens | Ordinary median | Speculative median |
| --- | ---: | ---: | ---: | ---: |
| Prompt lookup | 5 | 46/68 | 45.84 s | 53.84 s |
| Trained head | 3 | 36/50 | 45.84 s | 54.83 s |
| Trained head with earlier draft prefill skipped | 2 | 19/48 | 50.95 s | 63.63 s |

The shorter draft context kept the final hidden row from the skipped
target batches and left the target's full context intact. On the two
matched requests, median prefill fell from 44.65 to 42.95 s relative
to the full draft context. Acceptance fell from 25/36 to 19/48.
Median decode time rose from 15.96 to 20.67 s. One request overlapped
Minecraft restarting after an idle stop, so differences of a few
seconds between requests should be treated as approximate.
