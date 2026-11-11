#pragma OPENCL EXTENSION cl_khr_fp16 : enable

// Each adjacent row pair and 64-column scale block has a list of input
// column indices, grouped by the pair's two ternary weight codes. The
// all-zero pattern has no entries. Offsets has ten bytes per row-pair
// block; indices has 64 bytes per block, including unused tail bytes.
#ifdef cl_qcom_reqd_sub_group_size
#pragma OPENCL EXTENSION cl_qcom_reqd_sub_group_size : enable
__attribute__((qcom_reqd_sub_group_size("full")))
#endif
kernel void kernel_q2_segment_prefill(
    global const uchar *indices, global const half *scales,
    read_only image2d_t input, global float *output, ulong offset,
    int rows, int padded, int columns, int tokens,
    global const uchar *offsets
) {
    const int token = get_global_id(0) * 8;
    const int row = get_global_id(1) * 2;
    if (row >= rows || token >= tokens) return;
    output += offset / sizeof(float);
    float8 y0 = 0, y1 = 0;
    for (int b = 0; b < columns / 64; ++b) {
        const int tile = b * (rows / 2) + row / 2;
        half8 p0 = 0, p1 = 0;
        #pragma unroll
        for (int pattern = 0; pattern < 9; ++pattern) {
            if (pattern == 4) continue;
            half8 sum = 0;
            for (int k = offsets[tile * 10 + pattern]; k < offsets[tile * 10 + pattern + 1]; ++k) {
                const int column = b * 64 + indices[tile * 64 + k];
                sum += (half8)(read_imageh(input, (int2)(token / 4, column)),
                               read_imageh(input, (int2)(token / 4 + 1, column)));
            }
            if (pattern % 3 == 0) p0 -= sum;
            if (pattern % 3 == 2) p0 += sum;
            if (pattern / 3 == 0) p1 -= sum;
            if (pattern / 3 == 2) p1 += sum;
        }
        const float2 scale = convert_float2(vload2(0, scales + b * rows + row));
        y0 += convert_float8(p0) * scale.s0;
        y1 += convert_float8(p1) * scale.s1;
    }
    #define STORE(t) if (token + t < tokens) \
        vstore2((float2)(y0.s##t, y1.s##t), 0, output + (token + t) * rows + row)
    STORE(0); STORE(1); STORE(2); STORE(3);
    STORE(4); STORE(5); STORE(6); STORE(7);
    #undef STORE
    (void)padded;
}
