#pragma OPENCL EXTENSION cl_khr_fp16 : enable

// Each workgroup shares a lookup table for every pair of input columns.
// A four-bit code identifies the contribution of two weights to eight tokens.
#define TILE_COLUMNS 32
#define PAIR_CODES 16
#define TILE_PAIRS (TILE_COLUMNS / 2)

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
    const int token = get_global_id(0) * 8;
    const int row = get_global_id(1) * 4;
    const int lid = get_local_id(1);
    const int threads = get_local_size(1);
    const int input_stride = tokens_padded / 4;
    local half8 contributions[TILE_PAIRS * PAIR_CODES];
    float8 y0 = 0, y1 = 0, y2 = 0, y3 = 0;

    for (int column = 0; column < columns; column += TILE_COLUMNS) {
        for (int i = lid; i < TILE_PAIRS * PAIR_CODES; i += threads) {
            const int pair = i / PAIR_CODES;
            const int code = i % PAIR_CODES;
            const int pos = (column + 2 * pair) * input_stride + token / 4;
            const half8 a = (half8)(read_imageh(input, pos), read_imageh(input, pos + 1));
            const half8 b = (half8)(read_imageh(input, pos + input_stride),
                                    read_imageh(input, pos + input_stride + 1));
            contributions[i] = a * (half)((code & 3) - 1) + b * (half)((code >> 2) - 1);
        }
        barrier(CLK_LOCAL_MEM_FENCE);

        if (row < rows) {
            const float4 d = convert_float4(vload4(0, scales + (column / 64) * rows + row));
            half8 p0 = 0, p1 = 0, p2 = 0, p3 = 0;
            for (int k = 0; k < TILE_COLUMNS; k += 4) {
                const uchar4 q = vload4(0, codes + ((column + k) / 4) * rows + row);
                local const half8 *lo = contributions + (k / 2) * PAIR_CODES;
                local const half8 *hi = lo + PAIR_CODES;
                p0 += lo[q.s0 & 15] + hi[q.s0 >> 4];
                p1 += lo[q.s1 & 15] + hi[q.s1 >> 4];
                p2 += lo[q.s2 & 15] + hi[q.s2 >> 4];
                p3 += lo[q.s3 & 15] + hi[q.s3 >> 4];
            }
            y0 += convert_float8(p0) * d.s0;
            y1 += convert_float8(p1) * d.s1;
            y2 += convert_float8(p2) * d.s2;
            y3 += convert_float8(p3) * d.s3;
        }
        barrier(CLK_LOCAL_MEM_FENCE);
    }

    if (row < rows) {
        output += offset / sizeof(float);
        #define STORE(t) if (token + t < tokens) \
            vstore4((float4)(y0.s##t, y1.s##t, y2.s##t, y3.s##t), 0, output + (token + t) * rows + row)
        STORE(0); STORE(1); STORE(2); STORE(3);
        STORE(4); STORE(5); STORE(6); STORE(7);
        #undef STORE
    }
}
