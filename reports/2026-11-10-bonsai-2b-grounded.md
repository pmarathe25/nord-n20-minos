# Bonsai 2B grounded quality check, November 10, 2026

Bonsai 2 2B did not match the existing 4B model on the two Wiki regression
sets. Explicit citation instructions repaired most formatting failures but
left lower factual scores and four failures to abstain on private state.
No phone inference or production model replacement followed this result.

## Method

The test used desktop llama.cpp b12110 (`cd959bae9`), Vulkan0, all layers
on GPU, a 2,048-token context, one slot, six CPU threads, temperature zero,
a 140-token output cap, thinking disabled and a blank-line stop. Prompt
reuse was disabled. Each model ran in its own temporary loopback server.
The existing Windows Rust retriever read an isolated copy of the phone's
public parsed Wiki cache offline. All variants received the same passages.
The production `GROUNDED`, `request_body` and `grounded_reply` functions
provided the initial prompt and response handling.

The 44-case set is `wiki/benchmark-fresh.json` (36 factual, 8 private).
The 60-case set is `wiki/benchmark-cases.json` (48 factual, 12 private).
A factual pass required the existing expected-answer regular expressions,
no rejected expression, a valid bracketed excerpt number and a normal stop.
Private passes required the exact standard abstention after citation removal.
These are regression scores, not independent human accuracy judgments; the
scorer can reject semantically reasonable shorthand and does not verify that
the cited passage entails the answer.

## Results

| Model and prompt | Facts, 44 cases | Facts, 60 cases | Private, 44 cases | Private, 60 cases |
| --- | ---: | ---: | ---: | ---: |
| 4B, existing grounded | 31/36 | 38/48 | 8/8 | 12/12 |
| 2B, existing grounded | 2/36 | 1/48 | 6/8 | 11/12 |
| 2B, explicit brackets added | 26/36 | 32/48 | 6/8 | 10/12 |

The matched 4B baseline reproduces the earlier published scores. The added
instruction was exactly:

> Write the source number in square brackets, for example Support: [1] followed by the decisive fact. Do not write a bare source number.

With the original prompt, 2B often wrote `Support: 1`, which the production
reply parser cannot convert to a citation. Ignoring citations, its answer
regex scores were 29/36 and 29/48. With the bracket instruction those scores
were 27/36 and 33/48. Correct-looking uncited answers are not suitable for the
existing chat route; changing the parser alone would not fix the remaining
factual or private-state failures.

Examples after adding the bracket instruction:

- Slowest repeater setting: `8 [2]`, confusing game ticks and redstone ticks.
- Elytra repair with phantom membranes: denied that repair is possible.
- Whether the campfire at the player's base is currently lit: `Yes [2]`,
  inferred from a general property conditional on being lit.
- The server's current comparator signal: `4 [1]`, copied from a worked
  arithmetic example rather than abstaining.
- Exact current offhand shield durability: `336 [1]`, substituting a general
  item property for private state.

Median output lengths for 44/60 cases were 31.5/35 tokens for 4B, 17/22
for the original 2B prompt, and 19/26 for explicit brackets. Desktop request
timings are not reported as a speed comparison because another desktop GPU
experiment overlapped. No phone latency claim follows from these runs.

## Model identity and scope

The existing 546,689,504-byte 2B file identifies itself in GGUF metadata as
`Ternary-Bonsai-2-2B`, architecture `qwen35`, file type 41. SHA-256:
`c2893fa6f951daee8b7263d5f49b455e57989ef56417a22125b03566dc560650`.
The 4B checkpoint SHA-256 was
`64a5183b44921bdb5ac4a6f5f92007821d6ebe5c66997d3c147fd214ab9433d4`.
This checks local file identity, not independent publisher provenance.

No live world, server settings, firmware, model service or cache was changed.
The desktop servers were stopped after testing. Scratch cache copies, raw
answers and logs were removed after recording these findings. The original
preexisting model and runtime assets were retained.
