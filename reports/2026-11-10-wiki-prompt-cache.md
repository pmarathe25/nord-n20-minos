# Bonsai prompt cache memory on the Nord N20, November 10, 2026

The chat bridge has a 64-entry cache for sourced answers. It reruns retrieval
and checks the question, excerpts, and revisions before returning a saved
reply. The model server has two separate kinds of prompt reuse. Its active
slot can keep matching leading tokens. Its global RAM cache saves older
serialized prompt states when a different request replaces much of a slot.
In llama.cpp b12110 the global cache defaults to 8,192 MiB. The launcher
uses one slot and disables automatic memory fitting. The
[104-question evidence comparison](2026-11-10-wiki-evidence-reuse.md)
found zero pairs of distinct questions with an identical full evidence prefix.

## Desktop control

The comparison used Bonsai 2 4B Q2_0, Vulkan, llama.cpp b12110, a
1,536-token context and one slot. Both modes received the same cached
excerpts and chat request bodies. The order was 12 new questions, an
immediate repeat, two older repeats, an idle unload, a repeated question
after wake and one more question. All 17 generated texts matched byte
for byte.

| Sequence | Global cache default | Global cache disabled |
| --- | ---: | ---: |
| Median new request | 287.45 ms | 262.96 ms |
| Median cached tokens on new requests | 106 | 106 |
| Median evaluated prompt tokens on new requests | 229.5 | 229.5 |
| Evaluated prompt tokens on immediate repeat | 4 | 4 |
| Evaluated prompt tokens on the two older repeats | 4, 4 | 214, 407 |
| Evaluated prompt tokens after idle wake | 513 | 513 |
| Peak process RSS across the run | 3,667 MiB | 1,718 MiB |
| Process RSS asleep | 2,233 MiB | 440 MiB |

Times include the local HTTP request and complete reply. Both modes
reused the active system prefix on the first new question after the wake
request. The default cache kept older states in RAM while the model
slept. In a separate desktop screen of the 104-question benchmark with a
2,048-token context, the default cache reached 9.70 GiB peak process RSS.
The log showed evicted entries around 159–168 MiB each. Desktop memory
includes the process's own allocations and does not measure device-wide
GPU residency.

## Phone A/B

The phone ran the installed b12110 Adreno OpenCL worker with one slot,
a 1,536-token context and two efficiency-core threads at nice 15. Each
process used a private socket and shader-cache copy. Both arms used the
same grounded chat prompts built from offline Rust retrieval and a
12-second idle threshold. The requests went directly to the model so
the chat bridge could not serve the repeated question. The default
ran first. The monitored run stopped if available system memory dropped
below 1,200 MiB or the battery reached 40 °C.

| Request or metric | Global cache default | Global cache disabled |
| --- | ---: | ---: |
| New respawn anchor question, j07 | 47.817 s | 47.786 s |
| New shield question, g03 | 42.344 s | 42.123 s |
| New sponge question, f13 | 32.010 s | 31.786 s |
| Respawn anchor repeat after two other questions | 10.512 s | 31.613 s |
| Respawn anchor repeat after idle unload | 61.556 s | 58.778 s |
| Peak process RSS | 2,753.5 MiB | 2,284.6 MiB |
| Process RSS asleep | 1,017.5 MiB | 706.2 MiB |
| Lowest system MemAvailable | 1,646.7 MiB | 2,027.2 MiB |
| Peak battery temperature | 27.3 °C | 27.7 °C |

All five generated texts matched across modes. The model correctly
cited glowstone. It stopped before the shield answer line and gave
only “Wet” for the sponge variant, where the existing scorer expects
“Wet sponge.” Those outcomes occurred with both cache settings.

Both modes evaluated 247, 144 and 155 prompt tokens for the three new
questions and reused 106 system tokens on the second and third.
The direct older repeat evaluated 4 tokens with the global cache and
141 without it. On wake, both evaluated all 247 tokens. Model reload
varied between the two single wake samples. The three new-question
times were within 0.23 seconds per pair. These are request-to-complete
response times and exclude retrieval and Minecraft networking.

Disabling the global cache saved 469 MiB of peak process RSS and
311 MiB while asleep in this bounded run. The checkout launcher uses
`--cache-ram 0`. The bridge can serve the successful older repeat from
its existing reply cache if the retrieved evidence still matches. An
older model request that misses the reply cache can pay the extra
prefill measured above. The test did not install the launcher or
enable the bridge on the live server.

## Evidence trimming screen

The same 104-question desktop sets also tested a shorter header format
that preserved source numbers and omitted exact duplicate passage text.
A separate question-aware sentence filter kept all four passage numbers.
It retained sentences within
0.75 points of the best word-overlap score in each passage, favored
number-bearing sentences for quantity questions, and kept the preceding
sentence when a retained sentence began with an anaphor or transition.

| Evidence | 44-case factual answers | 60-case factual answers | Private abstentions | Median prompt tokens |
| --- | ---: | ---: | ---: | ---: |
| Original | 29/36 | 38/48 | 20/20 | 313.5 |
| Compact headers | 28/36 | 37/48 | 19/20 | 305.5 |
| Filtered | 29/36 | 37/48 | 20/20 | 300.5 |

The count of correct answers citing a benchmark-labeled title went from
63 to 61 with either variant. The median generation length stayed at
34 tokens. The desktop screening timings overlapped another GPU workload.
Both variants reduced prompt length at the cost of answer and citation
regressions.
