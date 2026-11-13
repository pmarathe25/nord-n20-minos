# Wiki and saved-location evaluation, November 12, 2026

These are desktop checks using the existing Bonsai 2 4B Q2_0 checkpoint,
llama.cpp b12110 Vulkan, one slot and a 1,536-token context. The Wiki runs
used the same disposable parsed cache as the earlier refreshed-cache test.
They do not measure the running phone, and no changes were deployed.

The earlier 116 `@find` examples were mostly literal variations and did not
adequately exercise uncertainty. The new 30-case synthetic challenge includes
semantically worded requests, similar locations, ambiguous terms and notes
describing a route or a missing location. Before this change the full bridge
matched 12/14 desired locations and declined 3/16 cases labeled no match or
ambiguous. The model alone had the same scores. One literal shortcut selected
an iron farm whose note said it was gone.

The bridge now declines a simple literal request appearing in multiple notes,
and declines narrowly recognized route-only or destroyed-location notes. The
model prompt also explicitly requests 0 for ambiguous requests. With these
changes together, the same 30 cases matched 12/14 desired locations and
declined 15/16 no-match or ambiguous cases. The remaining mistakes were a
request for a working farm when the note said it was under construction, and
two semantic matches the model declined. These labels reflect the fixture's
intended meanings; real notes can be more ambiguous. The model may still choose
the wrong pin, including when a note's wording does not fit these checks. The
original 116-case set scored 96/96 matches and 20/20 no-match results with the
bridge. These repeated templates are correlated, not independent samples.

For Wiki retrieval, questions containing the word “powered” or “powering” now
add “redstone signal” to the search terms. “Powered” is also normalized to
“power” in indexed text; “unpowered” is not treated as powered. The index
version changed, so the retriever rebuilds it from cached parsed pages without
refreshing those pages. The powered-hopper question previously returned an
incorrect “Yes” despite including a locked-hopper passage; the revised
retrieval placed the passage connecting a signal to a locked hopper first and
the model answered “No.” On the earlier 118-question set, pattern matches
changed from 86/98 to 87/98 factual questions, with 20/20 private-state
abstention patterns in both runs. This was the only changed response. That
comparison preceded the additional “powering” query expansion; none of the
118 questions contain that word.

An eight-question focused hopper set then tested powering, powered collection,
receiving items, unlocking, and comparator behavior. It matched 7/8 answer
patterns before the retrieval changes and 8/8 afterward. These examples were
written to investigate this particular gap and are not an independent general
accuracy estimate; regex matches are not evidence that every citation entails
the answer. The earlier benchmark's weakly supported anvil answer remains.
