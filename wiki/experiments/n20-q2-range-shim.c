/* Scratch-only Android OpenCL interposer for aggregate Q2_0 activation ranges.
 *
 * Place this before cl-compat.so in LD_PRELOAD in an isolated worker. It scans
 * only true (unpadded) tokens in the FFN activation image immediately after
 * the buffer-to-image copy and reports aggregate counts; no values are logged.
 * The blocking scan changes timing and is not for production use.
 */
#define _GNU_SOURCE
#define CL_TARGET_OPENCL_VERSION 200
#include <CL/cl.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_KERNELS = 64, MAX_TOKENS = 256 };
struct gemm_args { cl_kernel kernel; cl_mem image; int rows, padded, columns, tokens; unsigned seen; };
struct token_counts { cl_uint over1, over2, over4, over8, nonfinite; };
static pthread_mutex_t args_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t scan_lock = PTHREAD_MUTEX_INITIALIZER;
static struct gemm_args entries[MAX_KERNELS];
static _Thread_local int inside;
static cl_context scan_context;
static cl_device_id scan_device;
static cl_program scan_program;
static cl_kernel scan_kernel;
static cl_mem scan_output;

typedef cl_int (*set_arg_fn)(cl_kernel, cl_uint, size_t, const void *);
typedef cl_int (*copy_fn)(cl_command_queue, cl_mem, cl_mem, size_t, const size_t *, const size_t *,
                          cl_uint, const cl_event *, cl_event *);
typedef cl_int (*release_kernel_fn)(cl_kernel);
static set_arg_fn next_set_arg;
static copy_fn next_copy;
static release_kernel_fn next_release_kernel;
static pthread_once_t set_once = PTHREAD_ONCE_INIT, copy_once = PTHREAD_ONCE_INIT, release_once = PTHREAD_ONCE_INIT;

static void *symbol(const char *name) {
    void *result = dlsym(RTLD_NEXT, name);
    if (!result) { fprintf(stderr, "N20_RANGE_ERROR missing_symbol=%s\n", name); abort(); }
    return result;
}
static void init_set(void) { next_set_arg = (set_arg_fn)symbol("clSetKernelArg"); }
static void init_copy(void) { next_copy = (copy_fn)symbol("clEnqueueCopyBufferToImage"); }
static void init_release(void) { next_release_kernel = (release_kernel_fn)symbol("clReleaseKernel"); }

static int matching_kernel(cl_kernel kernel) {
    char name[128];
    size_t length = 0;
    if (clGetKernelInfo(kernel, CL_KERNEL_FUNCTION_NAME, sizeof(name), name, &length) != CL_SUCCESS ||
        !length || length > sizeof(name)) return 0;
    return !strcmp(name, "kernel_gemm_noshuffle_q2_0_f32");
}

CL_API_ENTRY cl_int CL_API_CALL clSetKernelArg(cl_kernel kernel, cl_uint index, size_t size, const void *value) {
    pthread_once(&set_once, init_set);
    const cl_int status = next_set_arg(kernel, index, size, value);
    if (inside || status != CL_SUCCESS || !value || (index != 2 && (index < 5 || index > 8))) return status;
    if ((index == 2 && size != sizeof(cl_mem)) || (index != 2 && size != sizeof(int))) return status;
    inside = 1;
    pthread_mutex_lock(&args_lock);
    struct gemm_args *entry = NULL, *empty = NULL;
    for (unsigned i = 0; i < MAX_KERNELS; ++i) {
        if (entries[i].kernel == kernel) { entry = &entries[i]; break; }
        if (!entries[i].kernel && !empty) empty = &entries[i];
    }
    if (!entry && empty && matching_kernel(kernel)) { entry = empty; entry->kernel = kernel; }
    if (entry) {
        if (index == 2) memcpy(&entry->image, value, sizeof(entry->image));
        else if (index == 5) memcpy(&entry->rows, value, sizeof(entry->rows));
        else if (index == 6) memcpy(&entry->padded, value, sizeof(entry->padded));
        else if (index == 7) memcpy(&entry->columns, value, sizeof(entry->columns));
        else memcpy(&entry->tokens, value, sizeof(entry->tokens));
        entry->seen |= 1u << index;
    }
    pthread_mutex_unlock(&args_lock);
    inside = 0;
    return status;
}

