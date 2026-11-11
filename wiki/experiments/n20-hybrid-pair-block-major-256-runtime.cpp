/* Requires block-major auxiliary pack and distinct block-major DSP skel. */
/* Experimental, opt-in Android/Bionic bridge for one Q2 FFN row split.
 * The caller serializes calls on the in-order compute queue. A failed project
 * leaves any partially queued shard in that queue; the caller MUST enqueue
 * a full GPU projection after it, before consuming the output. */
#define CL_TARGET_OPENCL_VERSION 200
#include "n20-hybrid-runtime.h"
#include "n20-dsp-client.h"
#include <CL/cl_ext.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <new>
#include <string>

namespace {
constexpr unsigned kRows=9216, kDspRows=3072, kColumns=2560, kBlocks=40, kChunkTokens=128, kMaxTokens=256;
constexpr size_t kInputBytes=(size_t)kChunkTokens*kBlocks*68;
constexpr size_t kOutputBytes=(size_t)kChunkTokens*kDspRows*sizeof(int32_t);

cl_program program(cl_context ctx, cl_device_id dev, const char *path, const char *opts) {
    if (!path || !*path) return nullptr;
    std::ifstream file(path, std::ios::binary);
    std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (source.empty() || source.size()>1024*1024) return nullptr;
    const char *data=source.c_str(); size_t length=source.size(); cl_int err;
    cl_program p=clCreateProgramWithSource(ctx,1,&data,&length,&err);
    if (err!=CL_SUCCESS || !p) return nullptr;
    err=clBuildProgram(p,1,&dev,opts,nullptr,nullptr);
    if (err!=CL_SUCCESS) {
        size_t n=0;
        if (clGetProgramBuildInfo(p,dev,CL_PROGRAM_BUILD_LOG,0,nullptr,&n)==CL_SUCCESS && n<65536) {
            std::string log(n,'\0');
            if (n && clGetProgramBuildInfo(p,dev,CL_PROGRAM_BUILD_LOG,n,&log[0],nullptr)==CL_SUCCESS)
                std::fprintf(stderr,"n20 hybrid: OpenCL build failed: %s\n",log.c_str());
        }
        clReleaseProgram(p); return nullptr;
    }
    return p;
}
cl_mem ion_import(cl_context ctx,const n20_dsp_buffer &b,size_t bytes) {
    cl_mem_ion_host_ptr ion={};
    ion.ext_host_ptr.allocation_type=CL_MEM_ION_HOST_PTR_QCOM;
    ion.ext_host_ptr.host_cache_policy=CL_MEM_HOST_UNCACHED_QCOM;
    ion.ion_filedesc=b.fd; ion.ion_hostptr=b.addr;
    cl_int err; cl_mem mem=clCreateBuffer(ctx,CL_MEM_READ_WRITE|CL_MEM_EXT_HOST_PTR_QCOM|CL_MEM_USE_HOST_PTR,
                                         bytes,&ion,&err);
    return err==CL_SUCCESS ? mem : nullptr;
}
}

struct n20_hybrid {
    n20_dsp_client *dsp=nullptr;
    n20_dsp_buffer input[2]={}, output[2]={}, pair_output[2]={};
    cl_context context=nullptr; cl_command_queue queue=nullptr;
    cl_mem input_cl[2]={}, output_cl[2]={}, pair_output_cl[2]={}, maxima=nullptr, inverse=nullptr, guard=nullptr;
    cl_kernel reduce=nullptr, pack=nullptr, scatter=nullptr, shard=nullptr;
    unsigned next_slot=0, attempts=0, successes=0, pair_attempts=0, pair_successes=0;
    bool pair_ready=false; unsigned pair_n=0, pair_padded=0; cl_mem pair_texture=nullptr;
};

extern "C" void n20_hybrid_close(n20_hybrid **ptr) {
    if (!ptr || !*ptr) return;
    n20_hybrid *s=*ptr;
    std::fprintf(stderr,"n20 hybrid: slots_attempted=%u projections_attempted=%u dispatched=%u fallbacks=%u\n",
                 s->next_slot,s->attempts,s->successes,s->attempts-s->successes);
    std::fprintf(stderr,"n20 hybrid pair: attempted=%u reused=%u\n",s->pair_attempts,s->pair_successes);
    if (s->queue) clFinish(s->queue);
    if (s->shard) clReleaseKernel(s->shard);
    if (s->scatter) clReleaseKernel(s->scatter);
    if (s->pack) clReleaseKernel(s->pack);
    if (s->reduce) clReleaseKernel(s->reduce);
    if (s->guard) clReleaseMemObject(s->guard);
    if (s->inverse) clReleaseMemObject(s->inverse);
    if (s->maxima) clReleaseMemObject(s->maxima);
    for (unsigned i=0;i<2;++i) {
        if (s->pair_output_cl[i]) clReleaseMemObject(s->pair_output_cl[i]);
        if (s->output_cl[i]) clReleaseMemObject(s->output_cl[i]);
        if (s->input_cl[i]) clReleaseMemObject(s->input_cl[i]);
        if (s->dsp && s->pair_output[i].addr) n20_dsp_ion_free(s->dsp,&s->pair_output[i]);
        if (s->dsp && s->output[i].addr) n20_dsp_ion_free(s->dsp,&s->output[i]);
        if (s->dsp && s->input[i].addr) n20_dsp_ion_free(s->dsp,&s->input[i]);
    }
    if (s->dsp) n20_dsp_close(&s->dsp);
    delete s; *ptr=nullptr;
}

