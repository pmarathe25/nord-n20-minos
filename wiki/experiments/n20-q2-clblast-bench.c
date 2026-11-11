/* Isolated CLBlast HGEMM probe; not a production backend. */
#define main reference_main
#include "n20-q2-opencl-bench.c"
#undef main
#include <time.h>
#include <clblast_c.h>
static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec+ts.tv_nsec*1e-9; }
int main(int argc,char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0); if(argc!=8) return 2;
    int rows=dimension(argv[3],64,262144), columns=dimension(argv[4],64,16384), tokens=dimension(argv[5],1,256), rm=dimension(argv[6],4,8), tn=dimension(argv[7],2,8);
    if(!rows || !columns || !tokens || !rm || !tn || (tn & (tn-1))) return 2;
    int padded=(tokens+7)/8*8, blocks=columns/64;
    size_t weights=(size_t)rows*blocks, ins=(size_t)columns*padded*2, outs=(size_t)rows*tokens*4;
    unsigned char *raw=read_file(argv[2],weights*18,NULL),*codes=alloc(weights*16);
    uint16_t *scales=alloc(weights*2); _Float16 *input=alloc(ins); float *output=alloc(outs);
    for(int r=0;r<rows;r++) for(int b=0;b<blocks;b++) {const unsigned char *v=raw+((size_t)r*blocks+b)*18;memcpy(scales+(size_t)b*rows+r,v,2);for(int j=0;j<16;j++) codes[(b*16+j)*(size_t)rows+r]=v[2+j];}
    uint32_t rng=621; for(int k=0;k<columns;k++)for(int t=0;t<padded;t++){rng=1664525u*rng+1013904223u;input[k*padded+t]=t<tokens?(float)((int)(rng>>16)-32768)/16384:0;}
    cl_platform_id platform;cl_device_id device;cl_int status;
    CL(clGetPlatformIDs(1,&platform,NULL));CL(clGetDeviceIDs(platform,CL_DEVICE_TYPE_GPU,1,&device,NULL));
    cl_context context=clCreateContext(NULL,1,&device,NULL,NULL,&status);check(status,"context");
    const cl_queue_properties props[]={CL_QUEUE_PROPERTIES,CL_QUEUE_PROFILING_ENABLE,0};
    cl_command_queue queue=clCreateCommandQueueWithProperties(context,device,props,&status);check(status,"queue");
        if(getenv("N20_BLAST_TUNE")) {
      const char *names[]={"GEMMK","KREG","KWG","KWI","MDIMA","MDIMC","MWG","NDIMB","NDIMC","NWG","SA","SB","STRM","STRN","VWM","VWN"};
      const size_t values[]={0,1,32,2,8,8,32,8,8,64,0,0,0,0,4,4};
      check(CLBlastOverrideParameters(device,"Xgemm",CLBlastPrecisionHalf,16,names,values),"override 640");
      const char *routine[]={"XGEMM_MIN_INDIRECT_SIZE"};const size_t threshold[]={0};
      check(CLBlastOverrideParameters(device,"GemmRoutine",CLBlastPrecisionHalf,1,routine,threshold),"force indirect");
    }
size_t len;const char *src=(char *)read_file(argv[1],0,&len);cl_program program=clCreateProgramWithSource(context,1,&src,&len,&status);check(status,"program");
    char options[100];snprintf(options,sizeof(options),"-cl-fast-relaxed-math -DRM=%d -DTN=%d",rm,tn);fprintf(stderr,"building expansion/cast kernels\n");status=clBuildProgram(program,1,&device,options,NULL,NULL);fprintf(stderr,"built expansion/cast kernels\n");
    if(status){char log[16384];clGetProgramBuildInfo(program,device,CL_PROGRAM_BUILD_LOG,sizeof(log),log,NULL);fprintf(stderr,"%s\n",log);check(status,"build");}
    cl_kernel expansion=clCreateKernel(program,"expand",&status);check(status,"expand");cl_kernel cast=clCreateKernel(program,"cast",&status);check(status,"cast");
    cl_mem q=upload(context,codes,weights*16),s=upload(context,scales,weights*2),x=upload(context,input,ins);
    cl_mem w=clCreateBuffer(context,CL_MEM_READ_WRITE,(size_t)rows*columns*2,NULL,&status);check(status,"weights");cl_mem y=clCreateBuffer(context,CL_MEM_WRITE_ONLY,outs,NULL,&status);check(status,"output");
cl_mem h=clCreateBuffer(context,CL_MEM_READ_WRITE,outs/2,NULL,&status);check(status,"half output");
#define ARG(k,i,v) CL(clSetKernelArg(k,i,sizeof(v),&v))
    ARG(expansion,0,q);ARG(expansion,1,s);ARG(expansion,2,w);ARG(expansion,3,rows);ARG(expansion,4,columns);
    ARG(cast,0,h);ARG(cast,1,y);
    size_t eg[]={rows,columns/4},el[]={64,1},cg[]={(size_t)rows*tokens},cl[]={128};
    printf("rows=%d columns=%d tokens=%d RM=%d TN=%d\n",rows,columns,tokens,rm,tn);
    for(int run=0;run<7;run++) {cl_event a,b;double start=now();CL(clEnqueueNDRangeKernel(queue,expansion,2,NULL,eg,el,0,NULL,&a));CLBlastStatusCode rc=CLBlastHgemm(CLBlastLayoutColMajor,CLBlastTransposeNo,CLBlastTransposeYes,rows,tokens,columns,0x3c00,w,0,rows,x,0,padded,0,h,0,rows,&queue,NULL);if(rc){fprintf(stderr,"CLBlast error %d\n",rc);return 1;}CL(clEnqueueNDRangeKernel(queue,cast,1,NULL,cg,cl,0,NULL,&b));CL(clFinish(queue));double wall=now()-start;cl_ulong as,ae,bs,be;CL(clGetEventProfilingInfo(a,CL_PROFILING_COMMAND_START,8,&as,NULL));CL(clGetEventProfilingInfo(a,CL_PROFILING_COMMAND_END,8,&ae,NULL));CL(clGetEventProfilingInfo(b,CL_PROFILING_COMMAND_START,8,&bs,NULL));CL(clGetEventProfilingInfo(b,CL_PROFILING_COMMAND_END,8,&be,NULL));printf("run=%d expand_ms=%.3f cast_ms=%.3f span_ms=%.3f wall_ms=%.3f\n",run,(ae-as)/1e6,(be-bs)/1e6,(be-as)/1e6,wall*1000);clReleaseEvent(a);clReleaseEvent(b);}
    CL(clEnqueueReadBuffer(queue,y,CL_TRUE,0,outs,output,0,NULL,NULL));double err=verify(raw,input,output,rows,columns,tokens,padded);return err>0.05;
}
