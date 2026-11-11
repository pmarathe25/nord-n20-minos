# Redstone signal wording in the Wiki fast path, November 10, 2026

The existing evidence-backed signal-strength route did not recognize
“What redstone signal strength does an observer output?” because of the
word `redstone`. It now accepts that qualifier while retaining the existing
exact article, subject, conflict and sentence checks. The returned sentence
preserves the condition that the observer has detected something and links
to Observer Behavior. Unmatched or ambiguous questions still use the model.

## Phone check

The Nord N20 ran the checkout's Python bridge as the Minecraft account,
with the installed Rust retriever and a private offline copy of the phone's
parsed Wiki cache. A private fake chat log, live test PID and FIFO exercised
the actual bridge's log reader, retrieval, matcher, citation renderer and
console write. The model socket did not exist. Five independent bridge
processes received the same question with fresh in-memory reply caches.
The interval starts when the private chat line is appended and ends when
the complete `tellraw` command is read from the FIFO.

| Sample | Chat line to console command |
| --- | ---: |
| 1 | 40.623 ms |
| 2 | 40.071 ms |
| 3 | 40.006 ms |
| 4 | 40.455 ms |
| 5 | 40.061 ms |

The median was **40.071 ms**. Every command included the full conditional
sentence, signal strength 15, and the Observer Behavior source link. This
does not include actual Minecraft networking or ticks, an online cache miss,
or a running model. The prior [phone model-only request](2026-11-10-wiki-stop-sequence.md)
for the same question took 48.112 s with prompt reuse disabled; its timer
starts at HTTP submission, so the two clocks cover different boundaries.
The comparison illustrates the cost avoided for this matched question,
not the latency of every Wiki question.

## Regression and boundary checks

An offline on-phone replay compared the prior matcher with the new matcher
on 112 existing benchmark questions and 14 additional wording and boundary
probes, using the copied cache. The prior matcher answered 10 of the 126
directly and the new matcher answered 13. The only changes were three
redstone-worded observer questions, all returning the same cited conditional
sentence. The probe set was designed for this change and does not estimate
real-world coverage.

The replay deferred questions about the observer at a player's base, its
current output, a different or powered observer, a side input, an owned
observer, and moving it. It also deferred the sampled comparator, torch and
repeater questions where the selected passages did not establish a unique
matching sentence. Unit fixtures independently verify conflicting values,
exact subject matching and retention of the condition. All 18 chat tests
and all five prefix tests passed on ARM64 Debian, and the changed Python
files compiled. This checks the copied cache at its existing revisions;
it is not an online freshness test or a general correctness guarantee.

No live Wiki worker, server configuration, firmware or world data was
changed. The service remains disabled on the live Minecraft server. Phone
scratch was removed, workstation scratch was moved to the Recycle Bin,
and the Minecraft Java process remained running.
