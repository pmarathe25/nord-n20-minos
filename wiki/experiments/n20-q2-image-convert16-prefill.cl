#pragma OPENCL EXTENSION cl_khr_fp16 : enable

/* Experimental 4-row/8-token texture projection. Unpack the four Q2 fields
 * for a weight byte with one uchar16-to-half16 conversion, then use named
 * lanes. Arithmetic is unchanged; all four Q2 codes, including code 3,
 * remain supported. This is an isolated screen, not a deployed backend. */
#ifdef cl_qcom_reqd_sub_group_size
#pragma OPENCL EXTENSION cl_qcom_reqd_sub_group_size : enable
__attribute__((qcom_reqd_sub_group_size("full")))
#endif
kernel void kernel_gemm_noshuffle_q2_0_f32(
    global const uchar *codes, global const half *scales,
    read_only image2d_t input, global float *output, ulong offset,
    int rows, int tokens_padded, int columns, int tokens) {
    const int token = get_global_id(0) * 8;
    const int row = get_global_id(1) * 4;
    if (row >= rows) return;
    output += offset / sizeof(float);
    float8 y0=0, y1=0, y2=0, y3=0;
    for (int column=0; column<columns; column+=64) {
        float4 scale=convert_float4(vload4(0, scales+(column/64)*rows+row));
        half8 p0=0, p1=0, p2=0, p3=0;
        for (int i=column; i<column+64; i+=4) {
            uchar4 code=vload4(0,codes+(i/4)*rows+row);
            uchar16 q=(uchar16)(code & (uchar4)3, (code >> (uchar4)2) & (uchar4)3,
                                (code >> (uchar4)4) & (uchar4)3, code >> (uchar4)6);
            half16 w=convert_half16(q)-(half16)1;
            #define STEP(j,a,b,c,d) do { \
                half8 x=(half8)(read_imageh(input,(int2)(token/4,i+(j))), \
                                read_imageh(input,(int2)(token/4+1,i+(j)))); \
                p0+=x*w.s##a; p1+=x*w.s##b; p2+=x*w.s##c; p3+=x*w.s##d; \
            } while (0)
            STEP(0,0,1,2,3); STEP(1,4,5,6,7); STEP(2,8,9,a,b); STEP(3,c,d,e,f);
            #undef STEP
        }
        y0+=convert_float8(p0)*scale.s0; y1+=convert_float8(p1)*scale.s1;
        y2+=convert_float8(p2)*scale.s2; y3+=convert_float8(p3)*scale.s3;
    }
    #define STORE(t) if(token+(t)<tokens) \
        vstore4((float4)(y0.s##t,y1.s##t,y2.s##t,y3.s##t),0,output+(token+(t))*rows+row)
    STORE(0); STORE(1); STORE(2); STORE(3); STORE(4); STORE(5); STORE(6); STORE(7);
    #undef STORE
    (void)tokens_padded;
}
