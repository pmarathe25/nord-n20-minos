#pragma OPENCL EXTENSION cl_khr_fp16 : enable
/* One 128-lane workgroup packs eight tokens for one 64-column block.
 * Each token uses sixteen lanes, one for each packed four-byte word. */
kernel void parallel_pack(global const half *x, global uint *out,
                          global const float *inverse, uint blocks,
                          uint padded, uint tokens) {
    const uint lane = get_local_id(0);
    const uint token = get_group_id(0) / blocks * 8 + lane % 8;
    const uint block = get_group_id(0) % blocks;
    const uint word = lane / 8;
    const uint base = block * 64 + (word / 4) * 16 + word % 4;
    local int sums[128];
    int sum = 0;
    uint packed = 0;
    if (token < tokens) {
        const float multiplier = inverse[token];
        for (uint j = 0; j < 4; ++j) {
            const float v = convert_float(x[(base + 4 * j) * padded + token]);
            const uint bits = as_uint(v) & 0x7fffffffu;
            const int q = bits < 0x7f800000u ? clamp(convert_int_rtz(round(v * multiplier)), -127, 127) : 0;
            sum += q;
            packed |= ((uint)(uchar)q) << (8 * j);
        }
        out[(token * blocks + block) * 17 + word] = packed;
    }
    sums[lane] = sum;
    barrier(CLK_LOCAL_MEM_FENCE);
    if (word == 0 && token < tokens) {
        int total = 0;
        for (uint w = 0; w < 16; ++w) total += sums[w * 8 + lane];
        out[(token * blocks + block) * 17 + 16] = as_uint(total);
    }
}
