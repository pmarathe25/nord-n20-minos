# Hybrid inference with a restored system prefix

2026-11-10. A scratch backend combined the rebuilt tuned Adreno profile, the existing experimental GPU–Hexagon 256-token pair-reuse patch, and the `n20-model-bench` sequence-state save/load APIs. The tested model was Bonsai 4B. The prompt was the same 323-token held-out Wiki question with four offline passages and approximate chat framing used in the [GPU-only prefix check](2026-11-10-wiki-rebuilt-prefix.md). The benchmark computed and saved the common fixed system prefix, then loaded it in a fresh process. Changing question and Wiki evidence were not saved. Each arm ran once in this order, using two efficiency-core threads and a 256-token microbatch.

| Arm | Model load and context | Prefix preparation / restore | Time to first token |
| --- | ---: | ---: | ---: |
| Hybrid fresh before, cold shader cache | 109.5157 s | — | 38.3010 s |
| Hybrid seed, warm shader cache | 6.6221 s | 9.4793 s to compute and save | — |
| Hybrid restored, warm shader cache | 6.6764 s | 0.0678 s to restore | 18.9834 s, including restore |
| Hybrid fresh after, warm shader cache | 6.6464 s | — | 27.7246 s |

The snapshot covered 106 tokens and was 56,168,144 bytes. The warm restored run reached the first token 8.7412 seconds earlier than the single warm fresh control, approximately 31.5% of that interval. Fresh and restored hybrid runs had the same first-token ID and full-logit FNV hash. Hybrid arithmetic quantizes a DSP shard and its logits are not expected to be identical to GPU-only inference. The earlier GPU-only restored time was 19.9302 seconds in a different run; that cross-run difference does not establish the independent benefit of the DSP when a prefix is restored.

The fresh arms each reported 62 of 62 hybrid projections dispatched and 62 of 62 eligible pairs reused. The seed and restored arms each reported 31 of 31 dispatched and 31 of 31 pairs reused. No fallback was recorded, and the two-column Gated DeltaNet option was confirmed. These are process-level counters and a single-prompt first-token parity check, not complete answer or citation parity, gameplay, or full chat latency. The first arm compiled shaders and is not a warm control.

This is a scratch benchmark of the underlying sequence-state APIs. It does not test the production launcher's fingerprint, integrity wrapper or automatic fallback for prefix snapshots. The hybrid runtime is experimental, assumes serialized execution and an empirical activation bound, and is not a supported installed profile. No unvalidated prefix checkpoint or hybrid module was installed.

The guard observed at least 2,607,080 KiB available memory and at most 27.2 °C battery temperature. Minecraft was restored and passed its status check; CDSP returned offline with zero recorded crashes. Firmware, partitions, model, configuration and world data were not changed. Private prompt, cache, state, binaries and raw logs were removed.