extern "C" int n20_hybrid_open(n20_hybrid **out,cl_context ctx,cl_device_id dev,
                                cl_command_queue queue,const char *row_path,const char *aux_path) {
    if (!out || !ctx || !dev || !queue) return -1;
    *out=nullptr;
    auto *s=new(std::nothrow) n20_hybrid;
    if (!s) return -1;
    s->context=ctx; s->queue=queue;
    cl_command_queue_properties props=0; cl_uint padding=0,page=0; cl_int err=CL_SUCCESS;
    if (clGetCommandQueueInfo(queue,CL_QUEUE_PROPERTIES,sizeof(props),&props,nullptr)!=CL_SUCCESS ||
        (props&CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE) ||
        clGetDeviceInfo(dev,CL_DEVICE_EXT_MEM_PADDING_IN_BYTES_QCOM,sizeof(padding),&padding,nullptr)!=CL_SUCCESS ||
        clGetDeviceInfo(dev,CL_DEVICE_PAGE_SIZE_QCOM,sizeof(page),&page,nullptr)!=CL_SUCCESS ||
        !page || n20_dsp_open(&s->dsp)!=0) goto fail;
    for (unsigned i=0;i<2;++i) {
        if (n20_dsp_ion_alloc(s->dsp,kInputBytes,128,padding,page,&s->input[i])!=0 ||
            n20_dsp_ion_alloc(s->dsp,kOutputBytes,0,padding,page,&s->output[i])!=0 ||
            n20_dsp_ion_alloc(s->dsp,kOutputBytes,0,padding,page,&s->pair_output[i])!=0) goto fail;
        s->input_cl[i]=ion_import(ctx,s->input[i],kInputBytes+128);
        s->output_cl[i]=ion_import(ctx,s->output[i],kOutputBytes);
        s->pair_output_cl[i]=ion_import(ctx,s->pair_output[i],kOutputBytes);
        if (!s->input_cl[i] || !s->output_cl[i] || !s->pair_output_cl[i]) goto fail;
    }
    s->maxima=clCreateBuffer(ctx,CL_MEM_READ_WRITE,kMaxTokens*sizeof(float),nullptr,&err);
    if (err!=CL_SUCCESS || !s->maxima) goto fail;
    s->inverse=clCreateBuffer(ctx,CL_MEM_READ_WRITE,kMaxTokens*sizeof(float),nullptr,&err);
    if (err!=CL_SUCCESS || !s->inverse) goto fail;
    s->guard=clCreateBuffer(ctx,CL_MEM_READ_WRITE,sizeof(cl_uint),nullptr,&err);
    if (err!=CL_SUCCESS || !s->guard) goto fail;
    {
        cl_program row=program(ctx,dev,row_path,"-cl-fast-relaxed-math");
        if (!row) goto fail;
        s->shard=clCreateKernel(row,"kernel_gemm_noshuffle_q2_0_f32",&err);
        clReleaseProgram(row);
        if (err!=CL_SUCCESS || !s->shard) goto fail;
        cl_program aux=program(ctx,dev,aux_path,"-cl-std=CL2.0");
        if (!aux) goto fail;
        s->reduce=clCreateKernel(aux,"reduce_max_token8",&err);
        if (err==CL_SUCCESS) s->pack=clCreateKernel(aux,"pack_q8_block_major",&err);
        if (err==CL_SUCCESS) s->scatter=clCreateKernel(aux,"scatter_q8",&err);
        clReleaseProgram(aux);
        if (err!=CL_SUCCESS || !s->reduce || !s->pack || !s->scatter) goto fail;
    }
    *out=s; return 0;
fail:
    n20_hybrid_close(&s); return -1;
}

