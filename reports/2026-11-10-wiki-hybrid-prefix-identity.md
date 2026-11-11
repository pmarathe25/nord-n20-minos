# Hybrid prefix cache identity trial

2026-11-10. The scratch identity checker was exercised with the rebuilt Bonsai 4B GPU/Hexagon hybrid worker on the Nord N20. Two distinct offline Wiki questions shared the same 106-token system prefix. Their question and evidence were still processed separately. The test supplied fingerprints for the actual scratch binaries and library trees, DSP shell and split firmware components, GPU driver and firmware components, shaders, model, reference and boot identity. These are caller declarations, not proof of which firmware or code actually ran; see the [checker review](2026-11-10-wiki-hybrid-prefix-review.md).

| Question | Fresh first token, before / after | Restored first token | Restore time | Cache existed |
| --- | --- | --- | --- | --- |
| j03 | 27.7706 / 27.8740 s | 18.9984 s | 0.0632 s | No; seeded in trial |
| j10 | 23.1897 / 23.1745 s | 14.4119 s | 0.0657 s | Yes; reused j03 prefix |

Fresh and restored runs matched first-token IDs and logit hashes for each question. Hybrid projection dispatches were 62 for each fresh run and 31 for each restored run, without fallbacks; all attempted paired projections were reused. Prefix creation took 9.7107 s of model work. Changing a declared scratch input caused the existing cache to be rejected. The first test uncovered a tuple-to-JSON-list mismatch in runtime-tree identities; the checker was fixed and a JSON round-trip regression test added before the successful run. Eleven isolated tests passed on the phone.

These are first-token tests, not full answers, answer-quality validation, concurrent gameplay measurements, or proof that the hybrid Q8 approximation matches the GPU-only model. First-token times exclude model loading, which took approximately 6.6–6.8 s in these runs. This experimental cache checker is not production-ready: caller assertions and before/after hashes do not establish a complete trust boundary or prevent change-and-revert races. The measurements do not establish that further kernel gains are impossible. During the guarded run the minimum available memory was about 2.50 GiB and peak battery temperature 27.8 °C. Minecraft status passed after restoration; CDSP was offline with zero reported crashes.
