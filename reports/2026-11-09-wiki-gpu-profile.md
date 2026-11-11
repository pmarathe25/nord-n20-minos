# Nord N20 Bonsai GPU profile, November 9, 2026

The installed Android ARM64 llama.cpp b12110 build ran the same Ternary
Bonsai 2 4B `Q2_0` checkpoint used in the [chat trial](2026-11-09-wiki-end-to-end.md).
The live Minecraft server was online with zero players. The probes used two
efficiency-core threads at reduced priority, the C.24 OpenCL 2.0 driver, and
an isolated benchmark process. KGSL samples reached the configured maximum
GPU clock of 840 MHz during inference.

## GPU event profile

Separate instrumented `llama-bench` runs measured a 128-token prompt and a
16-token generation. The table sums GPU event execution times within each
run. These shares exclude time spent outside recorded kernels, such as CPU
work, queue submission, and waits. The decode probe had a short context.

| Phase | Summed GPU kernel time | Largest contributors |
| --- | ---: | --- |
| Prefill | 18.73 s | Q2_0 batched projections 13.42 s (71.7%), Hadamard transforms 1.67 s (8.9%), gated delta net 1.59 s (8.5%) |
| Decode | 7.48 s | Q2_0 vector projections 4.80 s (64.2%), row gathers 0.67 s (8.9%), attention 0.63 s (8.4%) |

The 9,216-row and 2,560-row output shapes accounted for 9.72 s of batched
projection time and 2.90 s of vector projection time. The 248,320-row
vocabulary projection added 0.85 s across the 16 decode steps. Those three
shapes give a focused set for testing a replacement kernel with real
weights.

The 4B checkpoint already uses specialized Adreno Q2_0 kernels for both
phases. The GPU runs the Hadamard transforms and the recurrent delta-net
kernel as well. The installed driver reports FP16 support and exposes neither
the Khronos nor the Qualcomm packed integer dot-product extension. The tested
prefill path uses transposed FP16 activations and FP32 accumulation. The
compatibility shim permits compilation of sources that refer to the missing
intrinsic. It does not make native packed dot products available.
[Pinned OpenCL implementation](https://github.com/ggml-org/llama.cpp/blob/b12110/ggml/src/ggml-opencl/ggml-opencl.cpp)

## Kernel comparisons

The uninstrumented runs used two repetitions. The table shows the second
repetition to reduce first-use initialization effects. Prefill is 128
synthetic tokens and decode is 16 synthetic tokens. Each alternative
changed one setting or one kernel.

| Configuration | Prefill tokens/s | Decode tokens/s |
| --- | ---: | ---: |
| Installed Adreno kernels | 6.93 | 2.12 |
| Generic Q2_0 layout, `GGML_OPENCL_Q2_0_NOSHUFFLE=0` | No result within 150 s | — |
| Q2_0 decode workgroup tuning, `GGML_OPENCL_Q2_0_MV_NSG=4` | — | 2.15 |
| Q2_0 decode workgroup tuning, `GGML_OPENCL_Q2_0_MV_NSG=8` | — | 2.10 |
| Prefill kernel applying one scale per 64-weight partial sum | 5.24 | — |

The decode setting changed throughput by under 2%. The prefill experiment
reduced scale multiplications by accumulating four separate partial sums,
which increased the live accumulator count. Throughput fell about 24%.
The experiment did not isolate the effect of registers or measure numerical
parity. The installed backend remains the baseline.

A larger speedup requires a different dataflow. On the existing driver,
the most direct experiment is a tiled FP16-input, FP32-accumulating
projection that reuses activations across rows, with a separate
bandwidth-oriented kernel for single-token decode. Measure the dominant
shapes against the current kernels and a scalar reference before testing
a full chat prompt and server tick latency.

## Hexagon

Qualcomm identifies the Snapdragon 695 accelerator as Hexagon 686 with HVX
vector and scalar acceleration. It does not list a separate tensor accelerator
for this SoC. The phone's firmware targets v66 and the pinned llama.cpp DSP
backend starts at v73. A custom v66 HVX port could in principle run coarse
quantized projections, but would first need a functioning FastRPC transport
and an on-device comparison. The earlier transport probe returned
`ENODEV` and the phone rebooted, so this trial did not invoke it. Shared LPDDR
also means a DSP implementation would still compete for memory bandwidth
when streaming model weights.

- [Qualcomm Snapdragon 695 product brief](https://www.qualcomm.com/content/dam/qcomm-martech/dm-assets/documents/product_brief_-_snapdragon_695_5g_mobile_platform.pdf)
- [Pinned Hexagon targets](https://github.com/ggml-org/llama.cpp/blob/b12110/ggml/src/ggml-hexagon/CMakeLists.txt)
- [Previous on-device accelerator trial](2026-11-09-wiki-phone.md)