CL_API_ENTRY cl_int CL_API_CALL clReleaseKernel(cl_kernel kernel) {
    pthread_once(&release_once, init_release);
    if (!inside) {
        pthread_mutex_lock(&args_lock);
        for (unsigned i = 0; i < MAX_KERNELS; ++i)
            if (entries[i].kernel == kernel) memset(&entries[i], 0, sizeof(entries[i]));
        pthread_mutex_unlock(&args_lock);
    }
    return next_release_kernel(kernel);
}

static int initialize_scan(cl_context context, cl_device_id device) {
    if (scan_context) return scan_context == context && scan_device == device;
    const char *source =
        "#pragma OPENCL EXTENSION cl_khr_fp16 : enable\n"
        "kernel void n20_scan(read_only image2d_t x, global uint *out, uint k, uint n) {\n"
        " uint t=get_global_id(0); if (t>=n) return; uint a=0,b=0,c=0,d=0,nf=0;\n"
        " for (uint i=0; i<k; ++i) {\n"
        "  float4 v=convert_float4(read_imageh(x,(int2)(t/4,i)));\n"
        "  float s=(t%4==0 ? v.x : t%4==1 ? v.y : t%4==2 ? v.z : v.w);\n"
        "  uint bits=as_uint(s) & 0x7fffffffu;\n"
        "  if (bits>=0x7f800000u) { ++nf; continue; }\n"
        "  a += bits>0x3f800000u; b += bits>0x40000000u;\n"
        "  c += bits>0x40800000u; d += bits>0x41000000u;\n"
        " }\n"
        " out[t*5]=a; out[t*5+1]=b; out[t*5+2]=c; out[t*5+3]=d; out[t*5+4]=nf;\n"
        "}\n";
    cl_int status;
    cl_program program = clCreateProgramWithSource(context, 1, &source, NULL, &status);
    if (status != CL_SUCCESS || !program) return 0;
    status = clBuildProgram(program, 1, &device, "-cl-std=CL2.0", NULL, NULL);
    if (status != CL_SUCCESS) {
        size_t length = 0;
        if (clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, 0, NULL, &length) == CL_SUCCESS && length < 65536) {
            char *log = malloc(length + 1);
            if (log && clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, length, log, NULL) == CL_SUCCESS) {
                log[length] = 0; fprintf(stderr, "N20_RANGE_ERROR build_log=%s\n", log);
            }
            free(log);
        }
        clReleaseProgram(program); return 0;
    }
    cl_kernel kernel = clCreateKernel(program, "n20_scan", &status);
    if (status != CL_SUCCESS || !kernel) { clReleaseProgram(program); return 0; }
    cl_mem output = clCreateBuffer(context, CL_MEM_WRITE_ONLY, MAX_TOKENS * sizeof(struct token_counts), NULL, &status);
    if (status != CL_SUCCESS || !output) { clReleaseKernel(kernel); clReleaseProgram(program); return 0; }
    scan_program = program; scan_kernel = kernel; scan_output = output;
    scan_context = context; scan_device = device;
    return 1;
}

