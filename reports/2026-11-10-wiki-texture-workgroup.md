# Nord N20 texture-prefill workgroup screen, November 10, 2026

Changing the workgroup shape of the existing Q2_0 activation-texture
projection did not produce a stable enough improvement to warrant a
whole-model change. The tested shapes preserved complete output hashes.
No backend or live service was changed.

## Two-dimensional workgroups

The C.24 Adreno 619 ran the existing four-row/eight-token texture kernel
against real layer-0 Bonsai 2 4B Q2_0 up and down weights and deterministic
FP16 inputs. The baseline workgroup had one token-tile lane and 128 row
lanes. The candidate had four token-tile lanes and 32 row lanes; each
work item still computed exactly the same four rows and eight tokens with
the same arithmetic. The trial used 128 work items in both cases and only
batch sizes divisible by 32 tokens. A production implementation would
need a safe fallback or bounds handling for other batch sizes.

The harness included the GPU buffer-to-image copy and kernel execution,
but excluded host packing, allocations, compilation and output checking.
Each number is the median of five GPU events in one process. Baseline and
candidate ran in before/candidate/candidate/after order on CPUs 0–2 at
nice 15, with a private cache and the shared GPU lock. Clocks and thermals
were not controlled. [Qualcomm's OpenCL guide](https://docs.qualcomm.com/doc/80-NB295-11/80-NB295-11_REV_C_Qualcomm_Snapdragon_Mobile_Platform_Opencl_General_Programming_and_Optimization.pdf)
recommends measuring workgroup shapes rather than assuming one is optimal.

| Projection | Tokens | Baseline before | Candidate 1 | Candidate 2 | Baseline after |
| --- | ---: | ---: | ---: | ---: | ---: |
| Up | 64 | 28.015 ms | 28.200 ms | 27.806 ms | 28.163 ms |
| Up | 128 | 55.606 ms | 56.050 ms | 55.489 ms | 55.469 ms |
| Up | 256 | 112.949 ms | 111.887 ms | 112.126 ms | 112.482 ms |
| Down | 64 | 28.823 ms | 28.619 ms | 28.606 ms | 28.817 ms |
| Down | 128 | 56.969 ms | 56.622 ms | 55.971 ms | 56.026 ms |
| Down | 256 | 111.558 ms | 111.171 ms | 111.794 ms | 112.624 ms |

The differences were small compared with the baseline drift and repeated
candidate variation. Complete FNV-1a output hashes matched within each
batch and projection; sampled double-precision checks also passed. A
separate preliminary screen of two, four, eight and sixteen token-tile
lanes at 128 tokens likewise produced matching hashes, without a clear
winner. At 16 tokens, two token-tile lanes were close to baseline. These
isolated projections do not establish an end-to-end latency improvement.

## Other exploratory variants

At 16 and 128 tokens, the initial screen also varied one-dimensional
workgroups between 64, 128, 256 and 512 lanes, omitted the Qualcomm
required-subgroup qualifier, and tried a four-token rather than an
eight-token register tile. The larger workgroups and four-token tile
were generally slower; the subgroup and 64-lane results were close
to the baseline. These variants retained matching complete hashes.

An unsigned-code variant accumulated a separate activation sum per
64-column block and subtracted it from each of four row accumulators,
avoiding the per-weight subtraction of one. It changes FP16 rounding.
For 128-token up, its median was 54.865 ms and its sampled normalized
RMS error was 0.002200, compared with the baseline's roughly 55–56 ms
and 0.001007 error. For down, it took 56.204 ms with error 0.002267,
against baseline medians of 56.297 and 56.062 ms and error 0.001008.
The tiny, inconsistent timing difference and increased error did not
justify a complete-model trial.

These experiments did not change or deploy a kernel. They leave
projection math and memory traffic as the main measured opportunities
in the [whole-request trace](2026-11-10-wiki-attention-dispatch.md),
rather than establishing that this workgroup adjustment helps replies.
Phone scratch was removed, workstation scratch was moved to the Recycle Bin,
and Minecraft's Java process remained running.
