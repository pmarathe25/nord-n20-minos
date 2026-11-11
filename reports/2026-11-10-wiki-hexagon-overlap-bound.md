# Optimistic GPU/Hexagon projection overlap

The Nord N20 ran a single 128-token Q2_0 projection with one-third of its
rows on Hexagon and the rest on Adreno in **37.53 ms**, against **55.97–56.00
ms** for the complete projection on Adreno. This is an optimistic projection
bound: both devices' activation formats were prepared before the timer, and
the result conversion and merge were excluded. It is not a model or chat
speedup.

## Method

The [overlap probe](../wiki/experiments/n20-q2-overlap-bench.c) used the
existing C.24 OpenCL and FastRPC stacks, real Bonsai 2 4B layer-0 up weights
(9,216 by 2,560), and the [bounded sixteen-token HVX variant](2026-11-10-hexagon-token16.md).
The 128-token case uses that variant's ordinary eight-token path. A single
host thread enqueued the GPU texture copy and row-shard projection, flushed
the queue, called the DSP synchronously using ION-backed buffers, and then
waited for GPU completion. Each process measured full GPU / concurrent /
full GPU in that order, with one warmup and five timed samples per arm.
The table gives medians of host wall times in milliseconds; GPU timings cover
its copy and kernel, and DSP timings cover the FastRPC call.

| DSP rows | Full GPU before | Concurrent wall | GPU during overlap | DSP RPC during overlap | Full GPU after |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 2,304 | 56.072 | 43.624 | 43.352 | 27.467 | 56.118 |
| 3,072 | 56.000 | 37.532 | 37.277 | 33.540 | 55.971 |
| 3,328 | 56.285 | 37.643 | 37.406 | 36.426 | 56.232 |
| 3,840 | 56.020 | 42.139 | 34.120 | 41.976 | 55.988 |
| 4,608 | 56.537 | 50.401 | 28.285 | 50.246 | 56.509 |

For each split, every GPU-shard float matched the corresponding full-GPU
output bit-for-bit. The DSP's sampled integer outputs matched its scalar
fixed-point reference. Across 1,824 sampled combined outputs, normalized
RMS error relative to the original FP16 inputs was 0.00230 at the 3,072-row
split, compared with 0.00098 for the full-GPU projection. This quantization
error is real and requires whole-model quality testing; the GPU-only match
does not establish equality of the combined result. Inputs were deterministic
values in approximately −1 to 1, not captured live model activations.

The probe packs both activation representations on the CPU and uploads the
weights before timing. It does not measure GPU quantization, range checking,
cross-device buffer visibility, output conversion/scatter, repeated layer
dispatch, model quality, or chat delivery. Those omitted operations could
consume the measured ~18.5 ms margin. The previous whole-gate hook was slower
in a [complete-model trial](2026-11-09-hexagon-projection.md), so this
projection result is a reason for a stricter integration test, not evidence
that chat will improve.

The isolated test stopped the idle-checked Minecraft server, held the shared
GPU lock, used private device nodes and a private vendor mount namespace, and
restored the server afterward. Minimum sampled available memory was
5,022,940 KiB and maximum sampled battery temperature was 25.7 °C. Minecraft
answered a status query; CDSP returned to `OFFLINE` with zero crashes. No
firmware, installed model or live service configuration was changed.
