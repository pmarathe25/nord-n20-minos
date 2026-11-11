# Token-level Wiki prompt overlap, November 10, 2026

Partial evidence prefixes can match even when the complete evidence lists
differ. On the 104 distinct repository questions, however, this sample
offers little reuse to a small recent-question cache. This is an optimistic
token-overlap study, not a measured speedup or a proposal to deploy a new
cache.

## Method and results

The phone's installed Rust retriever ran offline against a private copy of
its parsed Wiki cache. The study combined the 44- and 60-question
repository sets, deduplicated exact questions, and rendered each actual
user-message content with `chat_request_body`. All 104 returned passages.
The model's own GGUF vocabulary tokenized the user contents, with automatic
special tokens disabled. The common system message and chat-template tokens
were not included. Because user content was tokenized in isolation, a
tokenization boundary at the chat header could differ from the complete
rendered prompt. No generated answers or gameplay traffic were measured.

All contents shared six initial tokens; their median length was 195.5
tokens. Treating every other question as available, the best additional
common prefix after those six tokens had a median of two tokens and a
maximum of 91. The table counts distinct questions with at least one
matching other question. The last column counts unordered matching pairs.

| Extra matching tokens | Questions | Pairs |
| ---: | ---: | ---: |
| At least 8 | 27 | 18 |
| At least 16 | 14 | 7 |
| At least 32 | 12 | 6 |
| At least 64 | 4 | 2 |

For an even more optimistic bounded-cache screen, 64 fixed-seed shuffled
orders of these questions were simulated for each capacity. The most recent
one, four, eight or sixteen questions were treated as cached; an oracle could
resume at any matching token prefix, rather than just an actually saved
checkpoint. The figures exclude the six common opening tokens.

| Recent questions retained | Mean extra matching tokens per question | Fraction matching at least 16 extra tokens |
| ---: | ---: | ---: |
| 1 | 0.086 | 0.11% |
| 4 | 0.476 | 0.50% |
| 8 | 0.863 | 0.95% |
| 16 | 1.623 | 1.91% |

The sample was designed as a diverse regression set, not a live traffic
distribution. Repeated or related questions could reuse substantially more.
Conversely, these figures assume a checkpoint at every possible matching
prefix. The recurrent Gated DeltaNet state cannot simply be rewound to an
arbitrary earlier token, so a real cache must explicitly save that state.
The existing system-prefix snapshot is about 56 MB, and each additional
snapshot has memory or storage and restore costs. Neither those costs nor
any end-to-end savings are included here. The common six user-content tokens
are not already claimed as part of the 106-token system snapshot; saving
them would require a separately tested checkpoint boundary.

This refines the [earlier whole-passage comparison](2026-11-10-wiki-evidence-reuse.md):
whole passages were a stricter match, and some shorter token prefixes do
recur. The observed overlap in this sample does not justify building a large
evidence checkpoint cache ahead of more consequential model work.

## Reproduction

Build [the tokenizer](../wiki/experiments/n20-tokenize-stream.c) against the
same b12110 llama and GGML libraries as the model, using the Android NDK.
It loads only the GGUF vocabulary and accepts length-prefixed UTF-8 strings.
Run [the overlap script](../wiki/experiments/n20-prefix-overlap.py) with
`wiki_answer.py` on `PYTHONPATH`, a private parsed-cache copy, the two case
files, and `--tokenizer` followed by the Android linker command, tokenizer
binary and model path. The script emits aggregate counts only. Python
byte-compilation passed on ARM64 Debian. The tokenizer built with
`-O3 -Wall -Wextra -Werror` and ran under the phone's Android linker; its
framing check covered empty and repeated Unicode input and truncated input.
No GPU worker, production cache,
Minecraft world or service setting was changed.
