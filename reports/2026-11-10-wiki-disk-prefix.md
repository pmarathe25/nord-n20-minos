# Nord N20 disk-backed Bonsai system prefix, November 10, 2026

The Android ARM64 trial used llama.cpp b12110, the existing Bonsai 2 4B Q2_0
checkpoint and C.24 Adreno 619 OpenCL driver. It used one 1,536-token slot,
two efficiency-core threads at nice 15, the tuned Q2_0 prefill and decode
paths, and single-turn Wiki batching. The global prompt cache was disabled.
The live Minecraft server stayed online. The model used a private socket
and copies of the Wiki and shader caches.

The experimental patch extends the in-process checkpoint implementation.
The one snapshot contains only the fixed system instruction's exact
rendered token IDs and its complete attention and recurrent sequence
state. The worker uses it only in an empty, single-slot text context and
keeps the 64 MiB state limit. It checks
the supplied runtime identity, token IDs, length and SHA-256 integrity
before calling the sequence restore API. It checks the resulting sequence
end position before adopting the tokens. A failed restore clears the
slot sequence before ordinary prefill.
[Disk patch](../wiki/experiments/llama-b12110-n20-disk-prefix.patch)
[Base patch](../wiki/experiments/llama-b12110-n20-server.patch)

Apply the disk patch after the server patch in a scratch b12110 tree.
Build `llama-server` and use `N20_KEEP_SYSTEM_PREFIX=1`,
`N20_PREFIX_DIR=/absolute/private/directory`, and `N20_PREFIX_KEY` set
to a 64-digit lowercase SHA-256 identity. The directory needs mode 0700
and ownership by the model worker. Its parents and the model inputs need
to be trusted. Derive the identity from the verified GGUF, executable,
libraries, kernel sources, and all model, context and backend options
that affect the state, including the driver and firmware revision.
Generate it again if any of those inputs change.
The experiment calculated this identity once before the timed arms. The
cache file has mode 0600 and is written to a temporary file in the same
directory before atomic replacement. Disk mode releases its host
serialization buffer after saving or restoring.

The request used the unchanged chat prompt, offline Wiki retrieval,
temperature zero and the ordinary blank-line stop. The timings run
from the HTTP request to the complete reply and exclude retrieval.
With `N20_HASH_FIRST_LOGITS=1`, the experiment logs a SHA-256 digest of
the first output's entire vocabulary logit buffer before greedy
sampling.

## Complete replies and restore timing

The two prompts asked for the observer's pulse duration in game ticks
(`j10`) and redstone output strength (`f02`). The worker returned two
game ticks and strength 15 respectively. Both cited the Observer article.
The idle threshold was five seconds and each wake request waited for the
worker to confirm it was asleep. Times include any reload triggered by the
request. For new processes the request followed a successful health check.

| Scenario | Case | Evaluated prompt tokens | HTTP request to complete reply |
| --- | --- | ---: | ---: |
| New process, seed the empty cache | `j10` | 276 | 35.170 s |
| New process, restore the disk snapshot | `j10` | 170 | 23.707 s |
| Idle wake with disk snapshot | `j10` | 170 | 30.907 s |
| Different question in active slot | `f02` | 246 | 39.270 s |
| Immediate repeat in active slot | `f02` | 246 | 39.251 s |
| New process, restore the other question's saved system prefix | `f02` | 246 | 39.904 s |
| Idle wake with empty disk cache | `f02` | 352 | 58.182 s |
| Idle wake with disk snapshot | `f02` | 246 | 47.013 s |
| Idle wake with RAM snapshot | `f02` | 246 | 46.013 s |

Disk restore cut the matched `f02` wake by 11.169 s (19.2%) and saved
11.463 s after health on the matched `j10` cold process. Including process
launch to health, the cold pair took 53.377 s with an empty cache and
42.061 s with disk. Each mode
produced the same raw generated text, stop reason and citation as its
fresh control. The SHA-256 digest of all 248,320 first-output vocabulary
logits also matched for each prompt. The revised worker skipped duplicate
snapshot writes on warm requests and after successful disk restores. The
initial prototype rewrote the file on warm requests, adding about 1.2 s
in each of the two warm comparisons.

The 106-token snapshot was 56,168,292 bytes on disk, including 56,167,716
bytes of sequence state. It took 12.24–12.31 s from task launch to a saved
snapshot, including evaluation of the prefix, serialization and the
durable write. Validated restore took 0.94–0.95 s from task launch to
the restore log. The matched wake reloaded model and context in about
7.1 s. Cold process launch to health was about 18.2 s with model warmup.
Computing the trial identity from the model, runtime and settings took
1.29 s once before the timed arms.

