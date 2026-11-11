# Wiki grounding and response-format screen, November 10, 2026

The exact-answer route now checks the full subject of a property sentence.
The old suffix match could cite a soul torch sentence for a normal torch
question if that sentence appeared under the parent Torch article. A focused
fixture reproduces the substitution. With both torch variants in the fixture,
the updated route cites the ordinary torch passage. It still accepts a plural
subject and preserves the condition in an observer pulse sentence.

On 130 retrieved question and passage sets, the exact route selected the same
eight answers before and after the change. All eight isolated chat tests
passed on the phone. Python compilation checks passed on the workstation.

## Desktop model screen

The comparison combined the 104 repository questions with 26 new questions
covering similar factual and private-state requests. There were 95 factual
questions and 35 private-state questions. The fresh set paired topics such
as hopper transfers with its present inventory, a new shield's durability
with the durability left in an offhand shield, and respawn anchor charging
with the charge left after a player's last death.

Desktop llama.cpp b12110 ran Bonsai 2 4B Q2_0 on Vulkan0 with six CPU threads,
one slot, a 2,048-token context, temperature zero, thinking disabled, a
140-token output cap and a blank-line stop. Global and per-request prompt
reuse were disabled. The offline Rust retriever supplied the same ranked
excerpts for all variants in each screen. Variant and question order were
interleaved using a fixed seed. Scoring used the bridge's reply renderer
and eight exact answers where applicable. Every request still ran inference
for the timing comparison. Median times cover local HTTP requests and exclude
retrieval, chat transport, and model loading.

A factual pass required the expected answer patterns and a rendered link to
an expected article. The cited-evidence column additionally checks the
existing evidence patterns against that specific cited passage, for the 84
repository facts with evidence labels. These patterns can miss valid
paraphrases and do not establish entailment. A private-state pass required
the standard abstention.

| Phone cache copy, second screen | Factual with expected title | Cited evidence | Private | Median input/output tokens | Median request |
| --- | ---: | ---: | ---: | ---: | ---: |
| Existing grounded prompt | 73/95 | 52/84 | 33/35 | 311 / 34 | 304 ms |
| Explicit actual state and supplied inputs | 72/95 | 51/84 | 32/35 | 330 / 32.5 | 298 ms |
| Two-line format grammar | 72/95 | 52/84 | 31/35 | 313 / 32 | 298 ms |
| One-line answer and citation grammar | 65/95 | 47/84 | 33/35 | 297 / 9 | 184 ms |

The one-line grammar forced a valid source number and cut median output by
25 tokens. It lost seven repository factual passes and one fresh factual
pass in aggregate. For the existing prompt, 65 of 84 repository facts and
eight of 11 fresh facts passed the expected-title score. All 20 repository
private-state questions passed. Two fresh cases received answers about a
particular event. The first asked about a specific observer pulse and got
the fixed duration, which the cited passage supports as a general rule.
The second asked for the exact charge after the player's last death. It got
the change in charge while the starting charge and whether the player
respawned there were unknown. The latter is a confirmed failure to abstain.

| Earlier cache archive, first screen | Factual with expected title | Cited evidence | Private | Median input/output tokens | Median request |
| --- | ---: | ---: | ---: | ---: | ---: |
| Existing grounded prompt | 71/95 | 50/84 | 33/35 | 311 / 34 | 330 ms |
| Compact support instruction | 71/95 | 46/84 | 30/35 | 320 / 20 | 276 ms |
| Exact support quotation requested | 72/95 | 52/84 | 29/35 | 303 / 40.5 | 361 ms |
| One-line answer without grammar | 19/95 | 15/84 | 34/35 | 297 / 11 | 179 ms |

The quote prompt produced a short verbatim match in 83 of 92 cited factual
replies, compared with 51 of 90 for the original prompt in the first screen.
It still answered private questions with values from plausible but inapplicable
passages. Quoting a real sentence establishes that the sentence exists.
It can still apply to a different condition or refer to a generic item property.
Manual review also found a baseline answer of zero remaining respawn charges
cited to a passage about the anchor's light levels. The passage did not say
whether respawning uses a charge.

An isolated shortcut for explicit questions about who performed a completed
world action matched five of 35 private cases, four of six separate private
examples, and zero of 95 factual cases or ten separate near matches. The
baseline already abstained correctly on those five, and the shortcut did not
cover the remaining-charge failure. The bridge keeps its existing request
and response format. No phone inference comparison or live deployment ran
for this screen.
