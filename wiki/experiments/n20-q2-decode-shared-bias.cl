#pragma OPENCL EXTENSION cl_khr_fp16 : enable

#ifndef N20_HALF_DECODE
#define N20_HALF_DECODE 0
#endif
#ifndef N20_DECODE_ROWS
#define N20_DECODE_ROWS 0
#endif

#if N20_DECODE_ROWS == 16
#define ROWS 16
#define LOAD_D vload16
#define STORE_Y vstore16
#define FLOAT convert_float16
#define HALF convert_half16
#define LOAD_Q(j) as_uchar16(((global const uint4 *)codes)[(j) * (rows / ROWS) + tile])
typedef uchar16 codes_t;
typedef float16 sums_t;
typedef half16 half_sums_t;
#elif Q2_0_MV_WIDE
#define ROWS 8
#define LOAD_D vload8
#define STORE_Y vstore8
#define FLOAT convert_float8
#define HALF convert_half8
#define LOAD_Q(j) as_uchar8(((global const uint2 *)codes)[(j) * (rows / ROWS) + tile])
typedef uchar8 codes_t;
typedef float8 sums_t;
typedef half8 half_sums_t;
#else
#define ROWS 4
#define LOAD_D vload4
#define STORE_Y vstore4
#define FLOAT convert_float4
#define HALF convert_half4
#define LOAD_Q(j) as_uchar4(((global const uint *)codes)[(j) * (rows / ROWS) + tile])
typedef uchar4 codes_t;
typedef float4 sums_t;
typedef half4 half_sums_t;
#endif

/* Compute each activation block sum once, rather than once per row tile.
 * Keep the original four FP32 accumulation chains and final addition order. */
kernel void n20_q2_decode_bias64(global const float * input, ulong input_offset,
                                 global float * bias, int columns, int input_stride) {
    const uint block = get_global_id(0), token = get_global_id(1);
    if (block >= (uint)columns / 64) return;
    global const float * x = input + input_offset / sizeof(float) + token * (ulong)input_stride;
    float4 sum = 0;
    for (uint i = 0; i < 16; ++i) sum += vload4(block * 16 + i, x);
    bias[token * ((uint)columns / 64) + block] = (sum.s0 + sum.s1) + (sum.s2 + sum.s3);
}

kernel void kernel_gemv_noshuffle_q2_0_f32(
    global const uchar * codes,
    global const half * scales,
    global const float * input,
    ulong input_offset,
    global float * output,
    ulong output_offset,
    int columns,
    int rows,
    int input_stride,
    int output_stride,
    global const float * block_bias
) {
    const uint lanes = get_local_size(0);
    const uint splits = get_local_size(1);
    const uint lane = get_local_id(0);
    const uint split = get_local_id(1);
    const uint tile = get_group_id(0) * lanes + lane;
    const uint token = get_group_id(1);
    const uint row = tile * ROWS;
    global const float * x = input + input_offset / sizeof(float) + token * (ulong)input_stride;
    sums_t total = 0;

    if (row < (uint)rows) {
        for (uint block = split; block < (uint)columns / 64; block += splits) {
            sums_t partial = 0;
#if N20_HALF_DECODE
            for (uint group = 0; group < 16; group += 4) {
                half_sums_t accum = 0;
                #pragma unroll
                for (uint i = 0; i < 4; ++i) {
                    const uint j = block * 16 + group + i;
                    const codes_t q = LOAD_Q(j);
                    const half4 a = convert_half4(vload4(j, x));
                    accum += (HALF(q & (codes_t)3) - (half)1) * a.s0;
                    accum += (HALF((q >> (codes_t)2) & (codes_t)3) - (half)1) * a.s1;
                    accum += (HALF((q >> (codes_t)4) & (codes_t)3) - (half)1) * a.s2;
                    accum += (HALF(q >> (codes_t)6) - (half)1) * a.s3;
                }
                partial += FLOAT(accum);
            }
#else
            for (uint i = 0; i < 16; ++i) {
                const uint j = block * 16 + i;
                const codes_t q = LOAD_Q(j);
                const float4 a = vload4(j, x);
                partial += FLOAT(q & (codes_t)3) * a.s0;
                partial += FLOAT((q >> (codes_t)2) & (codes_t)3) * a.s1;
                partial += FLOAT((q >> (codes_t)4) & (codes_t)3) * a.s2;
                partial += FLOAT(q >> (codes_t)6) * a.s3;
            }
            partial -= block_bias[token * ((uint)columns / 64) + block];
#endif
            total += FLOAT(LOAD_D(0, scales + block * (uint)rows + row)) * partial;
        }
    }

    local sums_t parts[512];
    parts[split * lanes + lane] = total;
    barrier(CLK_LOCAL_MEM_FENCE);
    if (split == 0 && row < (uint)rows) {
        for (uint s = 1; s < splits; ++s)
            total += parts[s * lanes + lane];
        STORE_Y(total, 0, output + output_offset / sizeof(float) + token * (ulong)output_stride + row);
    }
}
