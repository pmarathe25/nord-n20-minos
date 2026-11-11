# Nord N20 ternary projection lookup trial, November 9, 2026

The C.24 Adreno 619 driver ran the existing pairwise lookup-table
projection and the tuned `N20_PARTIAL=64` projection with layer 0 weights
from Bonsai 2 4B Q2_0. The inputs were reproducible synthetic FP16 values.
Five GPU event timings were taken per configuration. The phone's Minecraft
server was online with zero players. The measurements cover a projection,
and do not include the rest of the model or a chat reply.

| Projection | Tokens | Tuned median | Pairwise lookup median |
| --- | ---: | ---: | ---: |
| Up, 9,216 × 2,560 | 16 | 7.928 ms | 21.053 ms |
| Up, 9,216 × 2,560 | 128 | 61.250 ms | 166.061 ms |
| Down, 2,560 × 9,216 | 16 | 8.416 ms | 21.766 ms |
| Down, 2,560 × 9,216 | 128 | 62.124 ms | 166.132 ms |

The harness compared sampled outputs with a double-precision reference.
The lookup version's normalized RMS error was 0.00049–0.00057. The tuned
version's error was 0.00097–0.00101. The lookup version was slower on all
four shapes, so it was not used in a complete model test.

Qualcomm's recording API permits updating kernel arguments and work sizes
at submission. That expands the possible designs for reusable graph
segments. The earlier phone recording probe did not exercise argument
updates during replay or replay any model graph. The next investigation
needs to verify those operations and the lifetime of every captured buffer
before comparing complete replies. [Qualcomm OpenCL guide, section 9.1.3](https://docs.qualcomm.com/doc/80-NB295-11/80-NB295-11_REV_C_Qualcomm_Snapdragon_Mobile_Platform_Opencl_General_Programming_and_Optimization.pdf)

A subsequent phone probe recorded one dispatch and replayed it three times.
The middle replay changed a `cl_mem` output argument, a scalar argument,
and the global size from 32 to 64. The third replay explicitly restored the
original buffer, scalar, and size. All 64 values of both output buffers
matched the expected results on every replay. The probe is
`wiki/experiments/n20-record-update.c`. It checks this driver's update ABI
for one kernel, leaving graph lifetime, concurrent replay, and the model
output to be verified separately.
