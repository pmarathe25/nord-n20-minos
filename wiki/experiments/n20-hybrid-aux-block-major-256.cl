/* Two independent block-major DSP chunks; never mix with token-major skel. */
#pragma OPENCL EXTENSION cl_khr_fp16 : enable

/* This reproduces the tested 8-token-coalesced reduction and token-adjacent
 * pack in n20-q2-overlap-rowview-bench.c. The finite-magnitude limit remains
 * an empirical fallback rule, not a formal overflow guarantee. */
kernel void reduce_max_token8(global const half *x, global float *maxima,
                              global float *inverse, global uint *guard,
                              uint columns, uint padded, uint tokens) {
    uint lane=get_local_id(0), token=get_group_id(0)*8+lane%8, column_lane=lane/8;
    local float vals[64]; local uint bad[64]; float m=0.0f; uint invalid=0;
    if (token<tokens) for (uint k=column_lane; k<columns; k+=8) {
        float v=convert_float(x[k*padded+token]); uint bits=as_uint(v)&0x7fffffffu;
        invalid |= bits>0x41800000u; if (bits<0x7f800000u) m=fmax(m,fabs(v));
    }
    vals[lane]=m; bad[lane]=invalid; barrier(CLK_LOCAL_MEM_FENCE);
    for (uint d=4; d; d>>=1) {
        if (column_lane<d) { vals[lane]=fmax(vals[lane],vals[lane+d*8]); bad[lane]|=bad[lane+d*8]; }
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    if (!column_lane && token<tokens) {
        maxima[token]=vals[lane]; inverse[token]=vals[lane]>0.0f ? 127.0f/vals[lane] : 0.0f;
        if (bad[lane]) atomic_or((volatile global uint *)guard,1u);
    }
}

kernel void pack_q8_block_major(global const half *x, global uint *out0, global uint *out1, global const float *inverse,
                    uint blocks, uint padded, uint count, uint tokens) {
    uint id=get_global_id(0); if(id>=count) return;
    uint token=id%tokens, block=id/tokens;
    uint chunk_count=token<128 ? min(tokens,128u) : tokens-128;
    uint output_id=block*chunk_count+(token%128);
    global uint *out=token<128 ? out0 : out1;
    uint groups[16]={0}; int sum=0; float multiplier=inverse[token];
    for(uint j=0;j<64;++j) {
        float v=convert_float(x[(block*64+j)*padded+token]); uint bits=as_uint(v)&0x7fffffffu;
        int q=bits<0x7f800000u ? clamp(convert_int_rtz(round(v*multiplier)),-127,127) : 0;
        sum+=q; groups[(j/16)*4+j%4] |= ((uint)(uchar)q) << (8*((j%16)/4));
    }
    for(uint i=0;i<16;++i) out[output_id*17+i]=groups[i]; out[output_id*17+16]=as_uint(sum);
}

kernel void scatter_q8(global const int *dsp0, global const int *dsp1, global float *out, global const float *maxima,
                       ulong offset, uint rows, uint dsp_rows, uint tokens, float scale) {
    uint i=get_global_id(0); if(i>=dsp_rows*tokens) return;
    uint t=i/dsp_rows, row=i%dsp_rows;
    uint first=min(tokens,128u), count=t<128 ? first : tokens-128;
    uint local_token=t<128 ? t : t-128;
    global const int *dsp=t<128 ? dsp0 : dsp1;
    out[offset/sizeof(float)+t*rows+row]=convert_float(dsp[((row/32)*count+local_token)*32+row%32])*scale*maxima[t];
}
