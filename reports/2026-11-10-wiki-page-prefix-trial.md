# Wiki page-prefix and retrieval trial, November 10, 2026

The workstation used llama.cpp b12110, Bonsai 2 4B Q2_0, Vulkan and a
4,096-token context. The retrievers ran against a scratch copy of the phone's
108 parsed cache entries; no live phone services were changed. Six recent
questions were added to `wiki/benchmark-recent-chat.json`. Their expected
evidence appeared for 1/6 questions before the ranking change and 6/6 after.
With the existing chat prompt and pattern scorer, the model answered 1/6 before
and 6/6 after. The latter score accepts an answer saying the trim can be applied
to armor even if it does not explicitly mention decoration. Model outputs vary
across environments even with temperature zero; the scores are not guarantees.
The 36 labeled factual fresh cases had evidence hits 32/36 before and 33/36
after, and the 48 labeled development cases had 33/48 and 34/48. The eight
recipe cases had no matching evidence in this particular copied cache. Evidence
patterns do not prove that a response is correct or its citation supports it.

For an exploratory whole-page comparison, the model received all default parsed
sections of the first *expected* article for each question. This uses answer-key
page selection, an optimistic oracle that is unavailable in production. On the
six recent questions, all six answers matched the patterns. On the 36 factual
fresh questions, one page did not fit after reserving 400 tokens; 28 of the
other 35 answers matched. Ranked excerpts matched 29 of those same 35, and
30/36 overall; the ranked run also matched 8/8 private-state abstentions. The
whole-page trial did not test private-state abstentions, page selection, live
Wiki revisions, phone latency, or citations to precise sections. The runs were
not randomized and these counts are too small to establish a general winner.

The 108 entries had 651,487 bytes of default section text in total (median
4,363 bytes), excluding JSON structure. Using the Bonsai tokenizer on that
text, the median was 1,106.5 tokens and the longest was 9,201. Six of 108 were
too long for a 4,096-token context when allowing about 120 prompt tokens and
512 reserved tokens. This existing cache is a convenience sample, not a random
sample of the entire Wiki. The parser excludes common page furniture, history,
gallery and sounds by default, and recognizes several edition annotations;
the sampled dye crafting tables still contained some Education-only items.

A dedicated one-slot workstation server generated and restored three complete
page-prefix states with `wiki/build_page_cache.py`. The Coast Armor Trim state
used 117,583,496 bytes for 372 tokens; Green Dye used 192,641,956 bytes for
1,054 tokens; Lime Dye used 191,789,260 bytes for 1,028 tokens. After restore,
the server reported all prefix tokens reused and produced exactly the same text
as a cold run for one question per page. These are server slot files for this
specific build and 4,096-token configuration, not phone state sizes, and include
recurrent state as well as attention state. No claim of cross-device snapshot
compatibility follows. For scale only, 18,277 files at these measured sizes
would be roughly 2.15–3.52 TB decimal, before metadata or extra variants; the
Wiki's article count is not a count of suitable unique Java pages and the three
examples do not establish a population average. Rebuilding after updates, model
changes or runtime changes adds compute and storage turnover. Storing fewer,
popular page states and using ranked excerpts for other pages could be tested.
