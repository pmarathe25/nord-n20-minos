# Rebuilt GPU bundle: cross-process prefix state

2026-11-10. A fresh bundle from `tools/build-wiki-gpu.py` ran the Bonsai 4B model with the tuned activation-texture profile, a 256-token microbatch, two efficiency-core threads and the two-column Gated DeltaNet option. The model benchmark used one 323-token held-out Wiki prompt with four offline passages and approximate chat framing. It compared the exact tokenized fixed system prefix against the prompt, saved its complete sequence state, and loaded it into a fresh process; the changing question and Wiki passages were not included in the snapshot. Each arm ran once, in the order shown.

| Arm | Model load and context | Prefix preparation / restore | Time to first token |
| --- | ---: | ---: | ---: |
| Fresh before, cold shader cache | 108.3115 s | — | 38.1356 s |
| Seed system prefix, warm shader cache | 5.4172 s | 10.0099 s to compute and save | — |
| Restore system prefix, warm shader cache | 5.3913 s | 0.0736 s to restore | 19.9302 s, including restore |
| Fresh after, warm shader cache | 5.4240 s | — | 29.6025 s |

The snapshot held 106 tokens and occupied 56,168,144 bytes. The restored first-token result matched the fresh arms' token ID and full-logit FNV hash. The warm restored prefill was 9.6723 seconds faster than the single warm fresh control, approximately 32.7% for that measured interval. The cold first arm compiled shaders; it is not a warm baseline. The remaining processes shared the same private shader cache, and traces confirmed the two-column option. These observations do not establish general answer quality, performance across prompts, active-player tick latency, complete chat time, or a GPU–Hexagon result.

This is a scratch model-benchmark test of llama.cpp sequence-state save/load, not a test of the production launcher's runtime fingerprint, integrity validation or automatic cache management. Its private state file had no production provenance wrapper. Do not reuse that procedure as a production cache without the validated identity and fallback mechanisms described in [Wiki chat](../docs/wiki-chat.md). The [earlier launcher trial](2026-11-10-wiki-texture-launcher.md) measured a complete cited reply with the separate identity mechanism.

The guard observed at least 2,839,872 KiB available memory and at most 26.6 °C battery temperature. Minecraft was restored and passed its status query; CDSP returned offline with zero recorded crashes. No installed worker, model, configuration, firmware or world was changed. Private prompt text, cache copies, checkpoint state, binaries and raw logs were removed.
