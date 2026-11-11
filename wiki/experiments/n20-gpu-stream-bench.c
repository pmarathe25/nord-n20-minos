/* Scratch Adreno OpenCL streaming read probe. Reports decimal GB/s for input
 * bytes only; a checksum is read back to verify every stream contributed.
 * This does not represent the Q2 kernel's access pattern or a bandwidth cap. */
#define CL_TARGET_OPENCL_VERSION 200
#include <CL/cl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void ck(cl_int e, const char *where) {
    if (e != CL_SUCCESS) { fprintf(stderr, "%s failed (%d)\n", where, e); exit(1); }
}
#define CL(x) ck((x), #x)
static const char source[] =
"kernel void stream(global const uint4 * restrict src, global uint * restrict dst, uint n) {\n"
" uint id=get_global_id(0), stride=get_global_size(0); uint4 a=(uint4)(0);\n"
" for (uint i=id; i<n; i+=stride) a ^= src[i];\n"
" dst[id]=a.x ^ a.y ^ a.z ^ a.w;\n"
"}\n";
int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr,"usage: %s SIZE_MIB\n",argv[0]); return 2; }
    char *end; long mib=strtol(argv[1],&end,10);
    if (*end || mib<1 || mib>256) return 2;
    const size_t bytes=(size_t)mib*1024*1024, global=49152, local=128;
    const cl_uint count=(cl_uint)(bytes/16);
    cl_platform_id platform; cl_device_id device; cl_int err;
    CL(clGetPlatformIDs(1,&platform,NULL)); CL(clGetDeviceIDs(platform,CL_DEVICE_TYPE_GPU,1,&device,NULL));
    cl_context ctx=clCreateContext(NULL,1,&device,NULL,NULL,&err); ck(err,"context");
    const cl_queue_properties props[]={CL_QUEUE_PROPERTIES,CL_QUEUE_PROFILING_ENABLE,0};
    cl_command_queue queue=clCreateCommandQueueWithProperties(ctx,device,props,&err); ck(err,"queue");
    const char *ptr=source; cl_program prog=clCreateProgramWithSource(ctx,1,&ptr,NULL,&err); ck(err,"program");
    err=clBuildProgram(prog,1,&device,"-cl-std=CL2.0",NULL,NULL);
    if(err!=CL_SUCCESS){ size_t n=0; CL(clGetProgramBuildInfo(prog,device,CL_PROGRAM_BUILD_LOG,0,NULL,&n)); char *log=malloc(n+1); if(!log)return 1; CL(clGetProgramBuildInfo(prog,device,CL_PROGRAM_BUILD_LOG,n,log,NULL)); log[n]=0; fputs(log,stderr); free(log); ck(err,"build"); }
    cl_kernel k=clCreateKernel(prog,"stream",&err); ck(err,"kernel");
    cl_mem src=clCreateBuffer(ctx,CL_MEM_READ_WRITE,bytes,NULL,&err); ck(err,"input");
    cl_mem dst=clCreateBuffer(ctx,CL_MEM_WRITE_ONLY,global*sizeof(cl_uint),NULL,&err); ck(err,"output");
    const cl_uint pattern[4]={0x12345678u,0xabcdef01u,0x98765432u,0x13579bdfu};
    CL(clEnqueueFillBuffer(queue,src,pattern,sizeof(pattern),0,bytes,0,NULL,NULL));
    CL(clSetKernelArg(k,0,sizeof(src),&src)); CL(clSetKernelArg(k,1,sizeof(dst),&dst)); CL(clSetKernelArg(k,2,sizeof(count),&count));
    printf("mib=%ld sample_ms=",mib);
    for(int i=0;i<7;++i){
        cl_event event; CL(clEnqueueNDRangeKernel(queue,k,1,NULL,&global,&local,0,NULL,&event));
        CL(clWaitForEvents(1,&event)); cl_ulong start,stop;
        CL(clGetEventProfilingInfo(event,CL_PROFILING_COMMAND_START,sizeof(start),&start,NULL));
        CL(clGetEventProfilingInfo(event,CL_PROFILING_COMMAND_END,sizeof(stop),&stop,NULL));
        printf("%s%.3f",i?",":"",(double)(stop-start)/1e6); CL(clReleaseEvent(event));
    }
    putchar('\n');
    cl_uint *out=malloc(global*sizeof(*out)); if(!out)return 1;
    CL(clEnqueueReadBuffer(queue,dst,CL_TRUE,0,global*sizeof(*out),out,0,NULL,NULL));
    const cl_uint value=pattern[0]^pattern[1]^pattern[2]^pattern[3];
    for(size_t i=0;i<global;++i){size_t n=i<count ? 1+(count-1-i)/global : 0; if(out[i] != ((n&1)?value:0)){fprintf(stderr,"checksum mismatch at %zu\n",i);return 1;}}
    printf("checked=%zu input_bytes=%zu\n",global,bytes);
    free(out); CL(clReleaseMemObject(dst)); CL(clReleaseMemObject(src)); CL(clReleaseKernel(k));
    CL(clReleaseProgram(prog)); CL(clReleaseCommandQueue(queue)); CL(clReleaseContext(ctx));
    return 0;
}
