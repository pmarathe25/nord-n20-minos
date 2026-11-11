# Paired activation-texture reuse in full-model inference

2026-11-10. An opt-in scratch backend patch recognizes adjacent Q2 gate/up projections with the same input and compatible shapes, computes the first projection normally, then reuses its activation texture for the second. The two projection kernels and the following SwiGLU remain separate. Unsupported layouts or failure to obtain a usable first texture follow the original path. The candidate was not installed.

The phone ran Bonsai 4B with the same patched Android b12110 backend in both arms, selecting the candidate only with `N20_Q2_TEXTURE_PAIR=1`. Both used the tuned Q2 prefill and activation-texture sources, 256-token microbatches, two CPU threads, and the same GPU and private shader cache. A scratch harness loaded Wiki question and evidence text with approximate chat framing, ran two full prefills per process and generated eight subsequent tokens. The reported first-token time includes the host's first-logit hash. The server was idle-checked and stopped for this comparison; these numbers do not measure gameplay or an `@wiki` chat round trip.

| Prompt tokens | Profile | First-token seconds, runs 1 / 2 | Eight-token decode seconds, runs 1 / 2 |
| ---: | --- | ---: | ---: |
| 271 | Baseline before | 28.9201 / 28.8198 | 4.2610 / 4.2525 |
| 271 | Pair | 29.3994 / 29.3789 | 4.3155 / 4.3171 |
| 271 | Baseline after | 28.9107 / 28.8341 | 4.3005 / 4.2968 |
| 136 | Pair | 14.6027 / 14.5502 | 4.0194 / 4.0148 |
| 136 | Baseline after | 14.7420 / 14.6707 | 4.0201 / 4.0160 |

The paired dispatch trace appeared for both candidate processes. For each prompt, all six or four first-token full-logit FNV hashes and generated token sequences matched across profiles. This is a limited parity check, not a proof for all graph shapes or fallback conditions. The 271-token paired run was about 0.5 seconds slower than the bracketing controls. The 136-token result was about 0.1–0.2 seconds faster than its one subsequent control, without a preceding control. The mixed observations and small sample do not justify enabling the candidate. The cause of the longer-prompt slowdown was not isolated.

The patch is `wiki/experiments/llama-b12110-n20-texture-pair.patch`. No installed service, firmware or model was changed. The maintenance guard observed at least 2,693,428 KiB available memory and at most 28.6 °C battery temperature, then verified Minecraft's status; CDSP remained offline with zero recorded crashes.
