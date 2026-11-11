#pragma OPENCL EXTENSION cl_khr_fp16 : enable

// Group four butterfly inputs in registers for the first and final two
// stages. Only the middle stages exchange values through local memory.
static inline float4 butterfly4(float4 x) {
    const float a = x.s0 + x.s1;
    const float b = x.s0 - x.s1;
    const float c = x.s2 + x.s3;
    const float d = x.s2 - x.s3;
    return (float4)(a + c, b + d, a - c, b - d);
}

kernel void kernel_fwht(
    global const char * src,
    ulong offset_src,
    global char * dst,
    ulong offset_dst,
    int n,
    ulong ne1,
    ulong ne2,
    ulong nb1,
    ulong nb2,
    ulong nb3,
    float scale,
    local float * work
) {
    const ulong row = get_group_id(1);
    const ulong i1 = row % ne1;
    const ulong i2 = row / ne1 % ne2;
    const ulong i3 = row / ne1 / ne2;
    global const SRC_TYPE * x = (global const SRC_TYPE *)(src + offset_src + i1*nb1 + i2*nb2 + i3*nb3);
    global float * y = (global float *)(dst + offset_dst) + row*n;
    const int tid = get_local_id(0);
    const int nth = get_local_size(0);
    const int quarter = n / 4;

    // This source is selected only for power-of-two widths of at least 16.
    for (int i = tid; i < quarter; i += nth) {
        const float4 scaled = convert_float4(vload4(i, x)) * scale;
        vstore4(butterfly4(scaled), i, work);
    }
    barrier(CLK_LOCAL_MEM_FENCE);

    for (int h = 4; h < quarter; h *= 2) {
        for (int i = tid; i < n/2; i += nth) {
            const int j = 2*i - (i & (h - 1));
            const float a = work[j];
            const float b = work[j + h];
            work[j] = a + b;
            work[j + h] = a - b;
        }
        barrier(CLK_LOCAL_MEM_FENCE);
    }

    for (int i = tid; i < quarter; i += nth) {
        const float4 out = butterfly4((float4)(work[i], work[i + quarter],
                                              work[i + 2*quarter], work[i + 3*quarter]));
        y[i] = out.s0;
        y[i + quarter] = out.s1;
        y[i + 2*quarter] = out.s2;
        y[i + 3*quarter] = out.s3;
    }
}
