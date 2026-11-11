# Nord N20 Bonsai vocabulary argmax, November 10, 2026

The test used Bonsai 2 4B Q2_0 on the Nord N20's Adreno 619 with
llama.cpp b12110 and the C.24 OpenCL driver. Inference used two threads
on CPUs 0–2 at nice 15. Minecraft remained online. The test used an
isolated model process, private shader caches and the installed model
file.

The stock OpenCL argmax scans one row with 32 lanes. The prototype
scans the 248,320 vocabulary scores with 128 workgroups of 128 lanes.
Each group writes its best score and token ID to a shared 1 KiB
buffer. One final workgroup reduces those winners. Both GPU paths
choose the larger ID on a tie and ignore NaN scores.

| Isolated argmax | Median GPU event span |
| --- | ---: |
| Stock OpenCL | 2.750 ms |
| Two-stage OpenCL | 0.096 ms |

The direct probe used synthetic logits at the model's vocabulary
width. Both paths selected the expected token on all 25 tests, which
included equal maxima, infinities and NaNs. The medians exclude the
first pair. The two-stage timing spans both dispatches and excludes
logit upload and winner readback.

## Full-model decode

The installed backend took a median 8.0423 s for 16 decode steps with
host greedy sampling and 8.8603 s with stock backend greedy sampling.
Each mode ran three times and generated the same token IDs.

The tuned backend included the 16-row vocabulary projection,
64-weight prefill partial sums, the small-batch prefill tile,
width-512 radix Hadamard, direct recurrent-state copy and fused weight
packing. The paired run used a 63-token rendered Minecraft torch
question. It restored a 62-token in-memory snapshot before each
repeat, with a 1,536-token context and a 256-token microbatch limit.
Decode timing starts after the first output token. The six runs shared
one process and used host, stock GPU, tiled GPU, tiled GPU, stock GPU,
then host.

| Sampling | 16 decode steps, seconds | Median, seconds |
| --- | --- | ---: |
| Host greedy | 6.5052, 6.4973 | 6.5013 |
| Stock GPU greedy | 7.3696, 7.4003 | 7.3850 |
| Two-stage GPU greedy | 7.3555, 7.3451 | 7.3503 |

All six runs generated the same 17 token IDs, including the first
output token. The comparison checks token parity for one short
continuation. It does not score the answer or test grammar-constrained
sampling. The tiled GPU path was 0.47% faster than stock GPU greedy
and 13.1% slower than host greedy. The stock sampling graph pads the
logits and exposes the full sampled logits alongside the selected
token. The experiment changed only argmax. It did not profile the
remaining end-to-end gap.
