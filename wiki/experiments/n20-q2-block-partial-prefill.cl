#pragma OPENCL EXTENSION cl_khr_fp16 : enable

// Experimental image2D input: RGBA FP16, width=tokens_padded/4, height=columns.
// Selected by N20_Q2_TEXTURE_SOURCE with the activation-image worker patch.
// The original trial selected this source with N20_IMAGE2D=1.
// READ_INPUT is expanded inside the reduction loop and uses its direct coordinates.
// Quantized bytes and scales are transposed across output rows.
#ifndef N20_PARTIAL
#define N20_PARTIAL 64
#endif

#ifndef N20_FP32_PARTIAL
#define N20_FP32_PARTIAL 0
#endif

#ifndef N20_TILE_TOKENS
#define N20_TILE_TOKENS 8
#endif

#if N20_PARTIAL != 16 && N20_PARTIAL != 32 && N20_PARTIAL != 64
#error N20_PARTIAL must be 16, 32, or 64
#endif

#if N20_TILE_TOKENS == 4
typedef float4 sum_vec;
typedef half4 input_vec;
#define TO_SUM convert_float4
#define READ_INPUT(image, pos) read_imageh(image, (int2)(token / 4, i + shift / 2))
#elif N20_TILE_TOKENS == 8
typedef float8 sum_vec;
typedef half8 input_vec;
#define TO_SUM convert_float8
#define READ_INPUT(image, pos) (half8)(read_imageh(image, (int2)(token / 4, i + shift / 2)), read_imageh(image, (int2)(token / 4 + 1, i + shift / 2)))
#else
#error N20_TILE_TOKENS must be 4 or 8
#endif

#if N20_FP32_PARTIAL
typedef sum_vec partial_vec;
typedef float4 weight4;
#define TO_PARTIAL TO_SUM
#define TO_WEIGHT convert_float4
#else
typedef input_vec partial_vec;
typedef half4 weight4;
#define TO_PARTIAL(x) (x)
#define TO_WEIGHT convert_half4
#endif

#ifdef cl_qcom_reqd_sub_group_size
#pragma OPENCL EXTENSION cl_qcom_reqd_sub_group_size : enable
__attribute__((qcom_reqd_sub_group_size("full")))
#endif
kernel void kernel_gemm_noshuffle_q2_0_f32(
    global const uchar * codes,
    global const half * scales,
    read_only image2d_t input,
    global float * output,
    ulong offset,
    int rows,
    int tokens_padded,
    int columns,
    int tokens,
    global half * partials
) {
    const int token = get_global_id(0) * N20_TILE_TOKENS;
    const int row = get_global_id(1) * 4;
    if (row >= rows)
        return;

    output += offset / sizeof(float);
    const int input_stride = tokens_padded / 4;
    const int column = get_global_id(2) * 64;
    {
        partial_vec p0 = 0, p1 = 0, p2 = 0, p3 = 0;
        for (int i = column; i < column + N20_PARTIAL; i += 4) {
            const uchar4 code = vload4(0, codes + (i / 4) * rows + row);
            #pragma unroll
            for (int shift = 0; shift < 8; shift += 2) {
                const int pos = (i + shift / 2) * input_stride + token / 4;
                const input_vec value = READ_INPUT(input, pos);
                const partial_vec x = TO_PARTIAL(value);
                const weight4 w = TO_WEIGHT(convert_int4((code >> (uchar4)shift) & (uchar4)3) - 1);
                p0 += x * w.s0;
                p1 += x * w.s1;
                p2 += x * w.s2;
                p3 += x * w.s3;
            }
        }
        #define PART(t) if (token + t < tokens) vstore4((half4)(p0.s##t, p1.s##t, p2.s##t, p3.s##t), 0, partials + ((size_t)get_global_id(2) * tokens + token + t) * rows + row)
        PART(0); PART(1); PART(2); PART(3); PART(4); PART(5); PART(6); PART(7);
        #undef PART
    }

 }

kernel void n20_reduce(global const half * partials, global const half * scales,
                       global float * output, int rows, int tokens, int blocks) {
    const int token = get_global_id(0);
    const int row = get_global_id(1) * 4;
    if (token >= tokens || row >= rows) return;
    float4 sum = 0;
    for (int block = 0; block < blocks; ++block) {
        const float4 part = convert_float4(vload4(0, partials + ((size_t)block * tokens + token) * rows + row));
        const float4 scale = convert_float4(vload4(0, scales + (size_t)block * rows + row));
        sum += part * scale;
    }
    vstore4(sum, 0, output + (size_t)token * rows + row);
}
