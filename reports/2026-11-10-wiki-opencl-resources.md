# Adreno OpenCL kernel resource queries

2026-11-10. The C.24 OpenCL driver compiled the three activation-texture projection sources used in the recent four-row/eight-token baseline, two-row/eight-token experiment, and four-row/sixteen-token experiment. The scratch program queried `clGetKernelWorkGroupInfo` and exported the resulting program binaries. The compiler options matched the standalone timing harness: `-cl-fast-relaxed-math`, `Q2_0_MV_WIDE=0`, `N20_DECODE_ROWS=4`, plus `N20_TILE_TOKENS=16` for the sixteen-token source. All three builds and queries completed twice.

| Kernel | Driver-reported private bytes per work item | Local bytes | Max workgroup | Preferred multiple | Program binary bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| Four rows × eight tokens | 368 | 0 | 1,024 | 64 | 7,148 |
| Two rows × eight tokens | 256 | 0 | 1,024 | 64 | 6,220 |
| Four rows × sixteen tokens | 624 | 0 | 1,024 | 64 | 11,028 |

The OpenCL [specification](https://registry.khronos.org/OpenCL/specs/unified/refpages/man/html/clGetKernelWorkGroupInfo.html) describes the private-memory figure as a per-work-item minimum. It is not a physical register count, occupancy measurement, or spill counter. The sixteen-token source reports more private memory and was over twice as slow in its [projection screen](2026-11-10-wiki-texture-token16.md), consistent with—but not proof of—resource pressure. The two-row source reports less private memory yet was roughly 1.8–2.0 times slower in its [screen](2026-11-10-wiki-texture-row2.md); reducing that number alone does not predict faster execution.

The exported program binaries were opaque driver-specific blobs, not directly recognized by the available ELF reader; no hardware ISA, spill counters, or execution behavior was recovered from them. An earlier single compile of the sixteen-token kernel with a different explicit `-cl-std=CL2.0` option returned `CL_OUT_OF_HOST_MEMORY`; this was not investigated and does not apply to the successful timing-harness option set above. The extractor is `wiki/experiments/n20-opencl-export.c`; its output is an exclusive mode-0600 file and a repeat attempt confirmed that it does not overwrite an existing file.

The guard observed at least 4,351,356 KiB available memory and at most 25.3 °C battery temperature in the final query run. The GPU lock was held, Minecraft remained running, and its status check passed. No installed service or device configuration was changed. Exported binaries and temporary logs were removed.
