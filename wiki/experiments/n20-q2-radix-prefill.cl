// Experimental Q2_0 x Q8 prefill. Each RGBA32UI pixel holds four tokens;
// each lane packs four unsigned, biased Q8 activations in column order.
// The host supplies per-block activation scales/sums and weight-code sums.
#pragma OPENCL EXTENSION cl_khr_fp16 : enable

#ifndef N20_RADIX_TOKENS
#define N20_RADIX_TOKENS 4
#endif
#if N20_RADIX_TOKENS == 8
typedef uint8 uvec;
typedef int8 ivec;
typedef float8 fvec;
#define READ_INPUT(image, x, y) (uint8)(read_imageui(image, (int2)(x, y)), read_imageui(image, (int2)((x) + 1, y)))
#define LOAD_SCALE(p) vload8(0, p)
#define LOAD_SUM(p) convert_int8(vload8(0, p))
#define TO_INT convert_int8
#define TO_FLOAT convert_float8
#elif N20_RADIX_TOKENS == 4
typedef uint4 uvec;
typedef int4 ivec;
typedef float4 fvec;
#define READ_INPUT(image, x, y) read_imageui(image, (int2)(x, y))
#define LOAD_SCALE(p) vload4(0, p)
#define LOAD_SUM(p) convert_int4(vload4(0, p))
#define TO_INT convert_int4
#define TO_FLOAT convert_float4
#else
#error N20_RADIX_TOKENS must be 4 or 8
#endif

#ifdef cl_qcom_reqd_sub_group_size
#pragma OPENCL EXTENSION cl_qcom_reqd_sub_group_size : enable
__attribute__((qcom_reqd_sub_group_size("full")))
#endif
kernel void kernel_q2_radix_prefill(
    global const uchar *codes, global const half *scales,
    read_only image2d_t input, global const float *input_scale,
    global const ushort *input_sum, global const uchar *code_sum,
    global float *output, int rows, int padded, int columns, int tokens
) {
    const int token = get_global_id(0) * N20_RADIX_TOKENS;
    const int row = get_global_id(1) * 4;
    if (row >= rows || token >= tokens) return;
    fvec y0 = 0, y1 = 0, y2 = 0, y3 = 0;
    for (int block = 0; block < columns / 64; ++block) {
        uvec s0 = 0, s1 = 0, s2 = 0, s3 = 0;
        for (int i = 0; i < 16; ++i) {
            const uvec a = READ_INPUT(input, token / 4, block * 16 + i);
            const uvec a01 = (a & 255u) | (((a >> 8) & 255u) << 11);
            const uvec a23 = ((a >> 16) & 255u) | (((a >> 24) & 255u) << 11);
            const uchar4 c = vload4(0, codes + (block * 16 + i) * rows + row);
            #define ACC(s, v) do { \
                const uint w01 = (((uint)(v) >> 2) & 3u) | (((uint)(v) & 3u) << 11); \
                const uint w23 = (((uint)(v) >> 6) & 3u) | ((((uint)(v) >> 4) & 3u) << 11); \
                s += ((a01 * w01) >> 11 & 2047u) + ((a23 * w23) >> 11 & 2047u); \
            } while (0)
            ACC(s0, c.s0); ACC(s1, c.s1); ACC(s2, c.s2); ACC(s3, c.s3);
            #undef ACC
        }
        const fvec xscale = LOAD_SCALE(input_scale + block * padded + token);
        const ivec asum = LOAD_SUM(input_sum + block * padded + token);
        const float4 ws = convert_float4(vload4(0, scales + block * rows + row));
        const int4 cs = convert_int4(vload4(0, code_sum + block * rows + row));
        y0 += TO_FLOAT(TO_INT(s0) - asum - 127 * (cs.s0 - 64)) * xscale * ws.s0;
        y1 += TO_FLOAT(TO_INT(s1) - asum - 127 * (cs.s1 - 64)) * xscale * ws.s1;
        y2 += TO_FLOAT(TO_INT(s2) - asum - 127 * (cs.s2 - 64)) * xscale * ws.s2;
        y3 += TO_FLOAT(TO_INT(s3) - asum - 127 * (cs.s3 - 64)) * xscale * ws.s3;
    }
    #define STORE(t) if (token + t < tokens) \
        vstore4((float4)(y0.s##t, y1.s##t, y2.s##t, y3.s##t), 0, output + (token + t) * rows + row)
    STORE(0); STORE(1); STORE(2); STORE(3);
#if N20_RADIX_TOKENS == 8
    STORE(4); STORE(5); STORE(6); STORE(7);
#endif
    #undef STORE
}
