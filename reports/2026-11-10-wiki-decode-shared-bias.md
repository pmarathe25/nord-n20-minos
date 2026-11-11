# Shared activation sums for Q2 decode

2026-11-10. An experimental OpenCL decode path computes each 64-column activation sum once in a separate GPU kernel and reuses it across output rows. The measured interval includes that extra dispatch and the decode kernel. This is a projection test, not a model or chat latency result.

With real layer-0 Bonsai weights and one synthetic token, five timed samples per process gave the following medians, in milliseconds:

| Shape | Baseline before | Shared sums | Baseline after |
| --- | ---: | ---: | ---: |
| 9,216 × 2,560 | 1.190 | 1.162 | 1.188 |
| 2,560 × 9,216 | 1.197 | 1.169 | 1.203 |

The candidate reduced measured time by about 2–3% in these runs. Complete output-buffer FNV-1a hashes matched the bracketing controls for each shape; sampled outputs also matched the same FP64 reference error. These checks cover the tested synthetic inputs and weights, not all inputs, and hashes are not a proof of equality. The kernel and harness are `wiki/experiments/n20-q2-decode-shared-bias.cl` and `n20-q2-decode-shared-bias-bench.c`.

## Model-level screen

A subsequent scratch backend trial integrated the kernel behind `N20_Q2_BIAS_SOURCE`, retaining the ordinary decode path as fallback. The phone ran the same 323-token Wiki prompt in the 256-token hybrid model, with the two-column Gated DeltaNet setting. Each process generated 16 subsequent tokens after prefill. The baseline, candidate, and second baseline decode intervals were 7.0916, 6.9969, and 7.0719 seconds respectively. The candidate trace confirmed at least one shared-bias dispatch; a complete dispatch count was not observed in the harness teardown. The first-token full-logit hashes and all 17 token IDs, including the first token, matched across arms. First-token times were 28.1928, 27.8492, and 28.1656 seconds; the decode option was not intended to optimize that prefill.

The 0.075–0.095 second decode difference is a single bracketing comparison, not a robust speedup or full answer-quality result. The integration patch is `wiki/experiments/llama-b12110-n20-decode-bias.patch`; it remains opt-in and uninstalled. During this test the guard observed at least 2,611,240 KiB available memory and at most 29.3 °C battery temperature, restored Minecraft and verified its status, and CDSP returned offline with zero crashes.

The isolated phone tests used the shared accelerator lock and idle-checked Minecraft stop, and restored the server. No installed service or firmware was changed.
