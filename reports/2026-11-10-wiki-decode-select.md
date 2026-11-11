# Nord N20 ternary decode selection trial, November 10, 2026

Replacing the tuned Q2_0 decode arithmetic with ternary selection was about
2.85 times slower on the Adreno 619. Constant-address-space activations and
explicit inner-loop unrolling gave no clear improvement. These projection
tests did not proceed to a complete model or deployment.

The scratch Android ARM64 harness used the C.24 OpenCL driver, the existing
`n20-q2-opencl-bench.c`, and real Bonsai 2 4B layer-0 up and down weights.
It used one synthetic activation token, eight output rows per work item,
512-thread workgroups and 16 input splits, matching the tuned feed-forward
decode geometry. Processes ran at nice 15 on CPUs 0–2, with a private cache
and the shared `/tmp/n20-q2-gpu-benchmark.lock` held throughout each sweep.

The baseline accumulates unsigned codes times activations in FP32, then
subtracts the activation sum once per 64-column block. The selection
variant instead uses two vector `select` operations to choose negative
activation, zero or positive activation for each ternary code. It retains
FP32 accumulation, group scales, packing and launch geometry. Both extracted
matrices were checked and contained zero occurrences of reserved code 3;
the experimental selection expression does not implement that code.
The [Khronos OpenCL C specification](https://registry.khronos.org/OpenCL/specs/unified/html/OpenCL_C.html)
defines vector selection by the mask's most significant bit. The trial
converted signed comparison masks to matching 32-bit integer vectors.

Each invocation timed five launches with OpenCL events. Each shape ran
baseline, selection, selection, baseline; the table pools ten launches
per mode. Times exclude compilation, packing, allocation and transfers.

| Projection | Baseline median | Selection median |
| --- | ---: | ---: |
| Up, 9,216 × 2,560 | 1.192 ms | 3.4135 ms |
| Down, 2,560 × 9,216 | 1.2035 ms | 3.4305 ms |

The scalar double-precision reference checked 96 up outputs and 27 down
outputs. Baseline and selection returned identical reported normalized RMS
errors: 8.395785e-8 for up and 1.208615e-7 for down. Maximum errors were
1.648441e-7 and 5.830079e-7. These sampled checks do not establish equality
of every output or correctness for arbitrary activations or models.

A second sweep tested changing only the input pointer from `global` to
`constant`, and changing only the 16-iteration inner loop to request full
unrolling. Its order was baseline, constant, unroll, unroll, constant,
baseline for each shape, again five launches per invocation.

| Projection | Baseline median | Constant input median | Unrolled median |
| --- | ---: | ---: | ---: |
| Up | 1.196 ms | 1.1965 ms | 1.195 ms |
| Down | 1.202 ms | 1.195 ms | 1.204 ms |

All sampled numerical errors again matched the baseline. The small timing
differences overlap the variation between repeated baseline invocations;
they do not support a meaningful speed claim. The sweeps did not control
thermal state, measure power or active Minecraft tick latency. Scratch
weights, kernels, executable and cache were removed after recording results.
