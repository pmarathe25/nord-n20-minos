# Nord N20 Q2_0 tiled prefill and bit-plane trial, November 10, 2026

The phone ran the C.24 Adreno 619 OpenCL driver with layer-0 up and down
weights from the installed Bonsai 2 4B Q2_0 checkpoint. The projection
harness used deterministic synthetic FP16 activations and checked sampled
results against a double-precision calculation. It ran on CPU cores 0–2
at nice 15 with a private cache. A shared lock serialized GPU runs. Each
measurement is the median of five GPU execution events and excludes host
packing, compilation, allocations, uploads and output verification.

The reference was [the tuned prefill kernel](../wiki/experiments/n20-q2-prefill.cl)
with four rows and eight tokens per work item, 128-thread workgroups,
64-column FP16 partial sums and an FP32 accumulator. The four-token batch
used its four-lane tail. Weights had their usual packed-code and separate
scale buffers.

## Register and shared-memory tiles

The 128-token up projection compared two register layouts at each
combination of 2, 4, 8 or 16 rows and 4, 8 or 16 tokens per work item.
The first vectorized the accumulators across tokens and the second
vectorized them across rows. Neither changed the sequence of operations
within each output. The generated four-row, eight-token reference took
61.91 ms across tokens and 66.07 ms across rows. The closest different tile
was four rows by four tokens at 76.94 ms. Eight rows by eight tokens took
152.72 ms across tokens and 157.50 ms across rows. Sixteen rows by sixteen
tokens took 4.37–4.62 s. Register allocation and spill counts were outside
the scope of the experiment.

The shared-memory kernel held the same four-by-eight register tile. It
divided the 128 workgroup threads into one, two, four or eight token
lanes and 128, 64, 32 or 16 row lanes, respectively. For each 64-column
block, it loaded the activation tile from the usual FP16 image, unpacked
the weight tile from its packed bytes when requested, then synchronized
before computing and before reusing local memory. It also tested local storage of only the
activation tile, only the expanded weights, or the activation tile with
the packed weight bytes. This measures expansion fused into a tile load,
complementing the earlier [global expansion trial](2026-11-10-wiki-expanded-trits.md).

| Local-memory contents | Best token lanes per workgroup | Up, 128 tokens |
| --- | ---: | ---: |
| Baseline | 1 | 61.13–61.30 ms |
| Activations only | 1 | 61.30 ms |
| Activations only, sharing across multiple token tiles | 2 | 70.07 ms |
| Expanded weights only | 8 | 79.88 ms |
| Activations and expanded weights | 8 | 181.11 ms |
| Activations and packed weights | 2 | 79.62 ms |

The one-token-lane expanded-weight variants requested at least 64 KiB
of local storage per workgroup and failed to build. All completed
register and local-memory variants reproduced the baseline's complete
output hash, `959f94cc3d04f9e1`. The sampled normalized RMS error
was 0.1007%.

The two-token-lane activation tile also took 9.00 ms on the 16-token up
projection and 9.53 ms on down, compared with 7.78–7.88 and 8.27–8.28 ms
for the respective baselines. It took 70.41 ms on the 128-token down
projection versus 61.48–61.50 ms. The four-token-lane variant with local
activation and expanded weight tiles was slower at both tested widths
and batch sizes.

Changing the weight conversion to an FP16 subtraction matched the baseline
within run variation. Directly assembling the FP16 sign and exponent bits,
using nested selection, and calling explicit FP16 `mad` took 67–79 ms.
An explicit FP16 `fma` run produced no timing within 90 seconds and was
terminated. Unroll factors of four and eight both reached 61.15 ms, within
the range of the bracketing baseline timings.

## Integer activation bit planes

Each group of 64 weights was repacked as a nonzero mask and a negative
mask. Two 32-bit words per mask kept the packed weight size at 16 bytes
per group. The packer rejected code 3, which is outside the ternary range
used by these tensors. The host quantized each activation group
symmetrically to 4, 6 or 8 signed bits and transposed it into two 32-bit
words per bit plane, with one FP32 activation scale per group and token.

For each plane, the kernel counted bits in the intersection of the
nonzero mask and the XOR of the negative mask with the activation bits.
It subtracted the sign plane, doubled and added the remaining planes in
descending order, then added the negative mask's population count once.
The resulting integer dot product was scaled into an FP32 sum. Each work
item computed four rows by four tokens. The reference for error remained
the original FP16 activation buffer.

| Projection | Tokens | Tuned FP16 | 4-bit | 6-bit | 8-bit |
| --- | ---: | ---: | ---: | ---: | ---: |
| Up | 16 | 7.78–7.88 ms | 14.94 ms | 11.38 ms | 13.77 ms |
| Down | 16 | 8.27–8.28 ms | 15.04 ms | 11.31 ms | 13.52 ms |
| Up | 128 | 61.31–61.32 ms | 118.98 ms | 88.82 ms | 108.79 ms |
| Down | 128 | 61.48–61.50 ms | 114.79 ms | 87.59 ms | 106.00 ms |

The sampled normalized RMS error was 6.69–7.75% at 4 bits, 1.53–1.63%
at 6 bits and 0.37–0.42% at 8 bits. The tuned kernel was at or below
0.101% on these inputs. The measured cost of the fastest bit depth,
6 bits, was 37–46% above the tuned kernel before activation quantization.

## Split-K reduction inside one workgroup

Two, four or eight lanes per output tile each processed a strided
sequence of whole 64-column blocks. Each lane applied the block scale
and accumulated in FP32, then wrote its result to local memory. After
one barrier, the first lane summed the parts and wrote the output. This
avoided a separate reduction dispatch and global partial-output buffer.
A register shuffle variant failed to compile because the C.24 compiler
rejected `cl_qcom_subgroup_shuffle` for this device.
[Qualcomm OpenCL guide](https://docs.qualcomm.com/doc/80-NB295-11/80-NB295-11_REV_C_Qualcomm_Snapdragon_Mobile_Platform_Opencl_General_Programming_and_Optimization.pdf)

| Projection | Tokens | Tuned FP16 | Fastest local split-K | Splits |
| --- | ---: | ---: | ---: | ---: |
| Up | 4 | 2.56–2.57 ms | 2.75 ms | 2 |
| Down | 4 | 3.00–3.02 ms | 2.82 ms | 4 |
| Up | 8 | 4.01–4.04 ms | 5.33 ms | 2 |
| Down | 8 | 4.64–4.68 ms | 5.36 ms | 4 |
| Up | 16 | 7.78–7.88 ms | 10.20 ms | 2 |
| Down | 16 | 8.27–8.28 ms | 10.00 ms | 2 |
| Up | 128 | 61.31–61.32 ms | 82.30 ms | 2 |
| Down | 128 | 61.48–61.50 ms | 79.60 ms | 2 |

The sampled normalized RMS error stayed within 0.080–0.101%. The sole
improvement, the four-token down projection, was still slower than the
2.70 ms combined GPU span reported in the earlier
[two-kernel split-K trial](2026-11-09-wiki-split-k.md). The trial stopped
at projection testing. Scratch weights, logs, kernels, binaries and the
private shader cache were removed after recording the results.