extern "C" int n20_hybrid_upload(n20_hybrid *s,const void *raw,size_t size,uint32_t *slot,float *unit) {
    if (!s || !raw || !slot || !unit || s->next_slot>=N20_DSP_CLIENT_SLOTS ||
        size!=(size_t)kRows*kBlocks*18) return -1;
    const uint32_t current=s->next_slot++;
    if (n20_dsp_upload_q2(s->dsp,current,raw,(size_t)kDspRows*kBlocks*18,kDspRows,kColumns,unit)!=0) return -1;
    *slot=current; return 0;
}

extern "C" int n20_hybrid_project(n20_hybrid *s,uint32_t slot,float unit,unsigned n,
                                   unsigned padded,cl_mem input,cl_mem texture,cl_mem codes,
                                   cl_mem scales,cl_mem output,cl_ulong offset) {
    if (s) s->pair_ready=false;
    if (!s || !input || !texture || !codes || !scales || !output || n<16 || n>kMaxTokens ||
        padded!=(n+7)/8*8 || offset%sizeof(float)) return 0;
    struct count_call {
        n20_hybrid *s; bool success=false;
        ~count_call() { ++s->attempts; if (success) ++s->successes; }
    } count_result{s};
    cl_int err=CL_SUCCESS; cl_event event=nullptr; cl_uint zero=0, bad=0;
    cl_uint columns=kColumns, pad=padded, count=n*kBlocks, tokens=n, blocks=kBlocks;
    cl_uint rows=kRows, dsp_rows=kDspRows; float scale=unit/127.0f;
#define ARG(kernel,i,val) do { if (err==CL_SUCCESS) err=clSetKernelArg(kernel,i,sizeof(val),&(val)); } while (0)
    ARG(s->reduce,0,input); ARG(s->reduce,1,s->maxima); ARG(s->reduce,2,s->inverse); ARG(s->reduce,3,s->guard);
    ARG(s->reduce,4,columns); ARG(s->reduce,5,pad); ARG(s->reduce,6,tokens);
    ARG(s->pack,0,input); ARG(s->pack,1,s->input_cl[0]); ARG(s->pack,2,s->input_cl[1]); ARG(s->pack,3,s->inverse);
    ARG(s->pack,4,blocks); ARG(s->pack,5,pad); ARG(s->pack,6,count); ARG(s->pack,7,tokens);
    if (err!=CL_SUCCESS) return 0;
    const size_t group=64, reduce_global=(size_t)((n+7)/8)*64, pack_global=count;
    err=clEnqueueFillBuffer(s->queue,s->guard,&zero,sizeof(zero),0,sizeof(zero),0,nullptr,nullptr);
    if (err==CL_SUCCESS) err=clEnqueueNDRangeKernel(s->queue,s->reduce,1,nullptr,&reduce_global,&group,0,nullptr,nullptr);
    if (err==CL_SUCCESS) err=clEnqueueNDRangeKernel(s->queue,s->pack,1,nullptr,&pack_global,nullptr,0,nullptr,nullptr);
    if (err==CL_SUCCESS) err=clEnqueueReadBuffer(s->queue,s->guard,CL_FALSE,0,sizeof(bad),&bad,0,nullptr,&event);
    if (err==CL_SUCCESS) err=clFlush(s->queue);
    if (err==CL_SUCCESS) err=clWaitForEvents(1,&event);
    /* The nonblocking read targets a stack variable; even an error after
     * enqueue must drain the queue before this function returns. */
    if (err!=CL_SUCCESS && event) clFinish(s->queue);
    if (event) clReleaseEvent(event);
    if (err!=CL_SUCCESS || bad) return 0;
    const int gpu_rows=kRows-kDspRows, physical=kRows, first=kDspRows, output_rows=kRows;
    const int padded_i=(int)padded, cols_i=kColumns, tokens_i=(int)n;
    ARG(s->shard,0,codes); ARG(s->shard,1,scales); ARG(s->shard,2,texture); ARG(s->shard,3,output);
    ARG(s->shard,4,offset); ARG(s->shard,5,gpu_rows); ARG(s->shard,6,padded_i);
    ARG(s->shard,7,cols_i); ARG(s->shard,8,tokens_i); ARG(s->shard,9,physical);
    ARG(s->shard,10,first); ARG(s->shard,11,output_rows); ARG(s->shard,12,first);
    if (err!=CL_SUCCESS) return 0;
    const size_t origin[]={0,0,0}, region[]={padded/4,kColumns,1};
    const size_t global[]={(n+7)/8,((size_t)gpu_rows+511)/512*128,1}, local[]={1,128,1};
    err=clEnqueueCopyBufferToImage(s->queue,input,texture,0,origin,region,0,nullptr,nullptr);
    if (err==CL_SUCCESS) err=clEnqueueNDRangeKernel(s->queue,s->shard,3,nullptr,global,local,0,nullptr,nullptr);
    if (err==CL_SUCCESS) err=clFlush(s->queue);
    if (err!=CL_SUCCESS) return 0;
    n20_dsp_timing timing={};
    const unsigned first_tokens=n<kChunkTokens ? n : kChunkTokens;
    if (n20_dsp_project(s->dsp,slot,first_tokens,&s->input[0],&s->output[0],&timing)!=0) return 0;
    if (n>kChunkTokens && n20_dsp_project(s->dsp,slot,n-kChunkTokens,&s->input[1],&s->output[1],&timing)!=0) return 0;
    ARG(s->scatter,0,s->output_cl[0]); ARG(s->scatter,1,s->output_cl[1]); ARG(s->scatter,2,output); ARG(s->scatter,3,s->maxima);
    ARG(s->scatter,4,offset); ARG(s->scatter,5,rows); ARG(s->scatter,6,dsp_rows);
    ARG(s->scatter,7,tokens); ARG(s->scatter,8,scale);
    const size_t scatter_global=(size_t)n*kDspRows;
    if (err==CL_SUCCESS) err=clEnqueueNDRangeKernel(s->queue,s->scatter,1,nullptr,&scatter_global,nullptr,0,nullptr,nullptr);
#undef ARG
    count_result.success = err==CL_SUCCESS;
    if (count_result.success) { s->pair_ready=true; s->pair_n=n; s->pair_padded=padded; s->pair_texture=texture; }
    return count_result.success ? 1 : 0;
}

