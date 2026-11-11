# Experimental GPU–Hexagon model integration

2026-11-10. An opt-in scratch backend integrates dynamic Q8 activations and resident DSP gate/up weight shards into the Bonsai 4B OpenCL model graph. The GPU computes the remaining rows and the DSP result is scattered into the real destination. The implementation is experimental and was not installed. It assumes serialized model loading and graph execution, retains resident slots until backend teardown, uses an empirical ±16 activation limit, and has not been established safe for arbitrary graphs or model reloads.

## First model-prefill check

The phone processed one 323-token held-out Wiki question with approximate chat framing, using the same patched backend and model in all arms. The hybrid was enabled only in its own arm. All arms used two CPU threads and the tuned activation-texture GPU path; the hybrid and its GPU controls used a 128-token microbatch. The table reports first-token latency, including prefill but excluding model load and later answer generation.

| Arm | Microbatch | First-token time | Load and context |
| --- | ---: | ---: | ---: |
| GPU, cold shader cache | 128 | 41.0877 s | 109.7243 s |
| GPU + Hexagon | 128 | 30.2895 s | 6.6217 s |
| GPU, warm control after | 128 | 31.6745 s | 5.2806 s |
| GPU, warm | 256 | 31.5499 s | 5.3575 s |

The first GPU arm compiled shaders and is not a fair warm latency control. Against the subsequent warm GPU control, the hybrid prefill was about 4.4% faster in this single run; against the 256-token GPU run it was about 4.0% faster. The hybrid reported 64 attempted resident slots, 186 projection attempts, 186 dispatched and no fallbacks. Its first generated token matched the controls, but the full-logit hash changed, as expected for the approximate DSP arithmetic. One first token does not establish answer quality. Differences in startup, cache state, temperature and run order remain possible, and no game activity was measured.

The scratch implementation is in `wiki/experiments/llama-b12110-n20-hybrid-trial.patch`, `n20-hybrid-runtime.cpp`, `n20-hybrid-aux.cl`, `n20-dsp-client.c`, and the resident DSP module. A separate standalone phone smoke test of the final client passed two real matrices at one and sixteen tokens, scalar checks and lifecycle checks. The checkout's `test-wiki-chat.py` and paired-comparison tests passed on the phone with 18 and 3 tests respectively. The production installation's older chat module lacks the checkout's streaming interface; matched answer trials must use the same checkout code for both arms and should not be described as measurements of that installed older bridge.

## Matched Wiki request check

Eight held-out questions used frozen offline retriever evidence in the same order for separate GPU-only and hybrid workers. Both used the checkout's streaming request and response formatter, 128-token microbatches, the same model and GPU settings, a fresh worker, and an initially empty prompt cache. This measures model requests through the Unix socket; it excludes retrieval, worker startup, Minecraft chat, and gameplay. The harness deliberately called the model for all eight cases, including one for which the exact-answer route was available.

| Case | GPU seconds | Hybrid seconds | Automated check, GPU / hybrid |
| --- | ---: | ---: | --- |
| j03 | 62.584 | 61.435 | fail / fail |
| j10 | 24.981 | 24.989 | pass / pass |
| j13 | 29.480 | 28.913 | fail / fail |
| j18 | 36.219 | 36.179 | pass / pass |
| j25 | 85.635 | 85.245 | fail / fail |
| j32 | 36.105 | 34.207 | pass / pass |
| j37 | 41.121 | 40.669 | pass / pass |
| j39 | 22.604 | 22.191 | pass / pass |

Each arm passed 5 of 8 automated checks; there were no scored regressions or improvements, and one rendered answer text changed while satisfying the same check in both arms. Two private-state cases produced explicit abstentions in both arms. One request in each arm ended at the generation-length limit and was counted as a failure. Neither arm had transport errors. The median paired time difference was −0.433 seconds; this small, single-order sample does not establish a robust speedup or answer-quality equivalence. The hybrid worker reported 64 attempted slots and 1,054 dispatched projections with zero recorded fallbacks across all requests. These are aggregate worker counters, not a per-request coverage proof.

The first attempted paired run used the older installed `wiki_chat` module, which lacks the checkout's streaming reader; all eight calls failed before inference and those results were discarded. The valid comparison used the checkout code in scratch space. During it, the guard observed at least 2,695,460 KiB available memory and at most 29.2 °C battery temperature; Minecraft resumed and passed its status check, and CDSP returned offline with zero crashes. Private fixtures, answers and logs were kept in scratch only.
