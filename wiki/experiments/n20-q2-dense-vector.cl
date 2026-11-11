#pragma OPENCL EXTENSION cl_khr_fp16 : enable
__kernel void expand(__global const uchar *q, __global const half *s, __global half *w, int rows, int columns) {
 int r=get_global_id(0), j=get_global_id(1); if(r>=rows||j>=columns/4)return;
 uchar v=q[j*rows+r]; half scale=s[(j/16)*rows+r];
 for(int z=0;z<4;z++) w[(j*4+z)*rows+r]=(half)((int)((v>>(2*z))&3)-1)*scale;
}
__kernel void dense(__global const half *w,__global const half *x,__global float *y,int rows,int columns,int tokens,int padded) {
 int lr=get_local_id(0),lt=get_local_id(1),id=lt*8+lr,rb=get_group_id(0)*32,tb=get_group_id(1)*32;
 __local half wa[32][32],xa[32][32];
 float4 a0=0,a1=0,a2=0,a3=0;
 for(int kb=0;kb<columns;kb+=32){
  for(int i=id;i<1024;i+=64){wa[i/32][i%32]=w[(kb+i/32)*rows+rb+i%32];xa[i/32][i%32]=tb+i%32<tokens?x[(kb+i/32)*padded+tb+i%32]:0;}
  barrier(CLK_LOCAL_MEM_FENCE);
  for(int k=0;k<32;k++) {
   float4 v=convert_float4(vload4(lt,xa[k]));
   a0+=convert_float(wa[k][lr*4])*v;
   a1+=convert_float(wa[k][lr*4+1])*v;
   a2+=convert_float(wa[k][lr*4+2])*v;
   a3+=convert_float(wa[k][lr*4+3])*v;
  }
  barrier(CLK_LOCAL_MEM_FENCE);
 }
 #define STORE(j, component) if(tb+lt*4+j<tokens){int off=(tb+lt*4+j)*rows+rb+lr*4;y[off]=a0.component;y[off+1]=a1.component;y[off+2]=a2.component;y[off+3]=a3.component;}
 STORE(0,s0) STORE(1,s1) STORE(2,s2) STORE(3,s3)
}
