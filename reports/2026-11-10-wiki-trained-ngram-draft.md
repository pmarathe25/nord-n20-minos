# Trained n-gram drafting screen, November 10, 2026

A static n-gram cache built from earlier Wiki answers did not improve
complete desktop requests in this screen. The result does not establish
phone performance. It is distinct from the earlier current-prompt-only
`ngram-simple` experiment.

The b12110 (`cd959bae9`) server ran Bonsai 2 4B Q2_0 on Vulkan0 with one
2,048-token slot, six CPU threads and all layers offloaded. Each fresh
server warmed once, then answered the same 60 held-out questions in order.
All arms used the same cached passages, `chat_request_body`, temperature
zero, no prompt reuse and a blank-line stop. Times cover local HTTP request
submission through the complete response; retrieval and startup are excluded.

| Mode | Median complete request | Median decode | Accepted / proposed |
| --- | ---: | ---: | ---: |
| Plain before | 344.6 ms | 201.4 ms | — |
| Context-only `ngram-cache` | 417.2 ms | 281.9 ms | 1,120 / 2,482 |
| `ngram-cache` plus trained static cache | 407.5 ms | 266.6 ms | 1,102 / 2,479 |
| Plain after | 349.7 ms | 204.5 ms | — |

All four arms scored 32/48 factual cases using the expected answer and
source-title patterns and 12/12 private-state abstentions. These automated
patterns do not prove factual entailment. Raw text or finish reason differed
from the first plain run on two context-only and three static-cache cases;
one static-cache reply had different player-visible wording. The final plain
run matched all 60 original plain outputs. Therefore this screen does not
establish exact response parity for the speculative paths.

The 20,480-byte static cache contained 694 keys trained from 80 direct-answer
outputs produced in earlier benchmark-v5 work, not current two-line
Support/Answer completions. The held-out fixtures had no exact-question
overlap with those 80 examples, but may share topics, pages and wording.
The outputs were concatenated with blank-line separators and tokenized once
using b12110 `llama-tokenize --ids --no-bos --no-parse-special`; this also
permits bigrams across document boundaries. A helper counted next-token
frequencies for two-token keys and wrote the b12110 cache's little-endian
token/count representation. This is a limited training corpus, not an
evaluation of a comprehensive domain-trained predictor.

The speculative arms added `--spec-type ngram-cache --spec-draft-n-max 4`;
the static arm also passed `--lookup-cache-static` with the generated file.
The paired result argues against promoting these specific settings. It does
not rule out different training or an implementation optimized for this
phone, whose request latency and system-prefix reuse differ from desktop.
Temporary server processes and scratch artifacts were removed after the
aggregate results were recorded; preexisting model and fixture files stayed
in place.
