# Nord N20 Q2_0 split-K prefill trial, November 9–10, 2026

The projection harness ran on the Adreno 619 with the C.24 OpenCL driver,
using the installed Bonsai 2 4B model's layer-0 Q2_0 weights and reproducible
synthetic FP16 activations. The up projection is 9,216 rows by 2,560
columns. The down projection is 2,560 rows by 9,216 columns.

The baseline uses the tuned 64-column FP16 partial sums and an FP32
accumulator. It processes four rows per work item and four activation lanes
for the four-token case, matching the model's tuned tail. It uses eight
activation lanes for the larger batches. The split-K variant divides the
input columns into equally sized ranges of whole 64-column blocks. Each
range writes a separate FP32 output plane. A second kernel reads four
consecutive outputs from each plane, sums across ranges, and writes the
final four FP32 values. The fixed-range variant specializes the loop trip
count and rebases the input and weight addresses once per work item.

These are medians of ten launches after two warmups per configuration.
GPU span runs from the projection event's start through the reduction
event's end. Host time runs from just before enqueueing the projection
through the final event wait. Both exclude activation packing, transfers,
OpenCL compilation and buffer allocation. The table selects the fastest
split configuration by host time among 2, 4 and 8 splits and 64- and
128-thread workgroups. The baseline uses a 128-thread workgroup.

| Projection | Tokens | Baseline GPU / host, ms | Splits / threads | Split projection / reduction, ms | Split GPU span / host, ms | Host change |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Down | 4 | 2.992 / 3.139 | 8 / 128 | 2.671 / 0.027 | 2.700 / 2.854 | −9.1% |
| Down | 8 | 4.646 / 4.803 | 8 / 64 | 4.030 / 0.051 | 4.082 / 4.241 | −11.7% |
| Down | 16 | 8.275 / 8.475 | 8 / 64 | 8.009 / 0.101 | 8.110 / 8.317 | −1.9% |
| Down | 32 | 15.934 / 16.139 | 4 / 64 | 15.950 / 0.118 | 16.069 / 16.310 | +1.1% |
| Down | 128 | 61.685 / 61.993 | 2 / 64 | 63.331 / 0.269 | 63.600 / 63.902 | +3.1% |
| Down | 256 | 122.654 / 122.954 | 2 / 64 | 126.585 / 0.538 | 127.124 / 127.466 | +3.7% |
| Up | 4 | 2.557 / 2.702 | 8 / 64 | 2.475 / 0.093 | 2.568 / 2.736 | +1.3% |
| Up | 8 | 4.013 / 4.165 | 8 / 128 | 3.876 / 0.177 | 4.055 / 4.220 | +1.3% |
| Up | 16 | 7.876 / 8.034 | 8 / 64 | 7.741 / 0.352 | 8.095 / 8.269 | +2.9% |
| Up | 32 | 15.453 / 15.658 | 2 / 128 | 15.968 / 0.249 | 16.216 / 16.460 | +5.1% |
| Up | 128 | 61.266 / 61.527 | 2 / 64 | 63.411 / 1.022 | 64.435 / 64.745 | +5.2% |
| Up | 256 | 122.378 / 122.651 | 2 / 64 | 126.615 / 2.101 | 128.714 / 129.087 | +5.2% |

At four tokens the down projection's normalized RMS error against the
double-precision reference was 0.0008008149 for the baseline and
0.0008008364 for split-K over 108 sampled outputs. At eight tokens it was
0.0008406601 and 0.0008406848 over 216 samples. Maximum absolute errors
were about 0.00302 and 0.00313 respectively in both versions. Across the
table, normalized RMS error ranged from 0.00080 to 0.00155 for both paths.

The initial dynamic-range split kernel was about 12% slower than the
baseline on the up projection even with one split. Specializing the trip
count removed that large overhead. Requesting a half wave in a separate
short-batch sweep gave similar timings with 64 or 128 threads. A
32-thread group was slower in that sweep.

## Complete model

The scratch llama.cpp b12110 backend used eight fixed splits and
128-thread workgroups for the 2,560 × 9,216 down projection at two through
four tokens or exactly eight tokens. Both modes used the tuned prefill,
four-lane tail, packed weights, direct recurrent copy, radix-32 transform,
and tuned decode. The model harness used a 256-token microbatch limit,
1,536-token context, host greedy sampling, and the installed checkpoint.
It ran with two host threads on cores 0–2 at nice 15. The experiments used
private scratch processes and a private shader cache.

For the eight-token prompt `How do I craft a lightning rod?`, two processes
per mode ran in baseline, split, split, baseline order. Each process had
one first-use pass and four warm passes. Warm time to first token was
1.3686 s for baseline and 1.3282 s for split-K across the pooled passes,
a 3.0% reduction. The per-process warm medians were 1.3674 and 1.3694 s for
baseline and 1.3269 and 1.3292 s for split-K. First-use times were 12.09 to
12.12 s and all passes chose token 271.

The grounded Wiki test used the normal prompt format and offline retrieved
excerpts for “Which frog variant is in the pen at our base right now?”
The question was selected for its natural 259-token length, which gives
one full microbatch and a three-token tail. One run per mode followed an
eight-token warmup in that process. Time to first token was 26.6428 s
for baseline and 26.5251 s for split-K. Decode through the blank-line stop
took 15.0819 and 15.0913 s. All 31 generated token IDs matched. Both raw
replies led with an irrelevant beacon detail before abstaining. The
bridge's answer parser returns the same abstention in both cases. One
paired Wiki run is insufficient to establish a throughput difference.

All 248,320 first-token logits were finite in both modes. Compared with
baseline, normalized RMS differences were 0.000575 and 0.001061 for the
eight-token and 259-token prompts, with maximum absolute differences of
0.01288 and 0.01880. The top ten tokens and their ranking were identical
for both prompts.

Apply `wiki/experiments/llama-b12110-n20-splitk-trial.patch` after the
existing b12110 OpenCL patch and set `N20_SPLITK_SOURCE` to the absolute
path of `wiki/experiments/n20-q2-prefill-splitk.cl` in the scratch process.
The final patch builds and checks the optional kernels before selecting
them. Its temporary buffer is local to the OpenCL backend context, and the
reduction writes at the destination's original byte offset. An eight-token
phone smoke test reproduced the split logits hash. A second smoke test with
a missing source used the regular projection and reproduced the baseline
logits hash. The experiment was not deployed to the live service.
