#pragma OPENCL EXTENSION cl_khr_fp16 : enable

// Two output rows share sixteen tokens of the input image. This keeps
// the same number of accumulators as the four-row, eight-token kernel.
#ifdef cl_qcom_reqd_sub_group_size
#pragma OPENCL EXTENSION cl_qcom_reqd_sub_group_size : enable
#if N20_HALF_WAVE
__attribute__((qcom_reqd_sub_group_size("half")))
#else
__attribute__((qcom_reqd_sub_group_size("full")))
#endif
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
    const int token = get_global_id(0) * 16;
    const int row = get_global_id(1) * 2;
    if (row >= rows)
        return;

    output += offset / sizeof(float);
    const int input_stride = tokens_padded / 4;
    float16 y0 = 0, y1 = 0;
    for (int block = 0; block < columns; block += 64) {
        const float2 scale = convert_float2(vload2(0, scales + (block / 64) * rows + row));
        half16 p0 = 0, p1 = 0;
        for (int column = block; column < block + 64; column += 4) {
            const uchar2 code = vload2(0, codes + (column / 4) * rows + row);
            #pragma unroll
            for (int index = 0; index < 4; ++index) {
                const int pos = (column + index) * input_stride + token / 4;
                const half16 x = (half16)(read_imageh(input, pos), read_imageh(input, pos + 1),
                                          read_imageh(input, pos + 2), read_imageh(input, pos + 3));
                const uchar2 bits = (code >> (uchar2)(2 * index)) & (uchar2)3;
                const half2 weight = convert_half2(convert_int2(bits) - 1);
                p0 += x * weight.s0;
                p1 += x * weight.s1;
            }
        }
        y0 += convert_float16(p0) * scale.s0;
        y1 += convert_float16(p1) * scale.s1;
    }

    #define STORE(index, lane) if (token + index < tokens) \
        vstore2((float2)(y0.s##lane, y1.s##lane), 0, output + (token + index) * rows + row)
    STORE(0, 0); STORE(1, 1); STORE(2, 2); STORE(3, 3);
    STORE(4, 4); STORE(5, 5); STORE(6, 6); STORE(7, 7);
    STORE(8, 8); STORE(9, 9); STORE(10, a); STORE(11, b);
    STORE(12, c); STORE(13, d); STORE(14, e); STORE(15, f);
    #undef STORE
}
