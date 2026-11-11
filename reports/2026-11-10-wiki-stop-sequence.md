# Wiki answer truncation and stop sequence, November 10, 2026

The blank-line stop could end a model response before its `Answer:` line.
For one real cached Observer question, the phone stopped after 45.6 seconds
and the bridge abstained, even though the model had generated the correct
supporting fact. Omitting that stop let it generate the cited answer in
48.1 seconds. This is a response-completeness fix, not a faster inference
result. The 140-token generation limit and existing streaming deadlines
remain in effect; a length-limited response is still discarded.

The server excludes the matching stop string from the returned text, so
the partial support line itself does not reveal whether the model intended
another paragraph. [Pinned llama.cpp server documentation](https://github.com/ggml-org/llama.cpp/blob/b12110/tools/server/README.md)

## Desktop paired screen

An isolated llama.cpp b12110 worker ran Bonsai 2 4B Q2_0 on desktop
Vulkan0. The retriever used a private offline copy of the phone's parsed
cache. Each question received the same ranked passages, existing grounded
prompt, greedy sampling, 140-token cap and disabled prompt reuse in both
arms. One arm used the existing blank-line stop; the other omitted it.
Arm order was shuffled with a fixed seed. These are model-only calls: even
questions that the bridge could answer directly were sent to the model.

All **113 pairs had identical returned model text**:
60 repository cases, 44 earlier fresh cases, eight recipe cases, and the
targeted Observer wording from the phone failure. No request hit the
token limit. Median and 95th-percentile output-token counts were the same
for each case set. The first two case sets scored 65/84 factual answers with the
expected source, 52/84 against their cited-evidence patterns, and 20/20
private-state abstentions in both arms. The recipe set scored 3/8 factual
answers. These pattern scores are not an entailment proof; the earlier
fresh cases have also been used in prior research and are not a new
holdout. The desktop screen provides no phone latency measurement.

## Phone comparison

The phone used an isolated b12110 OpenCL worker with the tuned Q2_0 and
activation-texture paths, F16 KV, a 1,536-token context, a 256-token
microbatch and two efficiency-core threads at nice 15. Direct instrumentation
confirmed texture dispatch. Minecraft was running without players. Runs
held the shared GPU lock, used private shader and Wiki caches, disabled
prompt reuse and checked a 1,200 MiB memory floor and 40 °C battery ceiling
before each request. The timer spans HTTP submission through the complete
model reply; retrieval, game chat delivery and startup are excluded. Each
table entry is one request per arm except the bracketing Observer pair.

| Question | Blank-line stop | No stop sequence | Result |
| --- | ---: | ---: | --- |
| Observer signal strength | 45.607 / 45.651 s | 48.112 s | Stop arm omitted `Answer:` and abstained; no-stop arm returned `15 [1]`. |
| Shulker box contents | 53.869 s | 53.897 s | Same correct cited text. |
| Piston pushing 13 blocks | 30.484 s | 30.495 s | Same `No`, without a valid citation; the bridge abstains. |
| Private brewing-stand contents | 42.367 s | 42.371 s | Same correct abstention. |
| Observer recipe and detection | 59.336 s | 59.332 s | Same model text, but see citation limitation below. |

The Observer no-stop answer used 35 generated tokens versus 29 in the
stop arm; the extra decode accounts for approximately 2.5 seconds. The
other four pairs had identical model text and token counts. The phone
results are a small sample and do not exclude longer answers or timeouts
for other questions. Desktop and phone generated different Observer text
despite the same model and evidence, so desktop parity alone did not
establish phone behavior.

In the multi-part case, the model's support line named sources 1 and 2,
but the current bridge attached only source 1 to the answer. That is an
existing citation limitation: the first passage supported ingredients and
the second supported detection. This stop-sequence change does not fix it.

## Implementation and checks

`chat_request_body` no longer supplies a blank-line stop sequence. The
model's normal end-of-generation stop remains required by `generate`, and
the bridge continues to reject length-limited or incomplete streams. A
regression test streams a blank line between support and answer and checks
the cited result. On the phone, all 17 isolated chat tests and all five
prefix tests passed; the changed Python files compiled. The live Wiki
service was not enabled or deployed. Scratch model processes were stopped,
phone scratch was removed, and workstation scratch was moved to the Recycle
Bin. The phone's Minecraft Java process remained running.