static void scan(cl_command_queue queue, cl_mem image, const struct gemm_args *entry) {
    cl_context context = NULL;
    cl_device_id device = NULL;
    cl_command_queue_properties properties = 0;
    if (clGetMemObjectInfo(image, CL_MEM_CONTEXT, sizeof(context), &context, NULL) != CL_SUCCESS ||
        clGetCommandQueueInfo(queue, CL_QUEUE_DEVICE, sizeof(device), &device, NULL) != CL_SUCCESS ||
        clGetCommandQueueInfo(queue, CL_QUEUE_PROPERTIES, sizeof(properties), &properties, NULL) != CL_SUCCESS ||
        (properties & CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE)) {
        fputs("N20_RANGE_ERROR context_or_queue\n", stderr); return;
    }
    pthread_mutex_lock(&scan_lock);
    if (!initialize_scan(context, device)) {
        fputs("N20_RANGE_ERROR initialization\n", stderr); pthread_mutex_unlock(&scan_lock); return;
    }
    const cl_uint columns = (cl_uint)entry->columns, tokens = (cl_uint)entry->tokens;
    cl_int status = clSetKernelArg(scan_kernel, 0, sizeof(image), &image);
    if (status == CL_SUCCESS) status = clSetKernelArg(scan_kernel, 1, sizeof(scan_output), &scan_output);
    if (status == CL_SUCCESS) status = clSetKernelArg(scan_kernel, 2, sizeof(columns), &columns);
    if (status == CL_SUCCESS) status = clSetKernelArg(scan_kernel, 3, sizeof(tokens), &tokens);
    const size_t global = tokens;
    if (status == CL_SUCCESS) status = clEnqueueNDRangeKernel(queue, scan_kernel, 1, NULL, &global, NULL, 0, NULL, NULL);
    struct token_counts values[MAX_TOKENS];
    if (status == CL_SUCCESS) status = clEnqueueReadBuffer(queue, scan_output, CL_TRUE, 0,
                                                           tokens * sizeof(values[0]), values, 0, NULL, NULL);
    if (status != CL_SUCCESS) {
        fprintf(stderr, "N20_RANGE_ERROR scan_status=%d\n", status); pthread_mutex_unlock(&scan_lock); return;
    }
    uint64_t over1 = 0, over2 = 0, over4 = 0, over8 = 0, nonfinite = 0;
    unsigned bad_tokens = 0;
    for (unsigned i = 0; i < tokens; ++i) {
        over1 += values[i].over1; over2 += values[i].over2; over4 += values[i].over4;
        over8 += values[i].over8; nonfinite += values[i].nonfinite;
        bad_tokens += values[i].over1 || values[i].nonfinite;
    }
    fprintf(stderr, "N20_RANGE rows=%d columns=%d tokens=%u values=%llu bad_tokens=%u "
                    "over1=%llu over2=%llu over4=%llu over8=%llu nonfinite=%llu\n",
            entry->rows, entry->columns, tokens, (unsigned long long)tokens * columns, bad_tokens,
            (unsigned long long)over1, (unsigned long long)over2, (unsigned long long)over4,
            (unsigned long long)over8, (unsigned long long)nonfinite);
    fflush(stderr);
    pthread_mutex_unlock(&scan_lock);
}

CL_API_ENTRY cl_int CL_API_CALL clEnqueueCopyBufferToImage(
    cl_command_queue queue, cl_mem source, cl_mem image, size_t offset, const size_t *origin,
    const size_t *region, cl_uint nwait, const cl_event *waits, cl_event *event
) {
    pthread_once(&copy_once, init_copy);
    const cl_int status = next_copy(queue, source, image, offset, origin, region, nwait, waits, event);
    if (inside || status != CL_SUCCESS || !origin || !region) return status;
    struct gemm_args match = {0};
    const unsigned needed = (1u << 2) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8);
    pthread_mutex_lock(&args_lock);
    for (unsigned i = 0; i < MAX_KERNELS; ++i) {
        const struct gemm_args *e = &entries[i];
        if (e->kernel && e->image == image && (e->seen & needed) == needed &&
            ((e->rows == 9216 && e->columns == 2560) || (e->rows == 2560 && e->columns == 9216)) &&
            e->tokens >= 16 && e->tokens <= MAX_TOKENS && e->padded >= e->tokens &&
            e->padded % 4 == 0 && region[0] == (size_t)e->padded / 4 &&
            region[1] == (size_t)e->columns && region[2] == 1 &&
            origin[0] == 0 && origin[1] == 0 && origin[2] == 0 && offset == 0) {
            match = *e; break;
        }
    }
    pthread_mutex_unlock(&args_lock);
    if (match.kernel) { inside = 1; scan(queue, image, &match); inside = 0; }
    return status;
}
