# Nord N20 recorded feed-forward dispatches, November 9, 2026

The C.24 Adreno 619 driver replayed the two existing Bonsai 2 4B Q2_0
gate and up projections and their SwiGLU as three ordered dispatches.
The trial used llama.cpp b12110 with the existing MinOS OpenCL tuning,
one 1,536-token context, a 256-token microbatch limit, and host greedy
sampling. The model ran under UID 61124 with two threads on efficiency
cores at nice 15. Both modes used the same backend binary, prompt, and
private copy of the shader cache. Direct mode ran before replay mode.
The experiment used the installed model and a private scratch directory.

| Mode | First pass to first token | Second pass to first token | First 16-step decode | Second 16-step decode |
| --- | ---: | ---: | ---: | ---: |
| Direct | 11.7979 s | 0.9928 s | 6.5396 s | 6.5349 s |
| Recorded FFN | 11.8599 s | 0.9971 s | 6.5730 s | 6.5734 s |

The two-token synthetic prompt produced the same 17 token IDs and the
same cumulative hash `d2df4e976bcf7cd3` over the full 248,320 F32
logits at each of the 17 sampling points in all four runs. The harness
cleared model memory before each repeat. The replay trace reached 896
cache hits, 64 built recordings, 66 cold entries, and three invalidations.
The warm decode measurement was 0.6% slower with replay. The two passes
per mode on one short prompt do not establish a performance difference
at longer contexts. The trial did not cover KV bucket transitions,
multiple model contexts in one process, or complete Wiki replies.

The cache records only the recognized single-token, contiguous F32
feed-forward segment with packed Q2_0 weights and the tuned decode
kernel. It keys entries by the eight referenced buffers, four byte
offsets, and gate/up order. The fixed shape and kernel selection fix
the remaining arguments and work sizes. It builds on a repeated key
and retains the referenced buffers. Buffer reset or release clears
the cache, after waiting for any submitted recordings to finish.
Copies and other operations keep their existing live-queue order.
The C.24 vendor exports the recording entry points even though its
generic OpenCL extension lookup returned null in the earlier probe.
[Earlier driver probe](2026-11-09-wiki-graph.md#fusing-operations-and-submitting-work-together)
[Qualcomm OpenCL guide, section 9.1.3](https://docs.qualcomm.com/doc/80-NB295-11/80-NB295-11_REV_C_Qualcomm_Snapdragon_Mobile_Platform_Opencl_General_Programming_and_Optimization.pdf)

Apply the [replay patch](../wiki/experiments/llama-b12110-n20-recorded-ffn.patch)
after the [existing tuning patch](../wiki/experiments/llama-b12110-n20-opencl.patch)
in a separate b12110 checkout. Build the Android OpenCL backend with
OpenCL 2.0 and the existing Adreno settings. Set `N20_RECORD_FFN=1`
alongside the tuned `N20_Q2_DECODE_SOURCE` in the isolated test process.
Unset `N20_RECORD_FFN` for direct dispatch. The separate
[logits hash patch](../wiki/experiments/n20-model-decode-hash.patch)
applies to the MinOS model harness for this comparison. The timed decode
loop includes hashing in both modes. The replay switch remains opt-in.
