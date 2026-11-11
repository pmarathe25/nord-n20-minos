# Wiki chat streaming deadline trial, November 10, 2026

The [gameplay trial](2026-11-10-wiki-gameplay.md) reached its 75-second
client timeout after generating 124 tokens. The non-streamed HTTP reply
exposed neither those tokens nor the worker's progress to the bridge.
The [format screen](2026-11-10-wiki-reply-timeout.md) found that prompts
which materially shortened replies lost correct details.

The bridge now requests streaming with the same grounding prompt, 140-token
generation cap, temperature, and blank-line stop. It allows 75 seconds to the
first text, then 15 seconds between content events, with a 105-second model
deadline. HTTP headers, keepalives and metadata cannot renew it. The separate
retriever has a 20-second timeout. The bridge requires a terminal normal stop
and the end of the stream before parsing and rendering the answer. It discards
partial output on timeouts or malformed streams and keeps the existing
abstention for a generation that reaches its token cap. The watchdog shuts
down the Unix socket on expiry, including when a worker keeps sending bytes.

## Desktop parity

The comparison used llama.cpp b12110, Bonsai 2 4B group-64 Q2_0, Vulkan0,
six CPU threads and a 2,048-token context. Prompt reuse was off for both
arms. For each question, both requests received the same ranked passages
from an offline copy of the phone's parsed Wiki cache. The 124 pairs
covered the two standard question sets, eight recipe counts and twelve
longer questions about recipes, item behavior, or two related mechanics.

All 124 pairs had identical model text, normal finish reasons and rendered
components. The standard sets gave 65/84 factual passes and 20/20 private
state abstentions in both arms. The separate recipe counts scored 3/8.
Factual passes required the expected answer patterns, no rejected pattern,
and a source link to an expected Wiki page. The eight requests handled by
the exact-answer path were scored with the rendered exact answer. The
twelve longer prompts generated 36–107 tokens and were checked for exact
parity. A follow-up screen of 12 additional recipe and multi-part mechanics
questions also matched text, finish reason and rendering in every pair.
These comparisons preserve measured behavior, including existing wrong
answers.

Eleven chat tests passed on ARM64 Debian. The socket tests covered a
complete reply after the initial deadline, first-text and decode stalls,
ongoing output that reached the absolute deadline, malformed and oversized
streams, and a normal stream ending at the generation cap.

## Phone comparison

The Nord N20 ran the installed Android ARM64 b12110 OpenCL worker,
Bonsai 2 4B group-64 Q2_0 and the C.24 Adreno 619 driver on a private
Unix socket. The stock worker used two efficiency-core threads at nice
15, one 1,536-token slot and private shader and retrieval cache copies.
Minecraft stayed running. The shared GPU lock serialized the experiment.

Both requests asked if the contents of a Shulker Box spill when it is
broken normally. They used the same four ranked passages and disabled
prompt reuse. The old non-streamed client timed out at 75.02 seconds.
The worker still held 440 tokens in its slot, including the 415-token
prompt. With streaming, a private chat log and the real bridge running
as `minecraft` produced a complete `tellraw` command in 79.03 seconds.
It said that the contents stay in the box and linked Shulker Box Storage.
The worker spent 63.63 seconds in prefill and 14.76 seconds generating 30
tokens.

A separate request lowered only the decode-idle budget to 80 ms to force a
disconnect on the real worker. First text arrived after 45.70 seconds. The
client closed the stream 80.78 ms later without returning the partial text.
The server released its slot 0.51 seconds after logging cancellation and
started the queued follow-up 1.4 ms after releasing it. The follow-up
returned signal strength `15` and the Observer Behavior link in 71.71
seconds. The minimum available memory was 1,968 MiB and the highest
battery temperature was 26.4 °C. The installed Wiki enable marker stayed
absent.

Streaming let this generation finish past the original cutoff. The
comparison measures recovery of a complete reply. It does not measure
faster model inference or reproduce the original lightning-rod prompt
under player movement.

The isolated worker stopped after the tests. Phone scratch files were
removed and workstation scratch files were sent to the Recycle Bin.
