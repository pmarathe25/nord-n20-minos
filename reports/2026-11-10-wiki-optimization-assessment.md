# Assessment of phone inference optimization

2026-11-10. The Bonsai 4B Wiki assistant works on the Nord N20, but its current GPU and Hexagon paths have reached diminishing returns. There is no demonstrated order-of-magnitude improvement for this device. This is a conclusion from the tested implementations, not a proof of a physical limit.

The instrumented [complete-request trace](2026-11-10-wiki-attention-dispatch.md) took 38.83 s, of which Q2 projections accounted for 24.766 s and the remainder outside recorded kernels accounted for 0.721 s. As arithmetic bounds for that particular trace, halving projection execution with all else fixed would give about a 1.47x faster request; eliminating every projection would still leave about 14.06 s. The trace predates some later optimizations, and event instrumentation itself can affect timings. These are not predictions for every prompt. They do show why reducing launch overhead alone is unlikely to deliver a tenfold improvement.

The experimental GPU–Hexagon model improved a limited eight-question request comparison by a median 1.114 s, with both arms passing 5 of 8 automated checks. Pair reuse saved fractions of a second in a separate screen. The newer block-major DSP kernel improved a short projection but did not improve the measured 323-token full-model first-token time. The approximate DSP runtime and the many experimental GPU kernels remain opt-in scratch implementations, not installed or production-validated. See [hybrid microbatch results](2026-11-10-wiki-hybrid-microbatch.md) and the [block-major model screen](2026-11-10-wiki-block-major-screen.md).

Astra independently reviewed the reports and did not identify a supported 10x kernel route. A different chunk-parallel Gated DeltaNet algorithm is untested, but the older trace attributes 3.941 s to all Gated DeltaNet kernels and a later column-layout optimization already improved them. A subsequent [token-block traversal test](2026-11-10-wiki-hexagon-token-order.md) removed the original single-call DSP penalty, but did not beat the established two-call mixed projection or demonstrate a full-model gain.

A task-specific trained or distilled model is a different research project, not an already demonstrated kernel optimization. The earlier zero-shot smaller-model results do not establish the quality of such a model. This project should only be pursued if its training and held-out factuality, citation, and abstention evaluation are explicitly desired; the present user preference is to focus on the existing model and hardware kernels.

## Decode reduction experiments

The 512-work-item, 16-split GPU decode kernel ordinarily serially sums all partials after a local-memory barrier. A Qualcomm subgroup-shuffle variant first combines four splits within each subgroup; a two-stage local-memory variant instead performs a four-way partial sum, a second barrier, and a final four-way sum. Both change floating-point summation order. The phone ran real layer-0 Q2 up and down weights with deterministic synthetic activations and full-output scalar comparison. Each number is a five-event median; the baselines bracketed each candidate. Timings exclude compilation, host transfer and the rest of the model.

| Projection | Baseline before | Subgroup shuffle | Baseline after |
| --- | ---: | ---: | ---: |
| Up 9,216 × 2,560 | 1.194 ms | 1.364 ms | 1.188 ms |
| Down 2,560 × 9,216 | 1.195 ms | 1.501 ms | 1.197 ms |

The subgroup shader was substantially slower. Its first compile attempt omitted the OpenCL C 2.0 option and failed; the reported comparison uses that option in all arms.

| Projection | Baseline before | Local two-stage | Baseline after |
| --- | ---: | ---: | ---: |
| Up 9,216 × 2,560 | 1.190 ms | 1.178 ms | 1.199 ms |
| Down 2,560 × 9,216 | 1.195 ms | 1.192 ms | 1.196 ms |

The local variant's isolated difference was small; the model-level comparison is below. All 9,216 up and 2,560 down outputs were compared against the scalar reference; normalized RMS errors for the two-stage path were 7.06e-8 and 8.72e-8 respectively. Fallback tests for four and eight splits with a 16-row tile had full output hashes identical to the corresponding baseline for the tested inputs. These results are not a proof for other shapes or activation values. The experimental kernels are `wiki/experiments/n20-q2-decode-tree.cl` and `wiki/experiments/n20-q2-decode-subgroup.cl`; the latter is restricted to its documented geometry.

## Full-model decode check

The two-stage variant was compared in baseline/candidate/baseline order on a 323-token held-out Wiki question with four offline passages and approximate chat framing. The scratch GPU-only model used the same checkpoint, 256-token microbatches, two-column Gated DeltaNet, two efficiency-core CPU threads, and 16 greedy decode steps after the first token. Each fresh worker ran the same prompt twice. The first baseline's first prefill included lazy shader compilation. The shader cache was shared across the three scratch workers; no live worker was modified. First-token times include the first-logit hash, and the decode intervals exclude prefill, retrieval and chat delivery.

| Arm | First-token seconds, runs 1 / 2 | Sixteen-token decode seconds, runs 1 / 2 |
| --- | ---: | ---: |
| Baseline before | 37.7801 / 29.2701 | 7.0594 / 7.0543 |
| Local two-stage | 29.3204 / 29.2397 | 7.0739 / 7.0709 |
| Baseline after | 29.5553 / 29.4953 | 7.1051 / 7.1012 |

The candidate fell between its controls in decode time, so this does not establish an end-to-end improvement. All six generated the same 17 token IDs. The candidate's first-token full-logit hash differed from both matching baseline hashes, consistent with reordered floating-point addition; these checks do not establish answer-quality parity. The new kernel is not selected for deployment. The phone's guard observed at least 2,803,492 KiB available and at most 28.3 °C battery temperature. It restored the idle-checked Minecraft service, which passed its status query; CDSP was offline with zero recorded crashes afterward.
