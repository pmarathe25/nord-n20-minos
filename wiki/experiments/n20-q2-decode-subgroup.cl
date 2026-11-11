#pragma OPENCL EXTENSION cl_khr_fp16 : enable
#pragma OPENCL EXTENSION cl_khr_subgroups : enable
#pragma OPENCL EXTENSION cl_qcom_subgroup_shuffle : enable
#pragma OPENCL EXTENSION cl_qcom_reqd_sub_group_size : enable

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

__attribute__((qcom_reqd_sub_group_size("full")))
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
    int output_stride
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
            float4 bias = 0;
            for (uint i = 0; i < 16; ++i) {
                const uint j = block * 16 + i;
                const codes_t q = LOAD_Q(j);
                const float4 a = vload4(j, x);
                partial += FLOAT(q & (codes_t)3) * a.s0;
                partial += FLOAT((q >> (codes_t)2) & (codes_t)3) * a.s1;
                partial += FLOAT((q >> (codes_t)4) & (codes_t)3) * a.s2;
                partial += FLOAT(q >> (codes_t)6) * a.s3;
                bias += a;
            }
            partial -= (bias.s0 + bias.s1) + (bias.s2 + bias.s3);
#endif
            total += FLOAT(LOAD_D(0, scales + block * (uint)rows + row)) * partial;
        }
    }

    // This experimental path requires 32 row lanes, 16 splits, and 128-lane
    // Qualcomm subgroups. The host must dispatch only that exact geometry.
    // Keep all lanes active, including row tails, through subgroup operations.
    for (uint mask = 32; mask <= 64; mask *= 2) {
        sums_t other;
        #define SHUFFLE(c) other.s##c = qcom_sub_group_shuffle_xor(total.s##c, mask, CLK_SUB_GROUP_SHUFFLE_WIDTH_WAVE_SIZE_QCOM, total.s##c)
        SHUFFLE(0); SHUFFLE(1); SHUFFLE(2); SHUFFLE(3);
#if ROWS >= 8
        SHUFFLE(4); SHUFFLE(5); SHUFFLE(6); SHUFFLE(7);
#endif
#if ROWS == 16
        SHUFFLE(8); SHUFFLE(9); SHUFFLE(a); SHUFFLE(b);
        SHUFFLE(c); SHUFFLE(d); SHUFFLE(e); SHUFFLE(f);
#endif
        #undef SHUFFLE
        total += other;
    }
    local sums_t parts[128];
    if ((split & 3) == 0)
        parts[(split / 4) * lanes + lane] = total;
    barrier(CLK_LOCAL_MEM_FENCE);
    if (split == 0 && row < (uint)rows) {
        total += parts[lanes + lane];
        total += parts[2 * lanes + lane];
        total += parts[3 * lanes + lane];
        STORE_Y(total, 0, output + output_offset / sizeof(float) + token * (ulong)output_stride + row);
    }
}
