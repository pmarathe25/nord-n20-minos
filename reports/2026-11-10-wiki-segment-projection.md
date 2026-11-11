# Segment reuse for Bonsai Q2_0 projections, November 10, 2026

A simple segmented-reduction GPU prototype was much slower than the
existing FP16 texture projection, despite reducing an idealized arithmetic
count. At 128 tokens it took 395.1 ms versus 55.6–56.4 ms for the baseline,
including the same buffer-to-image copy. It was not integrated into model
inference or tested on complete chat requests.

## Checkpoint structure

The recent [Sparse Segment Reduction paper](https://arxiv.org/abs/2610.08403)
groups repeated ternary weight patterns and reuses intermediate sums. Its
results concern its own algorithm and hardware; they do not predict the
Nord N20's performance. Bonsai's Q2_0 format has an independent scale for
each output row and 64-input block, so this trial reuses sums only within
one 64-input block and applies each row's scale afterward. The prototype
is a two-row segment-reuse experiment, not an implementation of the full
paper's algorithm.

The [pattern counter](../wiki/experiments/n20-q2-patterns.c) inspected real
layer-0 Bonsai 2 4B weights. The up matrix contained 8,085,467 negative,
7,423,281 zero and 8,084,212 positive codes. The down matrix contained
8,047,244 negative, 7,498,095 zero and 8,047,621 positive codes. Neither
contained code 3. Zero weights were 31.5% and 31.8% respectively in these
two matrices; other layers may differ.

For adjacent pairs of rows in the up matrix, the counter found 11,364,637
segment-aggregation and distribution additions in an idealized scalar
count, compared with 23,592,960 dense weight/input terms. This excludes
loads, indexing, scale operations and GPU instruction costs. It is not a
runtime estimate: the existing kernel performs vector arithmetic and the
segment method has different control flow and memory accesses.

## GPU prototype

The [kernel](../wiki/experiments/n20-q2-segment-prefill.cl) groups the 64
input positions for each pair of output rows by their two ternary codes,
omits the all-zero pattern and reuses each segment sum for both outputs.
It changes FP16 addition order. The host prepares static indices and
offsets before timing, rejecting non-ternary codes. The simple fixed-size
representation uses 11,796,480 index bytes plus 1,843,200 offset bytes for
the up projection, compared with 5,898,240 bytes of original packed codes;
both also need weight scales. The prototype has variable-length loops and
indexed activation reads. The measurement does not isolate which costs
caused the slowdown.

The [harness](../wiki/experiments/n20-q2-segment-bench.c) used real layer-0
up weights (9,216 by 2,560), deterministic synthetic FP16 activations and
the C.24 Adreno driver. It ran under the shared GPU lock on efficiency cores
at nice 15 while Minecraft remained running. Each value is the median of
five GPU event spans, from buffer-to-image copy through projection; the
baseline bracketed the candidate. Battery readings before each run were
24.3 °C. Preprocessing, initial transfers and output checking are excluded.

| Tokens | Baseline before | Segment reuse | Baseline after |
| ---: | ---: | ---: | ---: |
| 16 | 7.128 ms | 46.249 ms | 7.117 ms |
| 128 | 55.575 ms | 395.081 ms | 56.417 ms |

At 128 tokens the sampled normalized RMS error against the FP16-input
double-precision reference was 0.000614 for the candidate and 0.001007
for the baseline. The candidate also passed the sampled reference check
on a seven-token layer-0 down-projection tail. These checks do not prove
complete-model quality or equality. Because this projection alone is about
seven times slower before charging preprocessing and integration costs, the
result is sufficient to reject this particular prototype; it does not rule
out a different data layout, an optimized SSR implementation, or a different
device.

Build the counter with `cc -O3 -Wall -Wextra -Werror` and pass a scratch
row-major Q2_0 tensor file, row count and column count. Build the benchmark
using the Android NDK and OpenCL instructions in
[`docs/wiki-chat.md`](../docs/wiki-chat.md). Run it with the existing FP16
texture source for the baseline or set `N20_SEGMENT=1` and supply the
segment source for the candidate. Use an isolated cache and the shared GPU
lock. No live worker, configuration, model or firmware was changed.
