# Nord N20 Wiki chat and accelerator trial

Tested November 9, 2026 local time on the Nord N20 running MinOS, Debian 13
ARM64, C.24 kernel 5.4.254, and Minecraft Java 26.3. Inference used the same
Ternary Bonsai 2 4B `Q2_0` checkpoint as the desktop trial. The production
server was idle during the probes.

| Path | Result |
| --- | --- |
| ARM64 Wiki retrieval | 97 parsed articles indexed. 3.63 ms median for 30 repeated warm requests after one priming request. A cold HTTPS question fetched and indexed two pages. A forced revision check found both unchanged. |
| Chat bridge | A protocol client sent `@wiki` to a disposable 26.3 server on loopback. The bridge used real Rust evidence and a deterministic model stub. The client received a private answer with a clickable Minecraft Wiki section link. |
| CPU inference | llama.cpp b12110 on four efficiency cores at reduced priority. The short redstone prompt took 41.5 seconds for 22 input and 39 output tokens, with 1.70 prompt tokens/s and 1.33 generated tokens/s. |
| Adreno setup | The C.24 OTA supplied the missing `a630_sqe` and `a615_zap` files. KGSL opened and returned chip ID `0x06010901`. A native Mesa 26.1.6 Turnip build exposed Adreno 619 to Vulkan. |
| Adreno llama.cpp | Vulkan device enumeration passed. Model loading failed because Turnip reports `storageBuffer16BitAccess=false` and llama.cpp requires it. |
| Stock OpenCL | The matching C.24 Android libraries exposed Adreno 619 OpenCL 2.0 after bridging two missing API symbols. The b12110 CLI reached GPU kernel compilation, where the stock compiler rejected `dot_acc_sat_4x8packed_ss_int`. It produced no model throughput sample. |
| Custom Adreno kernel | One packed `Q2_0` projection ran on the actual GPU using 32-bit storage buffers. Three real feed-forward weight matrices matched the scalar reference. The 9,216 × 2,560 up projection went from a 14.55 ms median in the one-row implementation to 3.97 ms with eight rows per workgroup. The down and gate projections took 4.21 and 3.97 ms. Largest absolute error across these final samples was under 0.00000024. Each reported sample averaged 25 warm dispatches. |
| Hexagon | The mounted ADSP and CDSP firmware ELF headers both target v66. The b12110 Hexagon backend ships v73 and newer kernels. The vendor FastRPC node opened, but the CDSP capability ioctl returned `ENODEV` even after CDSP reported online. The phone rebooted during that probe. |

The phone returned over USB SSH after the DSP reboot. Its clock had reset.
The clock was restored from the workstation over authenticated SSH, NTS
resynchronized, and Minecraft returned online with the whitelist enabled.
There was no persisted panic trace to establish the precise reboot cause.
DSP activation tests stopped after the reboot.

The native Vulkan run proves that the custom projection kernel can access the
GPU and produce correct results. It omits prompt batching, attention,
recurrent state, sampling, and other operations needed for an end-to-end
answer. Repeated dispatches use one resident matrix, so the numbers do not
predict full-model throughput or production game tick latency.

## Reusable Rust kernels and native instruction access

The Rust Vulkan host reused the projection shader and its resident weight
format. Alternating three 100-dispatch samples per host gave 3.953 ms median
per 9,216 × 2,560 projection in Rust and 3.954 ms in C. The reusable attention
module matched a scalar reference for all tested shapes. A separate dispatch
changed the visible prefix and a cache value using the original pipeline
and descriptor set. A two-dispatch chain also passed with the second
attention operation consuming the first operation's GPU output.

| Decode attention, one query token | Warm dispatch |
| --- | --- |
| 16 query heads, 4 KV heads, width 128, 1 of 7 keys visible | 0.254 ms |
| 16 query heads, 4 KV heads, width 128, 257 of 512 keys visible | 0.989 ms |
| 16 query heads, 4 KV heads, width 256, 512 keys | 3.428 ms |
| 8 query heads, 8 KV heads, width 64, 2,048 keys | 2.603 ms |

The largest absolute attention error was below `3e-8` on synthetic vectors.
The width-256 case matches Bonsai's attention head geometry. Bonsai also
contains recurrent blocks and checkpoint-specific Hadamard transforms.

Turnip dumped native A6xx instructions for the packed Vulkan projection.
The driver then accepted the dumped assembly through its IR3 override
interface and the GPU reproduced the reference result. This confirms that
hand-tuned Adreno assembly can be tested while Vulkan manages dispatch.
The tested override was unchanged assembly. Mesa's Computerator expects an
MSM DRM render node. The installed kernel provides KGSL for compute and a
separate DRM display node.

Clang and Zig produced a Hexagon ELF object with v66 flags and a 128-byte
HVX `vrmpy` instruction from the compile-only C probe. There is no on-phone
HVX timing or validation result because the live transport remains unusable.

The phone retains the verified checkpoint, parsed cache, Turnip driver, and
Rust probe under the Wiki deployment paths. The production enable marker
is absent. Minecraft was online with its whitelist enabled at the end of
the trial.

- [OnePlus C.24 OTA](https://android.googleapis.com/packages/ota-api/package/c9df99a58a0a6b3d447f6703c33d424872d3778e.zip)
- [llama.cpp b12110 Vulkan device checks](https://github.com/ggml-org/llama.cpp/blob/b12110/ggml/src/ggml-vulkan/ggml-vulkan.cpp)
- [llama.cpp b12110 Hexagon targets](https://github.com/ggml-org/llama.cpp/blob/b12110/ggml/src/ggml-hexagon/CMakeLists.txt)
- [LLVM Hexagon ISA flags](https://github.com/llvm/llvm-project/blob/main/llvm/include/llvm/BinaryFormat/ELF.h)
- [Mesa IR3 notes](https://docs.mesa3d.org/drivers/freedreno/ir3-notes.html)
- [Qualcomm v66 HVX reference](https://docs.qualcomm.com/doc/80-N2040-44/80-N2040-44_REV_B_Qualcomm_Hexagon_V66_HVX_Programmers_Reference_Manual_.pdf)
