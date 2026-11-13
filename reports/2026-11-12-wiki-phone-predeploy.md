# Phone Wiki predeployment check, November 12, 2026

With zero players online, an isolated chat bridge used a temporary log, FIFO,
pin file, and a copy of the phone's Wiki cache. It used the existing model
socket and an ARM64 build of the candidate retriever; no world files were used.
The observer pulse exact-answer question returned a cited correct reply in
0.05 seconds. A powered-hopper model question hit the existing 105-second total
deadline and returned unavailable. A second trial with a 180-second total
deadline finished in 70.39 seconds but answered incorrectly, despite citing the
relevant Hopper section. These two runs do not establish that increasing the
deadline improves reliability, so the production deadline remains unchanged.

The bridge now answers a narrow form of the powered-hopper push question
directly, but only when both the signal-to-lock rule and the no-push rule are
present in passages from the same Hopper section and revision. It defers other
questions, including receiving items, missing evidence, different revisions,
and personal server state. The same isolated phone test then returned the
correct cited answer in 0.034 seconds. Replaying the desktop suite's existing
passages showed that this new exact path applied only to that one question.
The fast route depends on current parsed passages and may fall back if the
Wiki's wording changes; it does not fix general model-backed answer errors.
