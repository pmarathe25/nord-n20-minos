# Nord N20 activation texture with FP16 outer accumulation

Combining the activation-texture path with FP16 outer accumulation reduced
two warm complete model requests by about 0.6–0.9 seconds, or 1.3–2.0%,
against bracketing references. This changes numerical precision. The limited
quality checks do not justify enabling it in the live worker.

## Projection check

The Nord N20 C.24 Adreno OpenCL driver ran deterministic FP16 activations
and real layer-0 Bonsai 2 4B Q2_0 weights. The existing image2D harness
included the GPU buffer-to-image copy in each event span. It used a private
cache, efficiency cores 0–2, nice 15 and the shared GPU benchmark lock.
The numbers are medians of five events per process; the baseline bracketed
the candidate. Sampled errors compare against a double-precision scalar dot
product; hashes cover the complete output.

| 128-token projection | Baseline before / after | FP16 outer | Baseline NRMSE | FP16 outer NRMSE |
| --- | ---: | ---: | ---: | ---: |
| Up, 9,216 × 2,560 | 56.278 / 55.459 ms | 51.840 ms | 0.001007 | 0.001347 |
| Down, 2,560 × 9,216 | 56.852 / 56.073 ms | 52.850 ms | 0.001008 | 0.002340 |

The candidate hashes were `87044013e66f0204` and `ae41c4b580c3ca43`;
the baseline hashes were `959f94cc3d04f9e1` and `259a995f55f8ed3b`.
Maximum sampled down-projection absolute error increased from 0.00788 to
0.03433. Rebuilding and executing the retained candidate source produced
the same down-projection hash and error. These deterministic activations
do not establish numerical range safety for arbitrary real prompts.

## Complete model requests

An isolated Android ARM64 llama.cpp b12110 worker used the existing N20
OpenCL, activation-texture and lazy Flash Attention cache patches. Both
variants used the same scratch backend, installed model, tuned fallback,
decode, packing, FWHT and tail kernels. Only the texture source changed.
Each process used F16 KV, a 1,536-token context, a 256-token microbatch,
two threads on CPUs 0–2 at nice 15, and a private shader cache. The model
stayed resident while the live Minecraft server was idle. The test held
the shared GPU lock, checked a 1,200 MiB available-memory floor and a 40 °C
battery ceiling before each request, and used a private copy of the cached
Wiki. Direct backend instrumentation confirmed texture dispatch in each
process. The initial cold baseline with suppressed backend log output was
excluded; the reported bracketing runs used the instrumented binary.

Each process warmed up on the Observer question, then answered the two
questions below. The clock runs from HTTP submission through receipt of
the complete response; it excludes worker startup, retrieval and chat
delivery. The bridge's grounded prompt, cached retriever excerpts, greedy
sampling and blank-line stop were used, with prompt reuse disabled. Each
row is one request per arm, not a distribution.

| Question | Tokens in / out | Baseline before | FP16 outer | Baseline after |
| --- | ---: | ---: | ---: | ---: |
| Observer output strength | 329 / 29 | 45.180 s | 44.488 s | 45.416 s |
| Current comparator output in a private base | 297 / 39 | 45.901 s | 45.322 s | 46.156 s |

Observer prefill was 33.739 / 33.001 / 33.925 s and private-state prefill
was 30.386 / 29.746 / 30.562 s in the same order. Returned text and
token counts matched across arms, but token IDs were not captured. For
Observer, the model gave the correct supporting fact but omitted the
required `Answer:` line; the bridge parser consequently abstained. The
private-state question correctly abstained. Both behaviors occurred in
all three arms. This suite is too small to establish general answer or
citation quality, and it does not test gameplay tick latency.

An additional one-token Observer request compared the top five output
choices. Their order and the selected token matched, but their log
probabilities changed: `Support` moved from -0.184353 to -0.183860, and
`Answer` from -1.810519 to -1.812711. This is a top-five check, not a
comparison of the entire vocabulary or later token distributions.

The [experimental kernel](../wiki/experiments/n20-q2-activation-image-halfacc-prefill.cl)
combines the existing image2D path with FP16 scale and outer sums; it
rejects FP32 partial-sum builds. Its candidate accumulators can overflow
or lose contributions on other inputs. Do not deploy based on these two
requests. For broader ideas, [Bitnet.cpp](https://aclanthology.org/2025.acl-long.457/)
describes ternary lookup approaches; the earlier pairwise lookup trial on
this phone was slower than the tuned kernel, so that paper does not itself
establish a speedup on this GPU.

No live backend, firmware or service configuration was changed. Scratch
workers used loopback only and stopped at the end of the runs. Phone scratch
was removed and workstation scratch was moved to the Recycle Bin after
recording the results.