/* Only called immediately after a matched first projection. The separate
 * outputs ensure that neither new DSP call races the prior GPU scatter. */
extern "C" int n20_hybrid_project_reuse(n20_hybrid *s,uint32_t slot,float unit,unsigned n,
                                         unsigned padded,cl_mem texture,cl_mem codes,
                                         cl_mem scales,cl_mem output,cl_ulong offset) {
    if (!s) return 0;
    const bool ready=s->pair_ready && s->pair_n==n && s->pair_padded==padded && s->pair_texture==texture;
    s->pair_ready=false; ++s->pair_attempts;
    if (!ready || !texture || !codes || !scales || !output || offset%sizeof(float)) return 0;
    cl_int err=CL_SUCCESS;
    const int gpu_rows=kRows-kDspRows, physical=kRows, first=kDspRows, output_rows=kRows;
    const int padded_i=(int)padded, cols_i=kColumns, tokens_i=(int)n;
    const cl_uint rows=kRows, dsp_rows=kDspRows, tokens=n;
    const float scale=unit/127.0f;
#define ARG(kernel,i,val) do { if (err==CL_SUCCESS) err=clSetKernelArg(kernel,i,sizeof(val),&(val)); } while (0)
    ARG(s->shard,0,codes); ARG(s->shard,1,scales); ARG(s->shard,2,texture); ARG(s->shard,3,output);
    ARG(s->shard,4,offset); ARG(s->shard,5,gpu_rows); ARG(s->shard,6,padded_i);
    ARG(s->shard,7,cols_i); ARG(s->shard,8,tokens_i); ARG(s->shard,9,physical);
    ARG(s->shard,10,first); ARG(s->shard,11,output_rows); ARG(s->shard,12,first);
    const size_t global[]={(n+7)/8,((size_t)gpu_rows+511)/512*128,1}, local[]={1,128,1};
    if (err==CL_SUCCESS) err=clEnqueueNDRangeKernel(s->queue,s->shard,3,nullptr,global,local,0,nullptr,nullptr);
    if (err==CL_SUCCESS) err=clFlush(s->queue);
    if (err!=CL_SUCCESS) return 0;
    n20_dsp_timing timing={};
    const unsigned first_tokens=n<kChunkTokens ? n : kChunkTokens;
    if (n20_dsp_project(s->dsp,slot,first_tokens,&s->input[0],&s->pair_output[0],&timing)!=0) return 0;
    if (n>kChunkTokens && n20_dsp_project(s->dsp,slot,n-kChunkTokens,&s->input[1],&s->pair_output[1],&timing)!=0) return 0;
    ARG(s->scatter,0,s->pair_output_cl[0]); ARG(s->scatter,1,s->pair_output_cl[1]);
    ARG(s->scatter,2,output); ARG(s->scatter,3,s->maxima); ARG(s->scatter,4,offset);
    ARG(s->scatter,5,rows); ARG(s->scatter,6,dsp_rows); ARG(s->scatter,7,tokens); ARG(s->scatter,8,scale);
    const size_t scatter_global=(size_t)n*kDspRows;
    if (err==CL_SUCCESS) err=clEnqueueNDRangeKernel(s->queue,s->scatter,1,nullptr,&scatter_global,nullptr,0,nullptr,nullptr);
#undef ARG
    if (err==CL_SUCCESS) ++s->pair_successes;
    return err==CL_SUCCESS ? 1 : 0;
}
