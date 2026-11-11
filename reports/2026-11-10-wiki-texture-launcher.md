# Nord N20 activation texture launcher trial, November 10, 2026

The Android ARM64 llama.cpp b12110 build used the N20 OpenCL, activation
texture, server, and disk-prefix patches. The scratch launcher used the
installed Bonsai 2 4B group-64 Q2_0 checkpoint and the C.24 Adreno 619 driver.
The texture profile kept the tuned image-buffer fallback and used one
64 × 9,216 RGBA FP16 image on the in-order OpenCL queue. The 256-token
microbatch capped its pixel payload at 4.5 MiB. The startup log confirmed
image allocation and the prompt log confirmed texture dispatch.

The private launcher and bridge used separate markers, socket, cache
directories, chat log, and console FIFO. The bridge ran under the
Minecraft account against a private copy of the offline Wiki cache.
The shared GPU lock serialized the runs. The harness monitored a
1,200 MiB available-memory floor and a 40 °C battery temperature ceiling.
The installed Minecraft server ran during the measured requests, after
the separate gameplay trial released the GPU lock and restored service.
The installed Wiki launcher stayed disabled.

The model answered the Observer output-strength question with `15` and
the bridge linked the Observer Behavior section. The harness verified
that the exact-answer path deferred to the model. Reply times run from
the private chat line to the complete `tellraw` command. The warm
launch reached health in 22.93 s and the first reply took 49.41 s,
including 35.73 s of prefill for 352 prompt tokens and 13.59 s of
decode for 34 tokens. This launch started with an empty prefix snapshot
and a shader cache populated by an earlier startup. The cold startup
reached the listener 121.90 s after the server process started. A
procfs assumption in that first harness stopped it before a chat
request. The measured launch used the corrected process check.

On restart, the worker reused the same fingerprint and restored 106
system tokens from its 56,168,292-byte mode-0600 snapshot. It took
22.69 s to reach health and 38.45 s to return the same complete reply,
including 24.88 s of prefill for the remaining 246 tokens and 13.48 s of
decode. The first-token logit hash matched the seed run.

## Failure injection

A nonempty malformed source passed launcher file validation and failed
OpenCL compilation. For the allocation case, a scratch preload shim
returned `CL_MEM_OBJECT_ALLOCATION_FAILURE` only for the texture image.
In both runs the launcher restarted with tuned image buffers. The
running worker had the tuned prefix directory and a different
fingerprint from the texture worker. Neither tuned restart received
the texture source option. Both returned the same complete `15` reply,
Observer link, and first-token logit hash as the texture runs.

| Failure | Launcher to health | Chat to complete reply |
| --- | ---: | ---: |
| Invalid texture source | 30.48 s | 51.31 s |
| Texture allocation rejected | 28.97 s | 51.46 s |

A separate shim returned `CL_OUT_OF_RESOURCES` from the first texture
copy during prefill. The worker logged one failure, disabled the texture
in that context, and completed the original request using tuned image
buffers. It reached health in 22.94 s and replied in 51.55 s. The model
produced the same first-token logit hash and the bridge returned the
same answer and citation. The scratch shim changed the worker identity,
so this request evaluated all 352 prompt tokens.

The launcher passed `sh -n`, all four Python modules compiled, and the
five prefix and eight chat tests passed on ARM64 Debian. The timings
cover one question per arm in sequence with different cache states.
They establish reply and fallback behavior on this checkpoint. They
do not measure player tick latency or long-running service behavior.
