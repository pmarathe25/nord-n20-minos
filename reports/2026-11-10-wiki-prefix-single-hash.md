# Texture and prefix startup hashing, November 10, 2026

Texture validation and disk-prefix fingerprinting previously hashed the
1.2 GB checkpoint in separate helper processes. They now share one
verified SHA-256 digest within a single helper invocation. On a warm-cache
Nord N20 trial, launch to a complete model reply fell from 56.188–56.484 s
in bracketing controls to 53.765 s. The change saves startup work; it does
not accelerate prefill or decoding once the worker is ready.

## Implementation

The texture helper still verifies the complete model hash against the
tested checkpoint and checks the texture backend and source. It records the
opened model's device, inode, size, modification time and change time
before and after hashing. Prefix fingerprinting reopens the trusted model,
checks the same identity and incorporates the verified 32-byte digest in
the original fingerprint encoding. The digest stays inside the helper
process; it is not accepted from an environment variable or command line.
The snapshot key format and the exact worker launch vector are unchanged.

If texture validation fails, the launcher keeps the tuned profile. If
disk-prefix validation or cache preparation fails after texture validation,
the helper revalidates the texture checkpoint and the launcher starts the
texture worker without prefix caching. Tuned and stock profiles retain
their existing separate prefix preparation. Identity checks detect ordinary
changes during preparation; as before, the runtime and its parent paths
must remain root-controlled.

## Phone comparison

The Nord N20 used a scratch copy of the launcher and a fresh Android ARM64
b12110 worker with the N20 OpenCL, activation-texture, server, disk-prefix
and lazy Flash Attention-cache patches. The installed checkpoint was
hard-linked read-only into the root-controlled scratch bundle. Both arms
used the C.24 Adreno driver, a 1,536-token context, a 256-token microbatch,
one slot, two efficiency-core threads at nice 15, the same private shader
cache and the same private disk snapshot. The shared GPU lock serialized
the runs while the Minecraft server remained online without players.

The old-path control used a scratch wrapper that performs the former
texture check and prefix preparation as two separate Python subprocesses.
That wrapper adds a small process-start cost, so the difference is an
estimate of the launcher improvement rather than an exact measurement of
the previously installed script. The test used the current grounded prompt
and real cached Shulker Box excerpts; it called the model directly so that
the result was not served by the exact-answer or reply caches. The clock
starts at launcher process creation and ends when the complete HTTP reply
is received. It excludes retrieval and Minecraft networking. A seed run
created the snapshot; one new-path run was bracketed by old-path runs.

| Arm | Launch to health | Health to complete reply | Launch to complete reply |
| --- | ---: | ---: | ---: |
| Old path, before | 13.039 s | 43.446 s | 56.484 s |
| Single hash | 10.534 s | 43.231 s | 53.765 s |
| Old path, after | 13.037 s | 43.151 s | 56.188 s |

Each measured run restored 106 system tokens and evaluated 309 remaining
prompt tokens. All returned the same cited answer, the same finish reason,
and the same SHA-256 of all first-token vocabulary logits. The snapshot
file hash stayed unchanged from the seed through all three runs. The
sampled minimum available memory was at least 2,322,024 KiB and maximum
battery temperature was 25.0 °C. These are one request per arm, not a
latency distribution or a gameplay test.

Six alternating direct hash probes on the phone took 2.046–2.107 s per
full model hash in Python and 2.047–2.055 s via `sha256sum`, all matching
the known checkpoint digest. This supports the duplicate-read explanation;
filesystem cache and load can change the saving on other starts.

## Fallbacks and tests

With the scratch prefix directory deliberately made non-private, the
launcher reached health with texture enabled and no prefix key. With the
scratch texture source temporarily absent, it reached health in tuned mode
without a texture source. The scratch inputs were restored afterward.
All six prefix tests passed on ARM64 Debian, including identical old/new
keys, one model hash per successful combined preparation, model-change
rejection and disk-only failure fallback. The changed Python files compiled
and the launcher passed `sh -n`. No live Wiki configuration, server, firmware
or world data was changed. Phone scratch was removed, workstation scratch was
moved to the Recycle Bin, and Minecraft's Java process remained running.
