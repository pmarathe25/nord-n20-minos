/* Export one compiled OpenCL program to a private scratch file for inspection. */
#define CL_TARGET_OPENCL_VERSION 200
#include <CL/cl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void ck(cl_int e,const char *where){if(e!=CL_SUCCESS){fprintf(stderr,"%s: %d\n",where,e);exit(1);}}
#define CL(x) ck((x),#x)
int main(int argc,char **argv){
 if(argc!=4){fprintf(stderr,"usage: %s SOURCE OPTIONS OUTPUT\n",argv[0]);return 2;}
 FILE *f=fopen(argv[1],"rb"); if(!f || fseek(f,0,SEEK_END))return 1;
 long len=ftell(f); if(len<=0 || len>1024*1024 || fseek(f,0,SEEK_SET))return 1;
 char *src=malloc((size_t)len+1); if(!src || fread(src,1,(size_t)len,f)!=(size_t)len)return 1;
 fclose(f); src[len]=0;
 cl_platform_id platform; cl_device_id device; cl_int err;
 CL(clGetPlatformIDs(1,&platform,NULL)); CL(clGetDeviceIDs(platform,CL_DEVICE_TYPE_GPU,1,&device,NULL));
 cl_context ctx=clCreateContext(NULL,1,&device,NULL,NULL,&err); ck(err,"context");
 const char *source=src; size_t length=(size_t)len;
 cl_program program=clCreateProgramWithSource(ctx,1,&source,&length,&err); ck(err,"program");
 err=clBuildProgram(program,1,&device,argv[2],NULL,NULL);
 size_t logs=0; CL(clGetProgramBuildInfo(program,device,CL_PROGRAM_BUILD_LOG,0,NULL,&logs));
 char *log=calloc(logs+1,1); if(!log)return 1;
 if(logs) CL(clGetProgramBuildInfo(program,device,CL_PROGRAM_BUILD_LOG,logs,log,NULL));
 if(*log) fputs(log,stderr); free(log); ck(err,"build");
 cl_kernel kernel=clCreateKernel(program,"kernel_gemm_noshuffle_q2_0_f32",&err); ck(err,"kernel");
 cl_ulong private_bytes=0, local_bytes=0; size_t max_group=0, preferred=0;
 CL(clGetKernelWorkGroupInfo(kernel,device,CL_KERNEL_PRIVATE_MEM_SIZE,sizeof(private_bytes),&private_bytes,NULL));
 CL(clGetKernelWorkGroupInfo(kernel,device,CL_KERNEL_LOCAL_MEM_SIZE,sizeof(local_bytes),&local_bytes,NULL));
 CL(clGetKernelWorkGroupInfo(kernel,device,CL_KERNEL_WORK_GROUP_SIZE,sizeof(max_group),&max_group,NULL));
 CL(clGetKernelWorkGroupInfo(kernel,device,CL_KERNEL_PREFERRED_WORK_GROUP_SIZE_MULTIPLE,sizeof(preferred),&preferred,NULL));
 printf("private_bytes=%llu local_bytes=%llu max_group=%zu preferred_multiple=%zu\n",(unsigned long long)private_bytes,(unsigned long long)local_bytes,max_group,preferred);
 CL(clReleaseKernel(kernel));
 size_t bytes=0; CL(clGetProgramInfo(program,CL_PROGRAM_BINARY_SIZES,sizeof(bytes),&bytes,NULL));
 if(!bytes || bytes>64*1024*1024){fputs("unexpected binary size\n",stderr);return 1;}
 unsigned char *binary=malloc(bytes); if(!binary)return 1;
 CL(clGetProgramInfo(program,CL_PROGRAM_BINARIES,sizeof(binary),&binary,NULL));
 int fd=open(argv[3],O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
 if(fd<0){perror("output");return 1;}
 f=fdopen(fd,"wb"); if(!f){close(fd);return 1;}
 if(fwrite(binary,1,bytes,f)!=bytes || fclose(f))return 1;
 printf("binary_bytes=%zu\n",bytes);
 free(binary); free(src); CL(clReleaseProgram(program)); CL(clReleaseContext(ctx)); return 0;
}
