# Wiki evidence formatting and passage ranking, November 10, 2026

The desktop comparison used Bonsai 2 4B Q2_0, llama.cpp b12110 on Vulkan0,
a 2,048-token context, one slot and six CPU threads. Each arm used the
bridge's existing system instruction, temperature zero, thinking disabled,
a 140-token output cap and a blank-line stop. Global and per-request prompt
reuse were disabled. Questions and arms were interleaved in a fixed shuffled
order. Separate GPU work overlapped this run, so the comparison reports
model token counts without a desktop timing claim.

The Rust retriever ran offline on a scratch copy of the public parsed
phone cache archive. The test combined 104 repository cases with 31 newly
written questions covering related topics, new combinations, recipe and
mechanics questions, and private state. The holdout has 21 factual cases
and 10 private-state cases. It includes questions such as whether a grindstone
removes a curse, whether a fortune pickaxe affects glass, how a brewing stand
is fueled, and how many slots are occupied in a player's actual brewing stand.
Neither the scorer nor the implementation was tuned using the held-out
model answers.

A factual pass required the expected and rejected answer patterns and a link
to an expected article, through the bridge's exact-answer and response rendering
functions. A cited-evidence pass additionally required the expected patterns
in the actual cited excerpt. A private-state pass required the standard
abstention. These are regression patterns. They can miss legitimate
paraphrases, and a passage containing the expected words can still refer to
a different condition. The offline cache sometimes lacks decisive evidence.

The formatting arms kept the existing ranking. The two-passage arms kept
the highest-ranked pair and relabeled the passages. The guarded version
reduced passage count only when the first passage's title and content or
section covered every non-stop question word after light plural normalization.
It used the same headings-free formatting as the unconditional two-passage arm.
Other arms retained all available passages.

The ranking arms kept the original evidence formatting. The title-weighted
arm awarded six ranking points when a complete title or alias occurred in
the question and the candidate matched another question term in the passage
or section. It subtracted three when the article had no title overlap. The
section-diversity arm subtracted three selection points for each earlier
selected excerpt with the same title and section. A fourth ranking arm
combined the two changes. The rebuilt stock retriever produced identical
ordered passages for all 135 questions. The generation screen reused the
baseline result for each exact match of the ordered passage list.

The numbers below give factual passes with an expected source, then passes
whose cited passage also met the evidence patterns. Tokens are medians for
the complete prompts and generated outputs, including requests simulated
through the exact-answer route for scoring. In production that route skips
inference.

| Arm | Repository factual | Repository evidence | Repository private | Repository input / output tokens | Fresh factual | Fresh evidence | Fresh private | Fresh input / output tokens |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |

| Existing | 63/84 | 50/84 | 20/20 | 313.5 / 34 | 9/21 | 9/21 | 9/10 | 309 / 40 |
| Omit Overview label and double separators | 63/84 | 49/84 | 20/20 | 313 / 33 | 9/21 | 9/21 | 10/10 | 309 / 40 |
| Also omit repeated consecutive titles | 61/84 | 49/84 | 19/20 | 310.5 / 33 | 9/21 | 9/21 | 10/10 | 306 / 37 |
| Omit all headings | 61/84 | 51/84 | 19/20 | 296.5 / 31 | 9/21 | 9/21 | 10/10 | 293 / 38 |
| Numbered text only | 59/84 | 47/84 | 20/20 | 288.5 / 32 | 9/21 | 8/21 | 10/10 | 285 / 37 |
| Title and coverage guard, two passages | 60/84 | 49/84 | 19/20 | 295 / 31 | 10/21 | 10/21 | 10/10 | 293 / 39 |
| Always two passages | 58/84 | 49/84 | 19/20 | 218 / 33.5 | 8/21 | 8/21 | 10/10 | 206 / 36 |
| Question before excerpts | 57/84 | 47/84 | 18/20 | 313.5 / 30 | 11/21 | 10/21 | 8/10 | 309 / 31 |
| Extra weight for exact title | 59/84 | 48/84 | 20/20 | 312 / 33 | 10/21 | 10/21 | 9/10 | 297 / 38 |
| Diversify article sections | 61/84 | 51/84 | 20/20 | 313 / 34.5 | 8/21 | 8/21 | 10/10 | 313 / 37 |
| Both ranking changes | 57/84 | 49/84 | 20/20 | 313.5 / 34 | 10/21 | 10/21 | 10/10 | 300 / 36 |

The two-passage format cut the repository median by 95.5 input tokens and
the fresh median by 103. It lost six repository factual passes net and one
fresh factual pass net. An assertion that a player's campfire was lit replaced
the baseline abstention. On another case, it said a hopper could collect
items through a full solid block. On the fresh set, it missed the grindstone
curse and brewing fuel questions. It did improve shulker-box capacity from
nine to 27 and correctly abstained when asked who took blaze powder out of a
particular brewing stand. The baseline answered with a hopper location.
The guarded two-passage format saved 18.5 and 16 median input tokens,
respectively, and answered a question about a particular shield's remaining
durability with the generic full value of 336.

The smallest formatting change omitted the Overview label and used single
newlines between excerpts. It saved a mean of 1.6 input tokens on the
repository questions and 1.7 on the fresh questions. The factual totals were
equal, with a different pair of repository passes and one fewer cited-evidence
pass. It also abstained correctly on the fresh brewing-stand question.
Dropping every article label reduced the repository median by 25 tokens and
lost four repository factual passes.

Among the four retrieved passages, baseline coverage was 64/84 repository
and 12/21 fresh when a single passage had to match both the title and evidence
patterns. Section diversity raised coverage to 66/84 and 13/21, while generated
factual scores went from 63/84 and 9/21 to 61/84 and 8/21. It replaced a
correct shield answer with a denial and attached the Tick article to a
correct negative answer about hopper pickup. Stronger title weighting
recovered a fresh conduit-frame answer but, among the repository cases,
replaced glowstone with a redstone comparator for respawn-anchor charging.
The coverage gains came with factual regressions.

The two desktop model processes were stopped. The scratch cache, binaries,
raw replies and logs were moved to the Recycle Bin after recording the results.
