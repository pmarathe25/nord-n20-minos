# One-column Gated DeltaNet prefill screen

2026-11-10. The opt-in Gated DeltaNet patch permits a one-column prefill layout, but its earlier complete-request measurements selected two columns. A standalone phone screen compared one against two on the C.24 Adreno 619 before considering a full-model trial. The kernel source was fetched from pinned llama.cpp commit `cd959bae99ff4bed22bef2604f45241c09bbb8c8` (SHA-256 `8479333644d58bbf43c752d8c3220252771f1cc534159454a16456d1b12ecc30`). The existing GDN harness was compiled with Android NDK r30, changing only its prefill baseline from four to two columns. It used synthetic deterministic inputs, 128-wide state, 32 value heads, 16 query/key heads, scalar gating and one state snapshot.

| Tokens | Two columns before | One column | Two columns after |
| ---: | ---: | ---: | ---: |
| 16 | 4.2450 ms | 5.9141 ms | 4.1930 ms |
| 128 | 28.4780 ms | 43.4171 ms | 28.7621 ms |

Each number is the median of five GPU events. Timing excludes compilation, allocation and transfers. Both complete output and final-state buffers had identical hashes and no differing floats for each fixture; sampled scalar-reference maximum absolute errors were 0.001143 and 0.001251 respectively. This is a limited numerical test, not a guarantee for all inputs. The one-column layout was slower than both controls by approximately 39% and 52%, so no complete-request improvement is established and it was not selected. The screen held the shared GPU lock, used efficiency cores at reduced priority and left Minecraft running; available memory and battery temperature after the brief run were 4,401,600 KiB and 27.0 °C. Scratch files were removed; no installed files or configuration changed.
