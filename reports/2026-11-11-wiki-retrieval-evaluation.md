# Wiki and location evaluation

An offline Windows run on 2026-11-11 used a disposable copy of the existing
local parsed Wiki cache and the debug Rust retriever. It evaluated 118 existing
questions across the four checked-in case files. Of 98 cases with evidence
labels, 93 retrieved a labeled title and 73 matched all evidence patterns.
Nine questions took the production exact-answer route and all nine matched
their answer patterns. Twenty cases lack evidence labels, mostly private-state
questions, so they are excluded from retrieval rates. These checks do not
measure model answer accuracy or establish correctness; regexes can miss valid
evidence and accept wrong answers. The copied cache contains older parses;
several crafting rows have ingredients but lack the newer ingredient counts.

Examples needing investigation include the slowest repeater setting, where
the selected repeater row describes the first setting, and a beacon range
question whose selected passages do not include the requested range. The
piston limit appeared in a sentence starting “When it extends”, without its
antecedent. A trial grouping such pronoun-led sentences removed that passage
from the selected context entirely, so that change was discarded. The
strict evidence regex also misses some semantically useful passages, such as
the piston limit and the redstone-tick conversion. Review labels, cached
article revisions, passages, and answers before attributing a miss to ranking.

The workstation's existing llama.cpp b12110 Vulkan server and the same Bonsai
2 4B Q2_0 checkpoint used on the phone were subsequently started as an isolated
local worker with a 1,536-token context. On the original copied cache, the
grounded model matched 77/98 factual and 20/20 private-state patterns. Manual
review found that two factual answers, “Yes” to an elytra repair question and
“Copper” to a copper-golem question, were valid but did not match the labels;
the labels have been corrected, giving 79/98 retrospectively. These scores
include parser fallbacks and do not establish citation entailment.

Refreshing nine crafting articles in the disposable cache and enabling the
production exact-answer route yielded 85/98 factual and 20/20 private-state
pattern matches under the corrected labels; 17 used the exact route. Six
previously failing recipe answers now matched. Both cache contents and answer
route changed, so this is not an isolated causal comparison or proof of current
phone behavior. Other observed problems included choosing an unrelated anvil
passage, reversing a hopper's powered behavior despite the relevant excerpt,
and miscalculating a shulker box's stack capacity. No cache or code was deployed.

A subsequent retriever change normalizes “highest” and “lowest” to the
corresponding limit terms and treats them as numeric questions. On a freshly
rebuilt index of the same refreshed cache, the same benchmark matched 86/98
factual patterns and 20/20 private-state patterns. The only changed answer
was the anvil question, from 64 to 39. Its top passage, however, states the
39-level limit specifically for renaming, so this is not sufficient evidence
for the general question even though the pattern matched. The other labeled
retrieval rates were unchanged. The new index version is rebuilt automatically
from parsed cache files; this does not refresh stale articles.

The synthetic `@find` suite has 96 matched and 20 absent queries, with similar
location names and rotated pin positions within the actual serialized size
limit. The model selected the labeled location in 91/96 matched cases and
returned 0 in all 20 absent cases. Five wrong selections were simple literal
phrases. A conservative bridge shortcut now resolves a unique literal phrase
at the start of a note with a simple location qualifier; ambiguous, relational,
or potentially negated requests still fall back to the model. Replaying the
recorded model outputs with the shortcut gave 116/116 labeled results, with
88 answered directly. This is a correlated synthetic test, not a live gameplay
measurement or a guarantee of semantic matching. Independent natural,
ambiguous, and misleading-note examples remain important.
