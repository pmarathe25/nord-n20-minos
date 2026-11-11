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
__kernel void dense(__global const half *w, __global const half *x, __global float *y, int rows,int columns,int tokens,int padded) {
    int lr=get_local_id(0),lt=get_local_id(1),id=lt*8+lr;
    int rb=get_group_id(0)*M,tb=get_group_id(1)*N;
    __local half wa[K][M]; __local half xa[K][N];
    float acc[RM][TN];
    for(int r=0;r<RM;r++) for(int t=0;t<TN;t++) acc[r][t]=0;
    for(int kb=0;kb<columns;kb+=K) {
        for(int i=id;i<K*M;i+=64) wa[i/M][i%M]=w[(kb+i/M)*rows+rb+i%M];
        for(int i=id;i<K*N;i+=64) xa[i/N][i%N]=tb+i%N<tokens ? x[(kb+i/N)*padded+tb+i%N] : 0;
        barrier(CLK_LOCAL_MEM_FENCE);
#if HALF
        half partial[RM][TN];
        for(int r=0;r<RM;r++) for(int t=0;t<TN;t++) partial[r][t]=0;
#endif
        for(int k=0;k<K;k++) {
            for(int r=0;r<RM;r++) {
#if HALF
                half a=wa[k][lr+r*8];
                for(int t=0;t<TN;t++) partial[r][t]+=a*xa[k][lt+t*8];
#else
                float a=convert_float(wa[k][lr+r*8]);
                for(int t=0;t<TN;t++) acc[r][t]=fma(a,convert_float(xa[k][lt+t*8]),acc[r][t]);
#endif
            }
        }
#if HALF
        for(int r=0;r<RM;r++) for(int t=0;t<TN;t++) acc[r][t]+=convert_float(partial[r][t]);
#endif
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    for(int r=0;r<RM;r++) for(int t=0;t<TN;t++) if(tb+lt+t*8<tokens) y[(tb+lt+t*8)*rows+rb+lr+r*8]=acc[r][t];
}
