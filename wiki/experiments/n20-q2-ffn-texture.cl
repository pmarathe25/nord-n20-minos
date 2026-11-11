#pragma OPENCL EXTENSION cl_khr_fp16 : enable

// Independent experimental fused gate/up/SwiGLU path on a real 2D texture.
#ifndef N20_FFN_ROWS
#define N20_FFN_ROWS 2
#endif
#ifndef N20_FFN_TOKENS
#define N20_FFN_TOKENS 8
#endif
#if N20_FFN_ROWS != 1 && N20_FFN_ROWS != 2 && N20_FFN_ROWS != 4
#error N20_FFN_ROWS must be 1, 2 or 4
#endif
#if N20_FFN_TOKENS == 8
typedef half8 hvec;
typedef float8 fvec;
#define INPUT(x, y) (half8)(read_imageh(input, (int2)(x, y)), read_imageh(input, (int2)((x) + 1, y)))
#define TO_FLOAT convert_float8
#elif N20_FFN_TOKENS == 4
typedef half4 hvec;
typedef float4 fvec;
#define INPUT(x, y) read_imageh(input, (int2)(x, y))
#define TO_FLOAT convert_float4
#else
#error N20_FFN_TOKENS must be 4 or 8
#endif

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
    const int token = get_global_id(0) * N20_FFN_TOKENS;
    const int row = get_global_id(1) * N20_FFN_ROWS;
    if (row >= rows || token >= tokens) return;
    fvec gate[N20_FFN_ROWS], up[N20_FFN_ROWS];
    #pragma unroll
    for (int r = 0; r < N20_FFN_ROWS; ++r) { gate[r] = 0; up[r] = 0; }
    for (int block = 0; block < columns / 64; ++block) {
        hvec gp[N20_FFN_ROWS], ap[N20_FFN_ROWS];
        #pragma unroll
        for (int r = 0; r < N20_FFN_ROWS; ++r) { gp[r] = 0; ap[r] = 0; }
        for (int i = 0; i < 16; ++i) {
            uchar g[N20_FFN_ROWS], a[N20_FFN_ROWS];
            #pragma unroll
            for (int r = 0; r < N20_FFN_ROWS; ++r) {
                g[r] = gc[(block * 16 + i) * rows + row + r];
                a[r] = uc[(block * 16 + i) * rows + row + r];
            }
            #pragma unroll
            for (int j = 0; j < 4; ++j) {
                const hvec x = INPUT(token / 4, block * 64 + i * 4 + j);
                #pragma unroll
                for (int r = 0; r < N20_FFN_ROWS; ++r) {
                    gp[r] += x * convert_half((int)((g[r] >> (2 * j)) & 3) - 1);
                    ap[r] += x * convert_half((int)((a[r] >> (2 * j)) & 3) - 1);
                }
            }
        }
        #pragma unroll
        for (int r = 0; r < N20_FFN_ROWS; ++r) {
            gate[r] += TO_FLOAT(gp[r]) * convert_float(gs[block * rows + row + r]);
            up[r] += TO_FLOAT(ap[r]) * convert_float(us[block * rows + row + r]);
        }
    }
    #pragma unroll
    for (int r = 0; r < N20_FFN_ROWS; ++r) {
        const fvec z = (gate[r] / (1.0f + exp(-gate[r]))) * up[r];
        #define STORE(t) if (token + t < tokens) output[(token + t) * rows + row + r] = z.s##t
        STORE(0); STORE(1); STORE(2); STORE(3);
#if N20_FFN_TOKENS == 8
        STORE(4); STORE(5); STORE(6); STORE(7);
#endif
        #undef STORE
    }
    (void)padded;
}

kernel void n20_swiglu(global const float *gate, global const float *up,
                       global float *output, uint count) {
    const uint i = get_global_id(0);
    if (i < count) output[i] = gate[i] / (1.0f + exp(-gate[i])) * up[i];
}

kernel void n20_swiglu_merged(global const float *both, global float *output,
                              uint rows, uint count) {
    const uint row = get_global_id(0);
    const uint token = get_global_id(1);
    const uint i = token * rows + row;
    if (i < count) {
        const float gate = both[token * (2 * rows) + row];
        const float up = both[token * (2 * rows) + rows + row];
        output[i] = gate / (1.0f + exp(-gate)) * up;
    }
}
