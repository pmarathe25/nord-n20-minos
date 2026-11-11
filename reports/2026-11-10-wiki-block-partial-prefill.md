# Split quantization-block GPU prefill screen

2026-11-10. This scratch screen tests Astra's proposed register-pressure experiment on the Nord N20's Adreno 619: calculate each 64-column Q2_0 block independently into FP16 partials, then read and accumulate the partials in original block order with FP32 scales. The baseline holds FP16 block partials and FP32 outer accumulators together in the [activation-texture kernel](../wiki/experiments/n20-q2-activation-image-prefill.cl). The [experimental kernel](../wiki/experiments/n20-q2-block-partial-prefill.cl) and [scratch benchmark patch](../wiki/experiments/n20-q2-block-partial-bench.patch) do not change the installed backend.

The phone ran the real layer-0 down-projection weights (2,560 rows by 9,216 columns), deterministic FP16 inputs and C.24 OpenCL. Each arm used a fresh worker; the table gives the median of five GPU-event spans including input buffer-to-image copy, both candidate kernel launches, and reduction, but excluding initialization, allocation, transfers to GPU, and scalar verification. The shared GPU lock was held, Minecraft stayed running, and the guard required at least 1,200 MiB available and battery under 40 C. Values are from a second bracketed run after an initial equivalent screen.

| Input tokens | Baseline before | Split block | Baseline after | Partial scratch |
| --- | ---: | ---: | ---: | ---: |
| 16 | 7.641 ms | 8.865 ms | 7.635 ms | 11.25 MiB |
| 128 | 56.548 ms | 181.427 ms | 56.716 ms | 90 MiB |

The median candidate partial kernel alone took 6.633 ms and 52.800 ms respectively. The reduction plus intervening queue time took approximately 2.2 ms and 128.3 ms; the pipeline numbers are the authoritative comparison and include the input image copy. All complete-output hashes matched between the three arms for each token count. Sampled scalar-reference normalized RMS errors were 0.000972 and 0.001008, respectively, identical between arms. This is only one deterministic input and two shapes; no comprehensive correctness or model-level equivalence is claimed.

The 128-token pipeline is more than three times slower, despite the lower-register inner kernel. The scratch memory and traffic, particularly the unfused reduction, overwhelm any benefit in this implementation. It does not meet the predeclared gate for model integration and was not deployed. This negative result does not prove that all block-partial layouts or reductions are slower.
