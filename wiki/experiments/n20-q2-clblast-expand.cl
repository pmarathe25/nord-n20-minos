#define HALF 1
#pragma OPENCL EXTENSION cl_khr_fp16 : enable
#ifndef RM
#define RM 4
#endif
#ifndef TN
#define TN 4
#endif
#define M (8*RM)
#define N (8*TN)
#define K 32
#ifndef HALF
#define HALF 0
#endif
__kernel void expand(__global const uchar *q, __global const half *s, __global half *w, int rows, int columns) {
    int r=get_global_id(0), j=get_global_id(1);
    if(r>=rows || j>=columns/4) return;
    uchar v=q[j*rows+r]; half scale=s[(j/16)*rows+r];
    for(int z=0;z<4;z++) w[(j*4+z)*rows+r]=(half)((int)((v>>(2*z))&3)-1)*scale;
}
__kernel void cast(__global const half *h,__global float *y) {size_t i=get_global_id(0);y[i]=convert_float(h[i]);}