#pragma OPENCL EXTENSION cl_khr_fp16 : enable

static inline float swiglu(float gate, float up) {
    return (gate / (1.0f + exp(-gate))) * up;
}

// Two rows of both projections share one activation tile. Keep the
// intermediate results private and write the SwiGLU output directly.
kernel void n20_q2_ffn_prefill(
    global const uchar * gate_codes, global const half * gate_scales,
    global const uchar * up_codes, global const half * up_scales,
    read_only image1d_buffer_t input, global float * output, ulong output_offset,
    int rows, int tokens_padded, int columns, int tokens
) {
    const int token = get_global_id(0) * 8;
    const int row = get_global_id(1) * 2;
    if (row >= rows)
        return;

    output += output_offset / sizeof(float);
    const int input_stride = tokens_padded / 4;
    float8 g0 = 0, g1 = 0, u0 = 0, u1 = 0;
    for (int column = 0; column < columns; column += 64) {
        const int scale_offset = (column / 64) * rows + row;
        const float2 gd = convert_float2(vload2(0, gate_scales + scale_offset));
        const float2 ud = convert_float2(vload2(0, up_scales + scale_offset));
        half8 pg0 = 0, pg1 = 0, pu0 = 0, pu1 = 0;
        for (int i = column; i < column + 64; i += 4) {
            const int offset = (i / 4) * rows + row;
            const uchar2 g = vload2(0, gate_codes + offset);
            const uchar2 u = vload2(0, up_codes + offset);
            #pragma unroll
            for (int shift = 0; shift < 8; shift += 2) {
                const int pos = (i + shift / 2) * input_stride + token / 4;
                const half8 x = (half8)(read_imageh(input, pos), read_imageh(input, pos + 1));
                const half2 gw = convert_half2(convert_int2((g >> (uchar2)shift) & (uchar2)3) - 1);
                const half2 uw = convert_half2(convert_int2((u >> (uchar2)shift) & (uchar2)3) - 1);
                pg0 += x * gw.s0;
                pg1 += x * gw.s1;
                pu0 += x * uw.s0;
                pu1 += x * uw.s1;
            }
        }
        g0 += convert_float8(pg0) * gd.s0;
        g1 += convert_float8(pg1) * gd.s1;
        u0 += convert_float8(pu0) * ud.s0;
        u1 += convert_float8(pu1) * ud.s1;
    }
    #define STORE(t) if (token + t < tokens) \
        vstore2((float2)(swiglu(g0.s##t, u0.s##t), swiglu(g1.s##t, u1.s##t)), \
                0, output + (token + t) * rows + row)
    STORE(0); STORE(1); STORE(2); STORE(3);
    STORE(4); STORE(5); STORE(6); STORE(7);
    #undef STORE
}

// Four rows of each projection use the same 16-way split across columns
// as the ordinary Q2 decode kernel.
kernel void n20_q2_ffn_decode(
    global const uchar * gate_codes, global const half * gate_scales,
    global const uchar * up_codes, global const half * up_scales,
    global const float * input, ulong input_offset, global float * output, ulong output_offset,
    int rows, int columns
) {
    const uint lanes = get_local_size(0);
    const uint splits = get_local_size(1);
    const uint lane = get_local_id(0);
    const uint split = get_local_id(1);
    const uint tile = get_group_id(0) * lanes + lane;
    const uint row = tile * 4;
    global const float * x = input + input_offset / sizeof(float);
    float4 gate = 0, up = 0;

    if (row < (uint)rows) {
        for (uint block = split; block < (uint)columns / 64; block += splits) {
            float4 pg = 0, pu = 0, bias = 0;
            for (uint i = 0; i < 16; ++i) {
                const uint j = block * 16 + i;
                const uint index = j * (rows / 4) + tile;
                const uchar4 g = as_uchar4(((global const uint *)gate_codes)[index]);
                const uchar4 u = as_uchar4(((global const uint *)up_codes)[index]);
                const float4 a = vload4(j, x);
                pg += convert_float4(g & (uchar4)3) * a.s0;
                pg += convert_float4((g >> (uchar4)2) & (uchar4)3) * a.s1;
                pg += convert_float4((g >> (uchar4)4) & (uchar4)3) * a.s2;
                pg += convert_float4(g >> (uchar4)6) * a.s3;
                pu += convert_float4(u & (uchar4)3) * a.s0;
                pu += convert_float4((u >> (uchar4)2) & (uchar4)3) * a.s1;
                pu += convert_float4((u >> (uchar4)4) & (uchar4)3) * a.s2;
                pu += convert_float4(u >> (uchar4)6) * a.s3;
                bias += a;
            }
            const float b = (bias.s0 + bias.s1) + (bias.s2 + bias.s3);
            const uint index = block * rows + row;
            gate += convert_float4(vload4(0, gate_scales + index)) * (pg - b);
            up += convert_float4(vload4(0, up_scales + index)) * (pu - b);
        }
    }
    local float8 parts[512];
    parts[split * lanes + lane] = (float8)(gate, up);
    barrier(CLK_LOCAL_MEM_FENCE);
    if (split == 0 && row < (uint)rows) {
        float8 sum = parts[lane];
        for (uint s = 1; s < splits; ++s)
            sum += parts[s * lanes + lane];
        const float4 g = sum.lo;
        const float4 u = sum.hi;
        const float4 result = (g / (1.0f + exp(-g))) * u;
        vstore4(result, 0, output + output_offset / sizeof(float) + row);
    }
}
