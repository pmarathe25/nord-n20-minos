# Nord N20 Bonsai attention and dispatch trial, November 10, 2026

## Complete model requests

The Nord N20 ran Bonsai 2 4B Q2_0 on the Adreno 619 with the C.24
OpenCL driver and Android ARM64 llama.cpp b12110. The scratch backend
used the existing tuned Q2_0 OpenCL patch and explicit 2D activation
texture patch. The server used one 1,536-token slot, a 1,536-token batch
limit, a 256-token microbatch, and two CPU threads on cores 0–2 at nice 15.
The benchmark held the shared GPU lock and used a copy of the shader
cache. Minecraft reported zero players at the status check. The installed
Wiki enable marker was absent.

The benchmark rendered the production grounded chat prompt with two
fixed, hand-written sets of Minecraft excerpts. It called the complete
model request over loopback with temperature zero, a 96-token cap, a
blank-line stop and prompt reuse disabled. Each process received a
warmup Observer request before the measured requests. Total time starts
at HTTP submission and ends when the client reads the complete reply.
The model's internal timers separately report prompt evaluation and
decode. The measurement excludes retrieval, chat delivery, and worker
startup.

The Observer question asked the redstone output strength and expected
`15` with excerpt 1. The second question asked the current output of a
private base's comparator and expected abstention. Both F16 modes gave
the correct result for both questions with matching returned token IDs
and normal end-of-generation stops. The timings use the second Observer
request and the first private-state request in each process.

| Attention with F16 KV | Question | Input / output tokens | Prefill | Decode | Complete reply |
| --- | --- | ---: | ---: | ---: | ---: |
| Flash Attention, first reference | Observer | 292 / 26 | 28.50 s | 10.29 s | 38.80 s |
| Unfused attention | Observer | 292 / 26 | 29.40 s | 9.18 s | 38.59 s |
| Flash Attention, second reference | Observer | 292 / 26 | 28.37 s | 10.20 s | 38.58 s |
| Flash Attention, first reference | Private comparator | 295 / 19 | 29.23 s | 7.34 s | 36.59 s |
| Unfused attention | Private comparator | 295 / 19 | 30.16 s | 6.61 s | 36.78 s |
| Flash Attention, second reference | Private comparator | 295 / 19 | 29.13 s | 7.35 s | 36.48 s |

Unfused attention moved about one second from decode to prefill. The
complete warmed requests differed by at most 0.31 s. These are individual
requests on two short contexts. Battery readings at the start of the timed
requests ranged from 25.0 to 26.5 °C.

The server logs also give the initial process-to-listening interval and
time through its first complete Observer generation. The first request
was submitted after polling health.

| Attention with F16 KV | Start to listening | Start to first complete generation |
| --- | ---: | ---: |
| Flash Attention, first reference | 20.19 s | 59.41 s |
| Unfused attention | 8.65 s | 47.72 s |
| Flash Attention, second reference | 18.12 s | 57.16 s |

The startup difference applies to this build, which had a private copy
of the eager shader cache. A separate trial of the lazy Flash Attention
cache patch reached 7.47 s to health with Flash Attention enabled.
[Earlier lazy shader-cache trial](2026-11-10-wiki-fa-cache.md)

With Flash Attention enabled and both KV types set to `q8_0`, the
worker loaded and began the first 292-token Observer request. Its log
reached 288 prompt tokens at an 18.27 s progress checkpoint. The first
request reached the 140 s client deadline without a reply, compared with
38.73 s for the corresponding F16 warmup. The worker logged cancellation.
The kernel subsequently reported a KGSL GPU fault for that worker's
OpenCL context. The worker was reaped, its listener closed, and phone
uptime continued. This trial established a failure of the Q8 configuration
on the tested C.24 stack. It did not isolate the failed command or
measure warmed Q8 throughput. [Pinned llama.cpp OpenCL attention dispatch](https://github.com/ggml-org/llama.cpp/blob/b12110/ggml/src/ggml-opencl/ggml-opencl.cpp)

## Whole-request dispatch trace

A separate `GGML_OPENCL_PROFILING=ON` build recorded OpenCL events for a
warm Observer request with F16 KV and Flash Attention. It returned the
same generated token IDs as the uninstrumented reference and finished
in 38.83 s. The queue recorded 33,981 kernel dispatches. Splitting at
the first vocabulary projection gives the following GPU measurements.

| Phase | Dispatches | First start to last end | Summed kernel execution | Q2_0 projections | Gated DeltaNet | Flash Attention kernels |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Prefill | 4,181 | 28.482 s | 28.340 s | 18.656 s | 3.476 s | 0.823 s |
| Decode | 29,800 | 10.253 s | 9.772 s | 6.111 s | 0.465 s | 1.820 s |
| Whole request | 33,981 | 38.778 s | 38.113 s | 24.766 s | 3.941 s | 2.643 s |

The first-to-last GPU interval contains 0.665 s between recorded
kernels. The complete request has 0.721 s outside their summed execution,
or 1.9% of wall time. That residual also includes unrecorded transfers,
sampling, waits and HTTP work, so it cannot isolate host launch cost.
Event collection can also affect timings. Removing the entire residual
in this trace would save under a second, while halving projection
execution with everything else fixed would save about 12.4 s. Those are
arithmetic estimates from one instrumented request.

Decode issued 1,192 kernels per forward pass. The driver's 512-command
recording limit would require multiple recordings per pass. Previous
three-command FFN replay was slightly slower on a complete model run,
and fusing gate/up plus SwiGLU increased prefill time. OpenCL's
workgroup barrier synchronizes within one workgroup, so a persistent
kernel spanning dependent projections across workgroups would need a
separate synchronization design. Qualcomm also advises limiting
kernel duration because long-running kernels can trigger a GPU reset.
[Earlier FFN replay](2026-11-09-wiki-ffn-replay.md) · [Earlier fused FFN](2026-11-09-wiki-graph.md) · [OpenCL C synchronization](https://registry.khronos.org/OpenCL/specs/unified/html/OpenCL_C.html#synchronization-functions) · [Qualcomm OpenCL guide, sections 3.2.5 and 3.3.2](https://docs.qualcomm.com/doc/80-NB295-11/80-NB295-11_REV_C_Qualcomm_Snapdragon_Mobile_Platform_Opencl_General_Programming_and_Optimization.pdf)

Fusion that reduces computation or memory traffic has more room than
launch elimination. Q2_0 projections were 65.0% of recorded execution.
The F32 recurrent kernel was 10.3%, mostly during prefill. Its existing
OpenCL implementation advances tokens sequentially inside each workgroup
while retaining the state shard in registers. Chunk-parallel recurrence
has a published formulation, with different FP32 reduction order and
hardware requirements that would need fresh tests on this device. FWHT and
activation transpose together accounted for 0.881 s of recorded kernels.
[Pinned recurrent kernel](https://github.com/ggml-org/llama.cpp/blob/b12110/ggml/src/ggml-opencl/kernels/gated_delta_net.cl) · [Gated DeltaNet paper](https://arxiv.org/abs/2412.06464)

The measurements used scratch workers. No backend or service changes
were deployed. The phone scratch directory and workstation trace files
were removed after recording the results. The workstation scratch build
directory was moved to the Recycle Bin after recording the results.
