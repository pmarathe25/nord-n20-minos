# Wiki evidence reuse across benchmark questions, November 10, 2026

The phone's installed Rust retriever ran offline against a private copy of
the installed parsed Java Wiki cache. The input combined the existing
44-question and 60-question benchmark sets, deduplicating questions before
retrieval. All 104 distinct questions returned at least one passage.

Each retrieved evidence prefix was compared as an ordered sequence of
passage title, section, and text. Revision and anchor fields were ignored,
which gives an optimistic count for exact prompt-state reuse. All 104
ordered evidence sequences were distinct. No question could reuse a full
evidence prefix created by another question in this sample. The first
passage matched across three pairs of questions. The first two passages
never matched in order across distinct questions.

This result applies to the existing benchmark's diverse questions and the
cache snapshot used in the run. Repeated or similarly phrased live questions
could have different reuse rates. This is an upper bound for a cache that
could keep every encountered prefix in memory. It does not include the
benefit of the existing system-prompt state cache.
