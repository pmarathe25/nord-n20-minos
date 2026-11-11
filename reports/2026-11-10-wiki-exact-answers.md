# Wiki exact-answer expansion, November 10, 2026

The chat bridge can answer more questions directly from its ranked Wiki
excerpts. The same citation renderer links each answer to its article and
section. The new paths cover complete crafting ingredient lists, selected
Overview fields, storage slot counts, and whole-item yields from the Smelting
fuel table. The bridge sends other questions to the model.

The complete ingredient route requires an unambiguous counted crafting row
on the item's article. It checks that the declared ingredient names agree
with the counted slots and retains the row's notes. The dispenser answer, for
example, includes the note about the bow's durability and enchantments.
The bookshelf's row gives only the fixed book count, so a request for the
full ingredient list still falls through. Overview answers retain the entire
field, including the anvil's same-damage-state stacking condition and the
beehive's exception for occupied hives. The storage route quotes a direct
capacity sentence with the requested container as subject. The fuel route
checks the per-fuel column for the exact material and defers on fractional
values, qualified rows, or a conflict with matching fuel prose.

The replay used the stock Windows Rust retriever in offline batch mode with a
scratch copy of the 97-article parsed phone cache archive plus ten previously
refreshed recipe entries. The existing 112 questions contain 92 factual and
20 private-state cases. Scoring checked the expected fact patterns and the
cited article. The new hits also received a manual review against their cited
excerpts.

| Question set | Questions | Previous fast hits | Expanded fast hits |
| --- | ---: | ---: | ---: |
| Main repository set | 60 | 0 | 1 |
| Follow-up repository set | 44 | 8 | 9 |
| Recipe counts | 8 | 8 | 8 |
| Earlier parser and reasoning suites | 104 | 1 | 1 |
| Factual probes | 50 | 0 | 35 |
| Adversarial probes | 30 | 0 | 0 |

The two new repository answers give five slots for a hopper and 27 slots
for an ender chest. All 16 previous exact answers stayed the same. The route
deferred all 20 repository private-state questions. All 35 probe hits had
the expected fact and cited article. The factual probes were written to
exercise the candidate question forms, so their hit rate is a branch-coverage
measurement. The adversarial probes included inventory and world state,
recipe variants and restrictions, item quantities, and edition differences.

The Windows replay measured about 2 ms median for warm batch retrieval and
JSON parsing. Median Python matching took about 5 µs for a hit and 2 µs for a
miss. A direct answer skips the model request when the reply cache misses.
The phone ran the 15 chat tests against isolated copies under `/tmp`. This
trial made no live configuration changes or GPU inference measurements.

## Ingredient identity follow-up

The same cache replay exposed a material substitution. Asked how many Stone
blocks a blast furnace needs, the old shortcut supplied the count for Smooth
Stone, a separate item. Ingredient count questions now require the full
material name, allowing simple plurals. The Smooth Stone question still
receives the direct count. The Stone question goes to the model. The route
also retains recipe notes and defers on edition-marked or conflicting rows.

| Question set | Questions | Before the ingredient fix | After |
| --- | ---: | ---: | ---: |
| Main and follow-up repository sets | 104 | 10 | 9 |
| Repository recipe counts | 8 | 8 | 8 |
| Fresh open-topic questions | 40 | 7 | 6 |
| Fresh wording and qualification probes | 40 | 15 | 11 |

The repository case about hanging-sign chains now goes to the model because
the passage names Iron Chain. The open-topic change was a question about
quartz in an observer recipe. The other probe changes used generic shells,
dust, and ingots. Manual review found that all 17 retained fresh direct
answers were supported by their cited excerpt. Both fresh sets were authored
for this replay. These hit rates describe the samples and do not estimate
live traffic coverage or score the model's fallback answers.

On Windows, the median offline batch lookup including JSON decoding was
2.0 ms across the 192 questions. With the matcher warmed and timed separately,
median matching was 8.7 µs for a hit and 2.3 µs for a miss. The replay used
the previously archived parsed pages. It did not check for subsequent Wiki
edits. The bridge test verified that revised evidence bypasses a previously
cached answer. All 16 chat tests passed on the ARM64 phone using files copied
to a disposable directory. This follow-up made no live configuration changes
or GPU inference measurements.