## Memory

The two disk arms sampled 511.3–511.4 MiB process RSS while asleep.
The RAM arm sampled 562.7 MiB. The peak process RSS in the paired
`f02` wake arms was 2,089.3 MiB with disk and 2,141.1 MiB with RAM.
The disk mode traded about one second of `f02` wake time for about
51 MiB less process RSS during idle and across the paired runs.
These measurements are process RSS; the file can also occupy reclaimable
filesystem cache.

## Model warmup

With the same compatible disk snapshot, `--no-warmup` removed the dummy
decode at model initialization. For `f02`, process launch to health fell
from 18.210 s to 17.294 s. Completion after health was 39.904 s with
warmup and 39.975 s without it. The sums were 58.114 s and 57.269 s.
On `j10` idle wake, model reload fell from 7.10 s to 6.13 s and complete
reply time fell from 30.907 s to 29.850 s. Both replies and their
first-token logits matched across settings. Each setting ran once per
fixture.

## Rejection checks

Each probe capped generation at one token and inspected the cache count
and restore log. Changing the runtime identity, replacing `short line`
with `brief line` in the system text, flipping the last payload byte, and
truncating the file to 100 bytes all caused full 276-token prefill and
replacement of the snapshot. The
changed system text still rendered to 106 system tokens, exercising exact
token comparison for equal-length prefixes. The original system prompt
produced its fresh first-token logit digest after the identity, payload,
and truncation checks.

The memory and battery guard thresholds were 1,200 MiB available and
40 °C. The main run reached a low of 2,151.2 MiB available and a high of
27.9 °C. These are two deterministic Wiki prompts on the C.24 device, with
separate one-token rejection probes. The experiment used isolated workers
and left the installed Wiki feature disabled.

## Launcher validation

The checkout launcher opts in with `/etc/n20-wiki-disk-prefix`. It hashes
the GGUF, protected OpenCL and Bionic runtime files, C.24 GPU firmware
files, kernel build notes, boot ID, and exact worker options. It derives
the identity once per process start. The boot ID scopes restores to the
current boot because the contents of loaded KGSL firmware are unavailable
through sysfs. The cache directory is mode 0700 and belongs to the model
UID and GID. Without a compatible server implementation or trusted inputs,
the launcher continues with ordinary prefill.

An isolated phone run used a fresh Android ARM64 b12110 build with the
two server patches and the stock OpenCL kernels. The scratch launcher
used a private bundle, socket and cache, a stub for the Minecraft bridge,
a five-second idle threshold and first-token logit tracing. Model warmup
and the usual prompt batch splits stayed enabled. The GPU lock serialized
the run with other tests. Inspection of the worker's process mappings
confirmed that all loaded executables and shared libraries were in the
fingerprinted OpenCL and Bionic trees. The live Minecraft service stayed
online.

| Scenario | Identity preparation | Launch to health | Evaluated prompt tokens | Request to complete reply | Launch to complete reply |
| --- | ---: | ---: | ---: | ---: | ---: |
| Disk enabled, seed snapshot | 4.212 s | 26.696 s | 276 | 50.246 s | 76.942 s |
| Disk enabled, restart and restore | 2.690 s | 24.716 s | 170 | 33.547 s | 58.263 s |
| Disk enabled, idle wake | — | — | 170 | 44.547 s | — |
| Marker absent, new process | — | 22.022 s | 276 | 49.144 s | 71.166 s |
| Marker absent, idle wake | — | — | 276 | 60.132 s | — |

The snapshot was 56,168,292 bytes with mode 0600. The matched new-process
restore saved 12.903 s through the complete reply after including the
startup identity check. The matched idle wake saved 15.585 s and both
wake times include reloading the model. The new-process runs were ordered
seed, restore, then marker absent. The five requests used the same offline
observer fixture and each returned two game ticks with the same Observer
citation and stop reason. Their first-token logits matched byte for byte.
The marker-absent run exercised ordinary model startup with the sanitized
environment. The run reached 2,007.0 MiB minimum available memory and a
27.0 °C maximum battery temperature.

The launcher syntax check and the three isolated helper tests passed.
The helper tests check identity changes for model, backend, driver, Bionic,
firmware, boot and context inputs, reject an unpatched or shadowed server
implementation and unsafe files, and verify private cache permissions.
