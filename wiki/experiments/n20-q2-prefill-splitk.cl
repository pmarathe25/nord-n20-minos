#pragma OPENCL EXTENSION cl_khr_fp16 : enable

// The input image contains FP16 activations transposed to [column][token].
// Quantized bytes and scales are transposed across output rows.
#ifndef N20_PARTIAL
#define N20_PARTIAL 32
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
#define READ_INPUT(image, pos) read_imageh(image, pos)
#elif N20_TILE_TOKENS == 8
typedef float8 sum_vec;
typedef half8 input_vec;
#define TO_SUM convert_float8
#define READ_INPUT(image, pos) (half8)(read_imageh(image, pos), read_imageh(image, pos + 1))
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
    read_only image1d_buffer_t input,
    global float * output,
    ulong offset,
    int rows,
    int tokens_padded,
    int columns,
    int tokens
) {
    const int token = get_global_id(0) * N20_TILE_TOKENS;
    const int row = get_global_id(1) * 4;
    if (row >= rows)
        return;

    output += offset / sizeof(float);
#if N20_SPLITS > 1
    const int split = get_global_id(2);
    const int count = N20_COLUMNS / N20_SPLITS;
    const int start = split * count;
    output += split * rows * tokens;
    codes += (start / 4) * rows;
    scales += (start / 64) * rows;
    const int input_start = start * (tokens_padded / 4);
#else
    const int count = columns;
    const int input_start = 0;
#endif
    const int input_stride = tokens_padded / 4;
    sum_vec y0 = 0, y1 = 0, y2 = 0, y3 = 0;

    for (int column = 0; column < count; column += N20_PARTIAL) {
        const float4 scale = convert_float4(vload4(0, scales + (column / 64) * rows + row));
        partial_vec p0 = 0, p1 = 0, p2 = 0, p3 = 0;
        for (int i = column; i < column + N20_PARTIAL; i += 4) {
            const uchar4 code = vload4(0, codes + (i / 4) * rows + row);
            #pragma unroll
            for (int shift = 0; shift < 8; shift += 2) {
                const int pos = input_start + (i + shift / 2) * input_stride + token / 4;
                const input_vec value = READ_INPUT(input, pos);
                const partial_vec x = TO_PARTIAL(value);
                const weight4 w = TO_WEIGHT(convert_int4((code >> (uchar4)shift) & (uchar4)3) - 1);
                p0 += x * w.s0;
                p1 += x * w.s1;
                p2 += x * w.s2;
                p3 += x * w.s3;
            }
        }
        y0 += TO_SUM(p0) * scale.s0;
        y1 += TO_SUM(p1) * scale.s1;
        y2 += TO_SUM(p2) * scale.s2;
        y3 += TO_SUM(p3) * scale.s3;
    }

    // The C.24 compiler requires named vector lanes.
    #define STORE(t) if (token + t < tokens) \
        vstore4((float4)(y0.s##t, y1.s##t, y2.s##t, y3.s##t), 0, output + (token + t) * rows + row)
    STORE(0); STORE(1); STORE(2); STORE(3);
#if N20_TILE_TOKENS == 8
    STORE(4); STORE(5); STORE(6); STORE(7);
#endif
    #undef STORE
}

#if N20_SPLITS > 1
kernel void reduce_q2_partials(global const float *partials, global float *out, int vectors, ulong offset) {
    const int index = get_global_id(0);
    if (index >= vectors)
        return;
    float4 sum = vload4(index, partials);
    #pragma unroll
    for (int split = 1; split < N20_SPLITS; ++split)
        sum += vload4(index + split * vectors, partials);
    vstore4(sum, index, out + offset / sizeof(float));
}
#endif
