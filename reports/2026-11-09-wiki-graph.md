# Nord N20 Wiki response latency, November 9, 2026

The phone ran Bonsai 2 4B Q2_0 through llama.cpp b12110 and the C.24
Adreno 619 driver. The live Minecraft server was online with zero players.
Each run used a separate model socket, a copy of the installed Java Edition
Wiki cache, the Rust retriever, and the Python chat bridge under the Minecraft
account. The model worker used two efficiency-core threads at nice 15. The
baseline included the tuned Q2_0 prefill and decode kernels, radix Hadamard,
recurrent copy, and the retained system prefix.

The clock starts when an `@wiki` chat line is appended to a private log and
stops when the bridge writes `tellraw` to a private console FIFO. It includes
retrieval and complete reply generation. The model was already listening
before the first question. This comparison excludes game networking, ticks,
model wake-up time, and network fetches for uncached pages. It ran each case
once per build, in the same order, with a fresh bridge reply cache for each
question. The model stayed up between questions within a run.

| Java Edition question | Tuned baseline | Fused FFN | Single-turn batching | Correct and cited |
| --- | ---: | ---: | ---: | --- |
| Dried kelp block fuel yield | 51.738 s | 53.318 s | 50.674 s | Yes |
| Hopper transfer interval | 29.306 s | 30.248 s | 28.313 s | Abstained |
| Infinity bow with no arrows | 37.615 s | 38.317 s | 36.665 s | Yes |
| Torch light level, existing exact route | 0.044 s | 0.044 s | 0.044 s | Yes |
| Median of three model replies | 37.615 s | 38.317 s | 36.665 s | |

All four replies, including their citation links, were identical across
the three builds. Hopper retrieval returned both the normal eight-game-tick
cooldown and the seven-game-tick exception for items pushed into an empty
hopper. The model abstained with both excerpts available.

## Fusing operations and submitting work together

The fused path matches adjacent gate and up Q2_0 projections with a shared
input followed by SwiGLU. Prefill takes one input transpose and one fused
dispatch, down from two transposes, two projections, and a separate SwiGLU.
Decode dispatches the three operations together. The phone exercised both
paths. Prefill rose by 0.69–1.58 s across the three questions. Decode stayed
near 2.42 tokens/s. The experiment remains separately opt-in. See the
[earlier GPU profile](2026-11-09-wiki-gpu-profile.md) for the projection
work that remains after combining dispatches.

The driver advertised `cl_qcom_recordable_queues` and reported a limit of
512 kernels per recording. Its generic extension-function lookup returned
null. The installed vendor library exported the recording functions and
calling those symbols worked. A probe submitted 128 dependent kernel
dispatches, changed the kernel argument at each step, and checked every
output against the scalar recurrence. Median wall time over seven samples
was 2.159 ms for direct dispatch, 0.495 ms for replay, and 7.742 ms for
recording afresh and submitting. The probe used fixed buffers and work
sizes and timed through `clFinish`. Model graph replay remains untested.
An implementation would need reusable command segments whose buffers,
arguments, and work sizes are valid for the current decode graph.
[Qualcomm OpenCL optimization guide](https://docs.qualcomm.com/doc/80-NB295-11/80-NB295-11_REV_C_Qualcomm_Snapdragon_Mobile_Platform_Opencl_General_Programming_and_Optimization.pdf)

## Scheduling and direct answers

The server patch skips two forced end-of-prompt batch boundaries for
single-user-turn Wiki requests. It retains the boundary at the end of the
system prefix, which saved 106 tokens in the tested run. Prefill fell by
0.95–1.06 s on the three prompts. Decode stayed within 5 ms of baseline on
each request. The median full reply fell by 2.5%. A separate run increased
the prefill microbatch limit from 256 to 384 tokens. The longest question
returned the same answer in 50.832 s, compared with 50.674 s at 256 tokens.
The test kept 256 tokens for the paired comparisons.

The bridge can also answer a narrow class of single-fuel yield questions
from explicit statements in the cited Java Wiki `Fuel` section. It defers
unrecognized statements and conflicting yields to the model. The new route
matched two of 104 existing benchmark questions, both correct. Dried kelp
and coal returned correctly cited answers in 37–75 ms across two private
log-to-console runs with the model socket absent. The dried kelp model
baseline was 51.738 s.

A separate test ran a disposable loopback-only Minecraft Java server on the
phone. A desktop protocol client connected through an SSH tunnel, sent the
dried kelp question as a chat packet, and received the correct private reply
with its clickable source in 253 ms. The test covered a single fast-route
request, with no model process and no comparative gameplay-tick measurement.
