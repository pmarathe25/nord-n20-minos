# Nord N20 fast Wiki answers and prefix snapshots, November 9, 2026

The phone used the installed Bonsai 2 4B Q2_0 model, Adreno 619, and C.24
OpenCL driver. The model tests used two efficiency-core threads at nice
15 with the live Minecraft server idle. The trial copied the parsed Wiki
cache and used isolated sockets and GPU shader caches.

## Model execution

The fused weight loader reads raw GGUF blocks and transposes the codes
and scales in a single GPU kernel. With the other Adreno optimizations
enabled in both runs, model and context initialization fell from 8.49 s
to 5.59 s. The warm 185-token prompt took 20.68 s before and 20.59 s after.
Both runs produced the same next-token logits hash.

At 128 tokens, the two-row, 16-token prefill tile took 142.55 ms for the
up projection and 147.21 ms for the down projection. The existing
four-row, eight-token tile took 61.15 and 61.77 ms. The half-wave variant
was also slower. The whole-model tests kept the four-row tile.

The 350M LFM2.5 QAD Q4_0 checkpoint took a median of 1.80 s on Adreno and
3.99 s on two efficiency cores for 12 uncached questions with a compact
prompt. GPU prefill was about 143 tokens/s and decode about 17 tokens/s.
The cited-answer scores were 0/12 on GPU and 1/12 on CPU. On desktop,
the compact prompt scored 4/36 factual answers. Some outputs gave the
right value with a literal citation placeholder. Others gave the wrong
value. The 350M model did not replace Bonsai.
[Liquid AI QAD release](https://huggingface.co/blog/LiquidAI/qad)

## Prompt comparison

The same Bonsai GGUF on desktop received the same cached passages under
each prompt. Answers needed a valid source number and the case's expected
answer. One rewrite requested a single line and included a literal
citation example. The other kept two lines and asked the model to verify
the item, variant, condition, and units.

| Prompt | Factual, 44-case set | Factual, 60-case set | Correct abstentions |
| --- | ---: | ---: | ---: |
| Existing support-then-answer prompt | 31/36 | 38/48 | 20/20 |
| One-line rewrite | 26/36 | 28/48 | 20/20 |
| Verification rewrite | 26/36 | 31/48 | 20/20 |

The existing prompt stays in the chat worker.

## State saved across process restarts

The harness prefilled a 39-token instruction or the instruction plus
fixed evidence and the start of the question field. It saved each
snapshot to disk and loaded it in a new model process. The full
185-token prompt asked which excerpt supported the observer pulse
duration. The harness picked one digit from the first-token logits.

| Cached prefix | Tokens | State file | Build time | First request to choice | Warm request to choice |
| --- | ---: | ---: | ---: | ---: | ---: |
| Full prefill each time | 0 | — | — | 29.79 s | 20.59 s |
| System instruction | 39 | 54.0 MB | 15.34 s | 26.10 s | 16.77 s |
| Instruction and evidence | 164 | 58.1 MB | 27.81 s | 13.82 s | 3.14 s |

Disk restore took 55–65 ms and is included in the request timings.
Model and context loading took another 5.4–5.6 s in the tuned runs
and is outside the table. First requests also paid first-use kernel costs.
Every run chose the correct source and produced the same logits hash.
The 3.14 s result is a source choice for one cached evidence prefix.

On desktop, putting the question after the facts let Bonsai choose the
labeled supporting excerpt in 21 of 31 eligible cases and decline all
eight private-state questions. Putting the question first gave 24 of 31
and four of eight. These scores measure evidence selection. A verdict or
arithmetic can still require another step.
[Pinned sequence state APIs](https://github.com/ggml-org/llama.cpp/blob/b12110/include/llama.h)

Bonsai's state contains attention KV and recurrent state. A snapshot
represents one exact sequence starting at the beginning of the prompt.
Keep its key tied to the model, runtime state format, rendered prompt
tokens, context settings, and the revisions of any Wiki excerpts it
includes. Separate page snapshots cannot be concatenated into a combined
prompt state. A thousand snapshots at the measured evidence size would
occupy about 58 GB before compression.

## Vector retrieval

The existing retrieval-augmented generation path searches the parsed
Wiki cache with SQLite FTS5 and a task-specific ranker. A desktop
comparison embedded all 7,348 distinct indexed passages with the
384-dimensional BGE small English model. The raw FP16 vector array
occupied 5.64 MB. The experiment used cosine similarity and compared
the first four passages under the same evidence scorer.

| Four-passage selection | Factual questions with labeled evidence |
| --- | ---: |
| Existing lexical ranker | 32/36 |
| Dense vectors | 33/36 |
| Equal-weight reciprocal rank fusion | 32/36 |
| Evidence anywhere in the cached corpus | 35/36 |

Vectors recovered three lexical misses and missed two questions the
lexical ranker covered. The union of both sets of four covered 35/36.
The measured scores are evidence recall. The trial did not test an
on-device embedding runtime or pass the expanded set to the generator.
The vector index was experimental.
[BGE model card](https://huggingface.co/BAAI/bge-small-en-v1.5)
[SQLite FTS5](https://www.sqlite.org/fts5.html)

## Exact answers from cached evidence

The bridge can read explicit crafting counts and property sentences from
retrieved excerpts. It retains the source link and passes conflicts,
unsupported variants, and other question forms to the existing model.
The route selected six of 36 factual questions in the 44-case set and
got all six right. It did not route the eight separate recipe prompts
with the cached evidence available in this run.

On the phone, five distinct questions took a median of 30 ms for one-shot
retrieval and reply construction. The same questions took a median of
390 ms through a separate chat log and console FIFO under the Minecraft
account. The model socket was absent. This measures the bridge with a
simulated chat source, excluding game networking and ticks.
