# Nord N20 accelerator follow-up, November 9, 2026

These trials used the Nord N20's C.24 Adreno OpenCL driver, llama.cpp
b12110, and the Bonsai 2 4B Q2_0 checkpoint. Model runs used an isolated
shader cache and two efficiency-core threads at nice 15.

## The 512-point Hadamard transform on Adreno

The new kernel factors each row into contiguous 32-point transforms
followed by strided 16-point transforms. The butterflies run in
registers. A single workgroup barrier separates the two passes.
The fastest of the tested layouts used 32 work items per row.

| 512-point rows | Existing kernel | Register transform | Speedup |
| ---: | ---: | ---: | ---: |
| 64 | 0.798 ms | 0.112 ms | 7.1× |
| 256 | 3.189 ms | 0.439 ms | 7.3× |
| 4,096 | 43.764 ms | 6.940 ms | 6.3× |

The table uses seven GPU events per configuration and the same
synthetic F32 data. All checked elements matched the scalar butterfly.
We also tested the inverse factorization and workgroups spanning
two, four, and eight rows. Those variants were slower on this driver.
The F16-input tests also matched the scalar reference. At 4,096 rows,
F16 input took 7.65 ms with the register kernel and 55.09 ms with the
existing kernel.

The whole-model test rendered the grounded Wiki prompt for a
lightning-rod recipe from a cached Java excerpt. It evaluated 219
tokens before the first sampled token, then generated eight more.
Both configurations used the existing tuned Q2_0 kernels.

| Time to first sampled token | Existing transform | Register transform |
| --- | ---: | ---: |
| First pass | 33.14 s | 31.17 s |
| Median of the two warm passes | 24.17 s | 22.32 s |

The first-token logits hash and all nine sampled token IDs matched
in all six runs. Warm generation of eight more tokens took about
3.36 s in either configuration.

## Single-token vocabulary projection

The 16-row kernel reads the existing packed Q2_0 layout. With 512 work
items per group and four input splits, the single-token vocabulary
projection took 36.67 ms versus 42.15 ms for eight rows, a 256-thread
group and eight splits. These are medians of 15 GPU events per
configuration, interleaved in three pairs with the real 248,320-row
weights. Both kernels had normalized RMS errors below 10⁻⁷
against the double-precision reference on sampled rows.

The whole-model decode test used a three-token prompt followed by
16 decode steps. Warm decode took 6.60 s with the existing layout
and 6.51 s with the wider tile. All sampled token IDs matched.
The first-token logits hash differed with the changed sum order.
The feed-forward shapes retained their eight-row kernels. Centered
weight accumulation, larger work-item tiles and subgroup shuffle
reductions did not improve the two feed-forward probes.

## Hexagon and shared ION buffers

The v66 HVX kernel transforms signed fixed-point values. In a
4,096-row batch it spent about 3.8 ms on the butterflies. With ordinary
FastRPC buffers, the GPU read, ARM conversion, FastRPC call and GPU
write totaled 26.76 ms. Importing the FastRPC ION allocations into
OpenCL reduced a fixed-scale round trip to 6.63 ms. Adreno alone took
7.11 ms in that comparison. The timed shared path did not read or
write the payload on the ARM CPU.

The concurrent probe sends a suffix of rows to Hexagon and launches
Adreno on the independent prefix. Computing a new fixed-point scale
for every row consumed most of the potential overlap. The faster
variant scales in one GPU pass and flags any input outside the
finite range from −32 to 32. The DSP skips that call's transform
when it sees the flag, and the host reruns the whole batch on Adreno.
Each run injected an outlier once and verified that fallback against
the reference. The other samples varied the inputs and verified every
output to check visibility across the shared buffers.

| 512-point rows | Adreno rows | Adreno only | Concurrent with range check |
| ---: | ---: | ---: | ---: |
| 512 | 317 | 1.02 ms | 1.03 ms |
| 1,024 | 512 | 1.84 ms | 1.54 ms |
| 2,304 | 875 | 3.94 ms | 3.04 ms |
| 4,096 | 1,556 | 6.89 ms | 4.83 ms |

Each row uses the fastest measured split for that size and the
Adreno baseline from the same process. These medians exclude the
deliberate fallback and first-use costs. They include pack, queue
submission, FastRPC, overlap and unpack. The largest absolute error
was 2.17 × 10⁻⁵ for the bounded fixed-point samples. The
shared probe measured a synthetic transform with GPU-resident F32
buffers. The whole-model timing above used Adreno alone.

The probe used the existing C.24 firmware and vendor FastRPC stack.
Minecraft stopped through its idle maintenance guard and restarted
after each DSP run. CDSP returned to `OFFLINE` with zero reported
crashes.
[Qualcomm v66 HVX reference](https://docs.qualcomm.com/doc/80-N2040-44/80-N2040-44_REV_B_Qualcomm_Hexagon_V66_HVX_Programmers_Reference_Manual_.pdf)
[OpenCL ION import](https://registry.khronos.org/OpenCL/extensions/qcom/cl_qcom_ion_host_ptr.txt)
[FastRPC memory API](https://raw.githubusercontent.com/qualcomm/fastrpc/development/inc/rpcmem.h)

## Reproduce the shared probe

Build the [v66 projection module](2026-11-09-hexagon-projection.md#rebuild-the-projection-probe)
and stage the C.24 FastRPC runtime as described there. Build the host
probe with the Android ARM64 NDK and the OpenCL headers and link library.

```sh
aarch64-linux-android30-clang -O3 -DN20_FWHT_FASTRPC -I"$OPENCL_HEADERS" \
    wiki/experiments/n20-fwht-bench.c -L"$OPENCL_LIB" -lOpenCL -ldl -lm -o fwht-bench
N20_FWHT_DSP=1 N20_FWHT_SHARED=1 N20_FWHT_GPU_PERCENT=38 \
    fwht-bench n20-fwht-radix.cl 512 4096
```

Setting only `N20_FWHT_DSP=1` measures the round trip through CPU
buffers. The GPU-only probe also accepts `--f16` after the row count.
