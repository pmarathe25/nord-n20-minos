# Hexagon token-block traversal

2026-11-10. A scratch GPU–Hexagon projection compared two 128-token RPCs against a single 256-token RPC with two ways of traversing the input. The earlier one-call implementation handled each row tile before switching to the next tile, processing both token halves for each tile. The new implementation instead processes the first 128-token block across every row tile, followed by the remaining block. A new strided HVX output routine writes the same canonical `[row tile][token][row]` layout directly. It does not allocate a second output or add a host scatter.

The phone used real Bonsai layer-0 up-projection weights, deterministic activations, 3,072 of 9,216 output rows on Hexagon, and the remaining rows on Adreno. GPU reduction, Q8 packing, synchronization, both compute shards, FastRPC, and output assembly are included. The two-call baseline bracketed both single-call variants. Each value is the median of five samples.

| Variant, 256 tokens | Mixed projection wall time | Representative DSP kernel time |
| --- | ---: | ---: |
| Two RPCs, before | 84.616 ms | 66.5–67.5 ms |
| One RPC, row tiles outermost | 219.259 ms | 206.7–206.8 ms |
| One RPC, token blocks outermost | 84.878 ms | 66.8–67.6 ms |
| Two RPCs, after | 85.546 ms | 66.6–67.3 ms |

Reordering eliminates the previously observed single-call penalty, a roughly 2.6× speedup of that single-call projection. It does **not** establish an improvement over the already-faster two-call implementation, and the GPU shard still takes about 74–75 ms. The measurements do not isolate whether cache behavior, code generation, or another effect caused the change. They do not measure a full model, answer quality, gameplay, or complete reply time; no runtime or service was installed.

For each 256-token arm, all 786,432 DSP integer results matched the scalar reference. Their complete canonical output buffers also had the same FNV-1a hash; the GPU packed bytes, shared-memory visibility, GPU shard and assembly checks passed. Five alternating-input checks passed. The new path also passed all 396,288 and 783,360 DSP integer comparisons at 129 and 255 tokens, respectively. Full scalar equality and output hashes here cover the tested inputs, not arbitrary workloads. The float comparison retains the existing approximate Q8 activation path (256-token normalized RMS error 0.003927).

The experiment uses `wiki/experiments/v66-q2-hvx-strided.c` and `wiki/experiments/v66-q2-rpc-token-outer-256.c`. The unchanged 128-token path remains in the strided kernel. During the guard, available memory stayed at or above 4,359,180 KiB and battery temperature at or below 26.7 °C. Minecraft was restored and passed its status query; CDSP returned offline with zero recorded crashes. Firmware, partitions, installed services and world data were not changed.
