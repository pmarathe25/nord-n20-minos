# Nord N20 Hexagon projection trial, November 9, 2026

The phone's v66 compute DSP ran the 128-byte HVX kernel through the stock
C.24 FastRPC path established in the [calculator trial](2026-11-09-hexagon-calculator.md).
The tests extracted Q2_0 weights for layer 0 of the installed Ternary Bonsai 2
4B checkpoint and generated reproducible floating-point activations.

The DSP kernel retained the packed 2-bit codes, quantized activations to
signed bytes, and used fixed-point block scales. One vector accumulates
32 output rows. Specialized 1-, 4-, and 8-token paths keep the accumulators
in vector registers and reuse each packed weight load. Each resident matrix
occupied 7.37 MB. Host packing took about 9–13 ms and the initial FastRPC
upload about 19 ms per matrix. Steady-state measurements exclude that setup.
[Qualcomm v66 HVX reference](https://docs.qualcomm.com/doc/80-N2040-44/80-N2040-44_REV_B_Qualcomm_Hexagon_V66_HVX_Programmers_Reference_Manual_.pdf)

## Individual projections

The table shows median milliseconds over the last three of four samples.
Adreno includes host upload and readback. Hexagon includes FastRPC input and
output transfers. The separate DSP kernel timing for the up projection at
128 tokens was 109 ms. These are isolated projection timings with the same
weights and synthetic input, so they differ from the model graph's device
event timings.

| Layer 0 projection | Tokens | Adreno round trip | Hexagon round trip |
| --- | ---: | ---: | ---: |
| Up, 9,216 × 2,560 | 1 | 3.73 ms | 5.14 ms |
| Up | 4 | 8.88 ms | 7.17 ms |
| Up | 16 | 14.83 ms | 24.34 ms |
| Up | 128 | 97.60 ms | 128.01 ms |
| Down, 2,560 × 9,216 | 1 | 3.79 ms | 5.01 ms |
| Down | 4 | 10.77 ms | 7.35 ms |
| Down | 16 | 15.95 ms | 22.33 ms |
| Down | 128 | 98.81 ms | 345.93 ms |

All 30,572 checked outputs matched the scalar integer reference exactly.
Compared with floating-point weights and inputs, the normalized RMS error
was 0.39–0.50% for the full-size matrices. The checker covered every
single-token output and sampled rows for larger batches.

Loop specialization reduced the 128-token up projection from about 400 ms
to 128 ms. Smaller K slices slowed the tested down projection. A second
HVX worker failed with FastRPC status `0x4e`. A temporary Halide turbo vote
was accepted and reset successfully, with no material improvement in the
projection timings. The benchmark code keeps one HVX worker.
[Halide power modes](https://github.com/halide/Halide/blob/f8d34e47ae71315c440ad73c7dc3fcbc0270cae1/src/runtime/HalideRuntimeHexagonHost.h)

## Concurrent gate and up projections

The gate and up matrices from the same layer are independent and have
the same 9,216 × 2,560 shape. With 128 tokens, the baseline computed both
on Adreno. The hybrid test read the common activation from Adreno, ran
the up projection on Adreno while Hexagon computed the gate, then
converted and uploaded the gate output to Adreno. Both weights were
resident before timing.

| 128-token gate and up | Median of four warm samples |
| --- | ---: |
| Both on Adreno | 192.06 ms |
| Gate on Hexagon, up on Adreno | 160.87 ms |

The hybrid reduced wall time for the pair by 16.2%. Its gate output differed
from the Adreno reference by 0.40% normalized RMS. This comparison includes
activation packing and cross-device transfers. It ends before SwiGLU and
the down projection.

## Full model

A scratch build of llama.cpp b12110 detected adjacent Q2_0 gate and up
nodes that shared an activation. For 64–128 tokens it read the activation
back from Adreno, enqueued the up projection, quantized and dispatched the
gate to Hexagon, and uploaded the gate result before continuing the graph.
The phone ran the original 4B checkpoint with the same 128-token synthetic
prefill and two efficiency-core threads in each configuration.

| Full-model 128-token prefill | First pass | Second pass |
| --- | ---: | ---: |
| Adreno only | 27.85 s | 18.50 s |
| Adreno and Hexagon | 32.25 s | 19.34 s |

The hybrid ran 24 gate projections on Hexagon in each pass. The second
pass reused their weights, but was 4.5% slower than the Adreno baseline.
The hook requires host synchronization around each split and duplicates
about 177 MB of weights on the DSP for these layers. The trial did not
isolate the contribution of each cost. The isolated pair's win did not
carry over to the graph, so the experimental hook was removed after the
benchmark. The trial used synthetic prompts and did not measure chat
accuracy or gameplay ticks. Minecraft was saved and stopped during the
DSP tests, and returned online with its whitelist enabled. CDSP returned
to `OFFLINE` with zero reported crashes.

## Rebuild the projection probe

Compile the RPC dispatcher separately so it can acquire a QuRT HVX context
before executing any vector instruction. The DSP resolves the QuRT and HAP
functions through the vendor FastRPC shell at load time.

```sh
zig cc -target hexagon-freestanding-none -mcpu=hexagonv66 -O3 -ffreestanding -fPIC \
    -c wiki/experiments/v66-q2-rpc.c -o v66-q2-rpc.o
zig cc -target hexagon-freestanding-none -mcpu=hexagonv66+hvxv66+hvx_length128b \
    -O3 -ffreestanding -fPIC -c wiki/experiments/v66-q2-hvx.c -o v66-q2-hvx.o
zig cc -target hexagon-freestanding-none -mcpu=hexagonv66+hvxv66+hvx_length128b \
    -O3 -ffreestanding -fPIC -c wiki/experiments/v66-fwht-hvx.c -o v66-fwht-hvx.o
ld.lld -shared --hash-style=sysv --no-rosegment -z max-page-size=4096 \
    -z common-page-size=4096 -z norelro -z separate-loadable-segments -Bsymbolic \
    --enable-new-dtags -rpath / v66-q2-rpc.o v66-q2-hvx.o v66-fwht-hvx.o \
    -o libn20_projection_skel.so
python3 wiki/experiments/tag-v66-elf.py libn20_projection_skel.so
aarch64-linux-android30-clang -O3 -march=armv8.2-a+fp16+dotprod \
    wiki/experiments/v66-q2-bench.c -ldl -lm -o n20-q2-bench
```

Use the [calculator's FastRPC staging](2026-11-09-hexagon-calculator.md),
substituting the projection module and benchmark client. The benchmark
accepts the original row-major GGUF tensor bytes followed by the row and
column counts.

```sh
n20-q2-bench blk.0.ffn_up.weight.q2_0 9216 2560
n20-q2-bench blk.0.ffn_down.weight.q2_0 2560 9216
```
