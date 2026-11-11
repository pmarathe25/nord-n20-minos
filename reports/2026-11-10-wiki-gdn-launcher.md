# Nord N20 guarded recurrent profile, November 10, 2026

The launcher has an opt-in two-column Gated DeltaNet prefill profile. It
requires the tuned and activation-texture profiles, a trusted separate
marker, and a backend containing the experimental option. The profile has
separate shader and disk-prefix caches. The backend and marker were not
deployed, and the installed Wiki service remains disabled.

## Message-to-reply measurement

The Nord N20 ran the real chat bridge, its offline cached Wiki retriever,
and a scratch Android ARM64 b12110 worker using the Bonsai 2 4B Q2_0 model
and C.24 Adreno driver. A private log and console FIFO substituted for
Minecraft. The timer starts at a simulated chat log line and ends at the
complete `tellraw` command. It includes retrieval and generation, but
excludes game networking, server tick scheduling, network fetches, and
worker startup. Each row is one request for the same Shulker Box question.
The shared GPU lock serialized the experiment and Minecraft remained up.

| Profile | Prefix | Launcher to health | Chat line to reply |
| --- | --- | ---: | ---: |
| Texture, four columns | New | 10.835 s | 54.238 s |
| Texture, two columns | New | 12.349 s | 51.596 s |
| Texture, four columns | Restored | 10.584 s | 43.403 s |
| Texture, two columns | Restored | 10.332 s | 41.399 s |
| Two-column launch fails, texture fallback | Restored | 16.111 s | 43.285 s |

All five replies said the contents stay in the box and cited the Shulker
Box Storage section. Full first-token logit hashes matched. Logs confirmed
the intended recurrent layout and that each restart restored 106 tokens
from its own snapshot. The test does not establish general answer parity,
thermal behavior, or player tick latency. The earlier
[six-question comparison](2026-11-10-wiki-gdn-columns.md) tests model replies
more broadly but excludes retrieval and bridge delivery.

For the fallback row, a scratch-only launcher returned a failure immediately
before starting the two-column worker. The real supervisor then prepared the
four-column profile's fingerprint, restarted it, and returned the same cited
answer with the same first-token logit hash. This exercises supervisor control
flow; it is not a simulated OpenCL compiler or GPU failure. The prefix helper
tests also covered rejection of a backend without the option and an unsafe
marker, as well as separate fingerprints. All seven helper tests passed on
ARM64 Debian, Python compilation succeeded, and the launcher passed `sh -n`.

## What this means for fusion

In an earlier [instrumented full request](2026-11-10-wiki-attention-dispatch.md),
recorded kernel execution accounted for 38.113 s of a 38.834 s request;
Q2_0 projections accounted for 24.766 s. The difference includes work
other than dispatch and the profiling itself can affect timing, but it
sets expectations for that request: simply eliminating launch gaps has
limited headroom. The earlier fused gate/up/SwiGLU implementation increased
complete reply time. A larger fusion should be evaluated for reduced
memory traffic and arithmetic, and measured from chat input to completed
reply, including any packing or scheduling it introduces.
