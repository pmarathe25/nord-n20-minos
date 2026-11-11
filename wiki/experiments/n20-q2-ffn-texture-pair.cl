#pragma OPENCL EXTENSION cl_khr_fp16 : enable

// Explicit two-row variant: avoid indexed private arrays in the fused loop.
#ifdef cl_qcom_reqd_sub_group_size
#pragma OPENCL EXTENSION cl_qcom_reqd_sub_group_size : enable
__attribute__((qcom_reqd_sub_group_size("full")))
#endif
kernel void n20_q2_ffn_texture(
    global const uchar *gc, global const half *gs,
    global const uchar *uc, global const half *us,
    read_only image2d_t input, global float *output,
    int rows, int padded, int columns, int tokens
) {
    const int token = get_global_id(0) * 8;
    const int row = get_global_id(1) * 2;
    if (row >= rows || token >= tokens) return;
    float8 g0 = 0, g1 = 0, u0 = 0, u1 = 0;
    for (int block = 0; block < columns / 64; ++block) {
        const float2 gd = convert_float2(vload2(0, gs + block * rows + row));
        const float2 ud = convert_float2(vload2(0, us + block * rows + row));
        half8 pg0 = 0, pg1 = 0, pu0 = 0, pu1 = 0;
        for (int i = 0; i < 16; ++i) {
            const int off = (block * 16 + i) * rows + row;
            const uchar2 g = vload2(0, gc + off);
            const uchar2 u = vload2(0, uc + off);
            #pragma unroll
            for (int j = 0; j < 4; ++j) {
                const int column = block * 64 + i * 4 + j;
                const half8 x = (half8)(read_imageh(input, (int2)(token / 4, column)),
                                        read_imageh(input, (int2)(token / 4 + 1, column)));
                const half2 gw = convert_half2(convert_int2((g >> (uchar2)(2 * j)) & (uchar2)3) - 1);
                const half2 uw = convert_half2(convert_int2((u >> (uchar2)(2 * j)) & (uchar2)3) - 1);
                pg0 += x * gw.s0; pg1 += x * gw.s1;
                pu0 += x * uw.s0; pu1 += x * uw.s1;
            }
        }
        g0 += convert_float8(pg0) * gd.s0; g1 += convert_float8(pg1) * gd.s1;
        u0 += convert_float8(pu0) * ud.s0; u1 += convert_float8(pu1) * ud.s1;
    }
    const float8 z0 = g0 / (1.0f + exp(-g0)) * u0;
    const float8 z1 = g1 / (1.0f + exp(-g1)) * u1;
    #define STORE(t) if (token + t < tokens) \
        vstore2((float2)(z0.s##t, z1.s##t), 0, output + (token + t) * rows + row)
    STORE(0); STORE(1); STORE(2); STORE(3);
    STORE(4); STORE(5); STORE(6); STORE(7);
    #undef STORE
    (void)padded;
}

kernel void n20_swiglu(global const float *gate, global const float *up,
                       global float *output, uint count) {
    const uint i = get_global_id(0);
    if (i < count) output[i] = gate[i] / (1.0f + exp(-gate[i])) * up[i];
}

kernel void n20_swiglu_merged(global const float *both, global float *output,
                              uint rows, uint count) {
    const uint row = get_global_id(0), token = get_global_id(1);
    const uint i = token * rows + row;
    if (i < count) {
        const float gate = both[token * (2 * rows) + row];
        const float up = both[token * (2 * rows) + rows + row];
        output[i] = gate / (1.0f + exp(-gate)) * up;
    }
}
