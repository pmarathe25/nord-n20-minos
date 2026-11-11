# Nord N20 activation-texture prefill trial, November 10, 2026

A true two-dimensional FP16 activation texture improved the tested Q2_0
projection by approximately 8–10%, including the GPU buffer-to-image copy.
The arithmetic and complete projection output hashes were unchanged.
No installed backend or service was changed by this experiment.

## Projection method

The C.24 Adreno 619 driver ran layer-0 up and down weights extracted from
the installed Bonsai 2 4B Q2_0 model. The existing projection harness
provided deterministic FP16 activations and sampled double-precision
verification. Runs used cores 0–2, nice 15, a private shader cache and
`/tmp/n20-q2-gpu-benchmark.lock`. Each number is the median of five GPU
launches; baseline runs bracketed each candidate. These are GPU timings,
not calibrated electrical-power measurements.

The tuned reference uses a four-row/eight-token register tile, 64-column
FP16 partial sums and FP32 outer sums. Its activation image is a linear
`image1d_buffer_t` over a transposed FP16 buffer. The candidate uses an
actual `image2d_t` allocated by the driver, with RGBA FP16 pixels,
`padded_tokens / 4` pixels per row and `columns` rows. Direct integer
coordinates select the token quartet and reduction column. The driver
owns the physical row pitch; `clEnqueueCopyBufferToImage` consumes a tightly
packed source and handles the destination layout. See the
[Khronos copy API specification](https://registry.khronos.org/OpenCL/specs/unified/refpages/man/html/clEnqueueCopyBufferToImage.html).

The final projection test includes that GPU copy on every launch. The
reported candidate span begins at copy start and ends at GEMM completion.
It excludes allocation, host upload, weight packing, compilation and
verification, as does the baseline. The full-model test below additionally
includes transpose, allocation and host dispatch overhead.

| Projection | Tokens | Baseline before/after ms | Texture including copy ms | Complete output FNV-1a hash |
| --- | ---: | ---: | ---: | --- |
| Up | 16 | 7.819 / 7.904 | 7.162 | `5df88dd81c87db5f` |
| Up | 128 | 61.106 / 61.855 | 55.466 | `959f94cc3d04f9e1` |
| Down | 16 | 8.345 / 8.353 | 7.623 | `7294f1e941a394ba` |
| Down | 128 | 61.709 / 61.885 | 56.145 | `259a995f55f8ed3b` |

The four-token tail also preserved its full output hash. Up took
2.557/2.550 ms for the bracketing baseline and 2.386 ms for the texture
including copy; down took 3.031/3.017 ms and 2.860 ms respectively.
Hashes were `e32855184c462f9c` and `38cc25ccad8d14c3`; sampled NRMSE
was 0.0009398 and 0.0008008. The earlier separate
[split-K trial](2026-11-09-wiki-split-k.md) reported a lower 2.70 ms
combined span for four-token down. This does not supersede that specialized
small-tail result; the present improvement targets main prefill.

Every baseline/candidate pair had the same complete output hash. Sampled
NRMSE was 0.0009854/0.0010068 for up at 16/128 tokens and
0.0009715/0.0010078 for down. All sampled results were finite.

The initial 128-token up test also compared direct activation-buffer
loads: two `vload4` operations took 84.917 ms and one `vload8` took
92.611 ms, versus a 61.719/61.229 ms bracketing image-buffer baseline.
The same 2D texture accessed through runtime division/modulo of a linear
index took 120.716 ms. Removing that unnecessary coordinate calculation
reduced the texture GEMM to 55.738 ms against 61.388/61.159 ms baselines.
The later copy-inclusive table is the useful comparison. This experiment
changes activation storage and addressing, not the register tile or
weight precision.

## Full-model comparison

A scratch llama.cpp b12110 build received the existing full N20 OpenCL
patch and the additional image-copy trial patch. Both variants used tuned
decode, packing, radix FWHT, direct row copy, the four-token tail and the
wide vocabulary projection. The only selected change was the prefill
activation storage and its added GPU copy. The production service and
backend remained untouched.

The harness used the installed 4B weights, 1,536-token context, a normal
256-token microbatch and greedy sampling. Each prompt ran twice with
context cleared. The table uses second passes, including all activation
transposes, image creation, copies and dispatch overhead. First-pass lazy
compilation and initial model/context setup are excluded. Baselines ran
before their candidates; the suite was not randomized or thermally
controlled. The first baseline populated its private cache and spent
111.2 seconds in model/context setup; its candidate spent 7.1 seconds.
Those setup times are not a fair performance comparison.

| Question | Input tokens | Baseline first-token s | Texture first-token s | Baseline decode s | Texture decode s | Generated tokens including stop |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Food for adult turtles to breed | 272 | 27.5388 | 25.0493 | 32.3297 | 32.1154 | 74 |
| Comparator subtract mode, rear 12 and side 5 | 393 | 40.1426 | 37.0049 | 4.4028 | 4.4253 | 11 |
| Current private respawn-anchor charge | 347 | 35.5755 | 33.1972 | 10.6671 | 10.8447 | 25 |

All 248,320 first-token logits were finite and byte-identical for each
pair. Dumps were overwritten on each repeat, so these byte comparisons
cover the final pass. All generated token sequences matched between
variants on both passes, including the termination token. SHA-256 values
for the paired logit dumps were:

| Question | SHA-256 |
| --- | --- |
| Turtle | `b54bd46ecb74c2c22ae5f7bb82d6a852485ab4834a2ce2e439f5b297e1f2ce85` |
| Comparator | `91edfd353bd7e3884d1aa5313a9386e176bb9fb3c771307d9e603ca0bac0b3b3` |
| Private anchor | `c7e512b0d4b0f51e01b69a6ccd6eaf4fd6b266f914718c11698b6d4642d367b8` |

Warmed first-token improvements were 9.04%, 7.82% and 6.69%, respectively.
Total stopped-reply improvements were 4.52%, 6.99% and 4.76%. Decode was
essentially unchanged; this is primarily a prefill improvement.

The comparator and private-anchor runs used the production 140-token
limit and stopped at the first blank line or EOG. Both stopped at a blank
line rather than the cap. Turtle was measured with the earlier 97-total-token
harness limit and EOG stopping; its 74-token output contained no blank-line
stop before EOG, so the production stopping rules would terminate at the
same point. An initial comparator run reached the earlier cap while
continuing an explanation; it was not counted as a complete answer or used
in the final table. The corrected comparator pair above was rerun with
production stopping.

Matching output does not imply good answers. Both turtle replies repeated
a long support excerpt containing the seagrass rule, then incorrectly
abstained from answering the actual food question. Both comparator replies
were `Support: [2]` followed by `Answer: 4`; the correct subtraction is 7.
Both private-anchor replies correctly abstained from inventing private
server state, although they added an unnecessary support heading. These
existing behavior and formatting problems were preserved, not repaired by
the GPU change. The prompts use actual cached Wiki snippets and an explicit
support-then-answer instruction; these runs directly exercise the model
rather than retrieval or bridge shortcuts. Three matching prompts are not
a general numerical or quality guarantee.

## Reproduction and scope

Apply [the projection harness patch](../wiki/experiments/n20-q2-activation-image-bench.patch)
to a scratch copy of `n20-q2-opencl-bench.c`. It adds `N20_IMAGE2D`,
per-launch GPU copies and complete-output hashes; `N20_BUFFER` selects the
alternative linear-buffer input for those rejected probes. Use the
[texture kernel](../wiki/experiments/n20-q2-activation-image-prefill.cl)
with `N20_IMAGE2D=1`. The source defaults to 64-column partial sums.
Compile with the same Android ARM64 NDK and OpenCL headers/library as the
ordinary harness. The patched harness passed `-O3 -Wall -Wextra -Werror`.

[The model trial patch](../wiki/experiments/llama-b12110-n20-activation-image-trial.patch)
applies after the existing N20 OpenCL patch. With `N20_IMAGE2D=1`, it
allocates a fresh 2D texture for each Q2_0 prefill projection and enqueues
a buffer-to-image copy after the existing transpose. The normal in-order
queue establishes the dependency. OpenCL retains queued objects until
their commands finish; host references are released after enqueue, as described
by [Khronos object lifetime rules](https://registry.khronos.org/OpenCL/specs/unified/refpages/man/html/clReleaseMemObject.html). This
simple trial deliberately includes per-projection allocation overhead.
It does not retain a new global activation cache.

The model opt-in requires both `N20_IMAGE2D=1` and
`N20_Q2_PREFILL_SOURCE` pointing to the texture kernel. To return to the
tuned baseline, unset `N20_IMAGE2D` and select the ordinary 64-partial
prefill source. Do not mix an image2D kernel with an image1D argument.
This is an isolated research patch, not an automatically selected
production path or a claim of compatibility with other devices.

The host model harness uses the existing first-token-logit instrumentation.
For production-style stopping, apply [the supplemental stop patch](../wiki/experiments/n20-model-stop-trial.patch)
after it and pass 139 decode steps: together with the initially sampled
token this caps generation at 140 tokens. It stops at EOG or the first
blank line and records whether a delimiter or the cap ended generation.


This trial has no automatic fallback after an image-allocation or copy
failure: the usual OpenCL error check stops that process. Production
integration would need separate baseline and texture kernels, verified
image limits, graceful fallback and worker-profile/cache fingerprint
coverage. Peak allocation pressure and long-running service behavior were
not measured. The measured model cases establish a useful candidate,
not a completed production integration.

The [launcher follow-up](2026-11-10-wiki-texture-launcher.md) tests the
opt-in profile with a bounded, reusable texture and tuned fallback.

The GPU runs finished and released the shared lock. After verifying the
exact scratch paths, the coordinating agent removed the phone scratch
and sent the Windows scratch directory to the Recycle Bin. No raw tensor
copies, logs, prompts, binaries or shader caches were added to Git.
Installed services and the shared GPU lock were left intact.
