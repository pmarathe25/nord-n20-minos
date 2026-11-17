# Retain unavailable Wiki questions — 2026-11-16

Previously a model startup timeout, retrieval/inference failure, or a resource
check that changed after dispatch became a final unavailable reply. Waiting
requests also expired after five minutes. The owner requested that these
questions remain queued until the model can run.

Temporary lookup failures now produce an internal deferred result rather than
an answer. The bridge returns the original request to the front of the queue,
waits at least 30 seconds before another attempt, and sends the requester one
private waiting notice. It does not broadcast an unavailable answer or put it
in answer history. Successful answers and genuine no-answer-found results are
still final. Semantic map searches share this behavior; direct map reads that
temporarily fail also retain their queued request.

Removed age expiry for accepted requests, including restored handoffs. Capacity
remains bounded to 16 outstanding questions / 8 KiB of UTF-8 question text and
two requests per player. An active request reserves its slot and byte budget,
so a failed attempt can requeue even when other players fill the remaining
capacity. Admission throttling and rejection limits remain. Retries preserve
arrival order, and work belonging to a previous Minecraft process is discarded.

The queue remains in bridge memory; ordinary bridge/server restarts still end
that session's pending work. Existing explicit handoffs retain their validated
queue. This change does not add crash-persistent request storage.

## Validation

Passed individually on ARM64 Debian:

- Five new retry tests, including an isolated real bridge loop with a private
  FIFO: repeated outage attempts retained one question, emitted one waiting
  notice, wrote no premature history, and delivered/recorded exactly once after
  recovery. No test message was sent to the production server.
- Slot/byte reservations at full capacity, no age expiry, delayed retry,
  server-identity changes, transient resource/retrieval/model failures, and a
  genuine unknown answer were checked.
- Wiki chat: 31 tests; model demand lifecycle: 4; map chat: 82; admission bounds.
- Python compilation and `git diff --check`.

Deployment checked that the installed bridge matched the original repository
blob and that the fresh Wiki status had no active or queued requests. Only Wiki
was reloaded. The new bridge returned with resources available and an empty
queue; the model was off as expected until demand. The installed module matched
the tested source. Minecraft remained online with two players and Java PID
8600 throughout; no Minecraft restart or live pressure test was performed.

A root-only copy of the previous public bridge module is retained under
`/var/lib/n20/wiki-retry-rollback-20261117T023654Z` (UTC; November 16 local time).
Temporary sources, helpers and archives were removed. World data, credentials,
backup safeguards and the memory/swap policy were unchanged.
