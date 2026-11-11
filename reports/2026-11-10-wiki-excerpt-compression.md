# Wiki excerpt and support-line compression, November 10, 2026

Bonsai 2 4B Q2_0 retained the same passing cases with conservative
excerpt trimming, but the median prompt shrank by only five tokens.
More aggressive trimming and shorter support lines lost factual
answers. The shorter support line also missed two private-state
abstentions.

## Desktop screening

Desktop llama.cpp b12110 ran Vulkan0 with all layers on GPU, one slot,
a 2,048-token context, six CPU threads, temperature zero, thinking
disabled, a 140-token output cap and a blank-line stop. The Rust
retriever searched a private copy of a public parsed Wiki cache
offline. Each of the 104 repository cases was retrieved once. Prompt
variants used those same ranked excerpts or shortened them as described
below. The requests disabled active prompt reuse and interleaved the
variants in deterministic shuffled order.

The test passed model output through the actual bridge's
`grounded_reply` and `render` functions. A factual pass needed a normal
stop, the existing expected and rejected answer patterns, and a
rendered link to an expected Wiki title. A private-state pass needed
the standard abstention. The title check is stricter than requiring
any valid excerpt citation. The scorer checks regression patterns and
does not prove that the cited passage supports the answer.

| Variant | Factual, 44-case set | Factual, 60-case set | Private | Median input tokens | Median output tokens |
| --- | ---: | ---: | ---: | ---: | ---: |
| Existing grounded request | 28/36 | 35/48 | 20/20 | 313.5 | 34 |
| Conservative excerpt trimming | 28/36 | 35/48 | 20/20 | 308.5 | 33 |
| Aggressive excerpt trimming | 27/36 | 33/48 | 20/20 | 259 | 32.5 |
| Terse support line | 26/36 | 34/48 | 18/20 | 320.5 | 19 |
| Short instruction | 3/36 | 3/48 | 20/20 | 277.5 | 18 |

The existing request scored 67/84 factual cases when any valid
citation was accepted, versus 63/84 when the cited title had to
match. The six questions already handled by the bridge's exact
route all passed the stricter score. Combining that exact route
with the full model still passed 63 factual cases because the model
also answered those six correctly.

The excerpt trims ranked sentences by overlap with the question's
lowercased, lightly stemmed words after removing common function
words. The conservative trim kept all four excerpt slots, kept the
highest-scoring sentence and a neighboring sentence in longer
passages, and dropped a sentence from a two-sentence passage only
with strong overlap in one and none in the other. It preserved
semicolon-delimited records. This shortened 37 prompts by 1,409
tokens in total. Three rendered answers or titles changed, with
the same 63 passing factual IDs and all 20 abstentions.

The aggressive trim kept at most three excerpts, usually with one
sentence per excerpt. It kept a neighbor for the first excerpt and
when a selected sentence with a pronoun had a preceding sentence.
It shortened every prompt by an average of 61.3 tokens. Four
baseline-correct cases regressed and one became correct. It used
a net-fuel figure of 11 instead of the gross yield of 20 for the
dried kelp block. The existing exact route repairs that case, but
three scored regressions remain. One omitted the word netherite from
a “diamond or better” answer. Another dropped the fourth passage,
which stated the 12-block piston limit. The zero remaining respawn
charges case was baseline-correct despite a citation about light
level that does not establish charge consumption.

The terse support instruction asked for a bracketed number and
a key phrase or calculation without full sentences. It cut median
output by 15 tokens, but answered a private comparator question
with 4 and a private shield durability question with 336. The
shortened system instruction often produced a bare support number
or ended the reply before an answer line, so the bridge could not
render a cited answer.

Desktop timing varied across the run. Token counts are reported
here as work estimates. No phone inference or live changes were
made for this screen.


## Cache-disabled follow-up

A second pass disabled llama.cpp's global prompt cache and repeated
the full 104-case screen. The conservative trim reproduced the
first pass's scores and response differences. A title-based filter
removed cross-page passages only when the top Wiki title appeared
in the question, at least two passages came from that title, and the
other passage did not add a question keyword absent from the
same-title evidence. It shortened four prompts by 194 tokens in
total and produced identical model output on all 104 cases.

A guarded aggressive trim kept full evidence when the question
contained conditional or explicit private-state wording. It cut
median input to 276.5 tokens. It restored the respawn charge score
but still lost three baseline passes and gained one. The existing
exact-answer route restored one of those passes.

| Variant | Factual with expected title | Private | Median local request |
| --- | ---: | ---: | ---: |
| Existing grounded request | 63/84 | 20/20 | 308.25 ms |
| Conservative excerpt trimming | 63/84 | 20/20 | 311.48 ms |
| Title-based filter | 63/84 | 20/20 | 308.47 ms |
| Guarded aggressive trimming | 61/84 | 20/20 | 300.56 ms |

These desktop medians cover complete local HTTP requests and
exclude retrieval. Neither quality-preserving trim had a measured
latency benefit in this comparison. Both desktop model processes
were stopped after the test. The copied cache, raw replies and scratch logs were removed.
