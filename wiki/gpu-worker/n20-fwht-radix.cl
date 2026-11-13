#pragma OPENCL EXTENSION cl_khr_fp16 : enable

static inline float4 hadamard4(float4 x) {
    const float a = x.s0 + x.s1;
    const float b = x.s0 - x.s1;
    const float c = x.s2 + x.s3;
    const float d = x.s2 - x.s3;
    return (float4)(a + c, b + d, a - c, b - d);
}

static inline float16 hadamard16(float16 x) {
    const float4 a = hadamard4(x.s0123);
    const float4 b = hadamard4(x.s4567);
    const float4 c = hadamard4(x.s89ab);
    const float4 d = hadamard4(x.scdef);
    const float4 lo_sum = a + b, lo_diff = a - b;
    const float4 hi_sum = c + d, hi_diff = c - d;
    return (float16)(lo_sum + hi_sum, lo_diff + hi_diff,
                     lo_sum - hi_sum, lo_diff - hi_diff);
}

static inline float16 gather16(local const float * x, int stride) {
    return (float16)(x[0], x[stride], x[2*stride], x[3*stride],
                     x[4*stride], x[5*stride], x[6*stride], x[7*stride],
                     x[8*stride], x[9*stride], x[10*stride], x[11*stride],
                     x[12*stride], x[13*stride], x[14*stride], x[15*stride]);
}

static inline void scatter16(global float * y, int stride, float16 x) {
    y[0] = x.s0;
    y[stride] = x.s1;
    y[2*stride] = x.s2;
    y[3*stride] = x.s3;
    y[4*stride] = x.s4;
    y[5*stride] = x.s5;
    y[6*stride] = x.s6;
    y[7*stride] = x.s7;
    y[8*stride] = x.s8;
    y[9*stride] = x.s9;
    y[10*stride] = x.sa;
    y[11*stride] = x.sb;
    y[12*stride] = x.sc;
    y[13*stride] = x.sd;
    y[14*stride] = x.se;
    y[15*stride] = x.sf;
}

// Each workgroup transforms one 512-element row as 32 × 16.
__attribute__((reqd_work_group_size(32, 1, 1)))
kernel void kernel_fwht(
    global const char * src, ulong offset_src, global char * dst, ulong offset_dst,
    int n, ulong ne1, ulong ne2, ulong nb1, ulong nb2, ulong nb3, float scale,
    local float * work
) {
    const uint lane = get_local_id(0);
    const ulong row = get_group_id(1);
    const ulong i1 = row % ne1;
    const ulong i2 = row / ne1 % ne2;
    const ulong i3 = row / ne1 / ne2;
    global const SRC_TYPE * x = (global const SRC_TYPE *)(src + offset_src + i1*nb1 + i2*nb2 + i3*nb3);
    global float * y = (global float *)(dst + offset_dst) + row*n + lane;
    if (lane < 16) {
        const float16 a = hadamard16(convert_float16(vload16(2*lane, x)) * scale);
        const float16 b = hadamard16(convert_float16(vload16(2*lane + 1, x)) * scale);
        vstore16(a + b, 2*lane, work);
        vstore16(a - b, 2*lane + 1, work);
    }
    barrier(CLK_LOCAL_MEM_FENCE);
    scatter16(y, 32, hadamard16(gather16(work + lane, 32)));
}
