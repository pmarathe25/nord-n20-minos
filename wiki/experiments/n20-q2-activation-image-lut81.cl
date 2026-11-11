#pragma OPENCL EXTENSION cl_khr_fp16 : enable
// Scratch-only four-trit LUT kernel. The host must reject Q2 code 3 and build
// the image with x=(token/4)*81+ternary_index, y=input_column/4, RGBA=tokens.
// Table creation and validation are excluded here, so timings are optimistic.
#ifdef cl_qcom_reqd_sub_group_size
#pragma OPENCL EXTENSION cl_qcom_reqd_sub_group_size : enable
__attribute__((qcom_reqd_sub_group_size("full")))
#endif
kernel void kernel_gemm_noshuffle_q2_0_f32(
    global const uchar *codes, global const half *scales, read_only image2d_t table,
    global float *output, ulong offset, int rows, int tokens_padded, int columns, int tokens) {
    const int token = get_global_id(0) * 8;
    const int row = get_global_id(1) * 4;
    if (row >= rows) return;
    output += offset / sizeof(float);
    float8 y0=0, y1=0, y2=0, y3=0;
    for (int column=0; column<columns; column+=64) {
        const float4 scale=convert_float4(vload4(0,scales+(column/64)*rows+row));
        half8 p0=0, p1=0, p2=0, p3=0;
        for (int i=column; i<column+64; i+=4) {
            const uchar4 q=vload4(0,codes+(i/4)*rows+row);
            const int x0=(token/4)*81, x1=x0+81, y=i/4;
            p0 += (half8)(read_imageh(table,(int2)(x0+q.s0,y)),read_imageh(table,(int2)(x1+q.s0,y)));
            p1 += (half8)(read_imageh(table,(int2)(x0+q.s1,y)),read_imageh(table,(int2)(x1+q.s1,y)));
            p2 += (half8)(read_imageh(table,(int2)(x0+q.s2,y)),read_imageh(table,(int2)(x1+q.s2,y)));
            p3 += (half8)(read_imageh(table,(int2)(x0+q.s3,y)),read_imageh(table,(int2)(x1+q.s3,y)));
        }
        y0+=convert_float8(p0)*scale.s0; y1+=convert_float8(p1)*scale.s1;
        y2+=convert_float8(p2)*scale.s2; y3+=convert_float8(p3)*scale.s3;
    }
    #define STORE(t) if (token+t<tokens) \
        vstore4((float4)(y0.s##t,y1.s##t,y2.s##t,y3.s##t),0,output+(token+t)*rows+row)
    STORE(0); STORE(1); STORE(2); STORE(3); STORE(4); STORE(5); STORE(6); STORE(7);
    #undef STORE
}
