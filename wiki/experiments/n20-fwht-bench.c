/* Compare OpenCL and FastRPC Hadamard paths with identical synthetic input. */
#define CL_TARGET_OPENCL_VERSION 200
#include <CL/cl.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef N20_FWHT_FASTRPC
#include "v66-q2.h"
#include <arm_neon.h>
#include <dlfcn.h>
#include <CL/cl_ext.h>
#endif

enum { SAMPLES = 7 };

static double seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec * 1e-9;
}

static void check(cl_int status, const char *operation) {
    if (status != CL_SUCCESS) {
        fprintf(stderr, "%s failed (%d)\n", operation, status);
        exit(1);
    }
}
#define CL(operation) check((operation), #operation)

static void *allocate(size_t size) {
    void *memory = malloc(size);
    if (!memory)
        exit(1);
    return memory;
}

static void report_extensions(cl_device_id device) {
    size_t size;
    CL(clGetDeviceInfo(device, CL_DEVICE_EXTENSIONS, 0, NULL, &size));
    char *extensions = allocate(size);
    CL(clGetDeviceInfo(device, CL_DEVICE_EXTENSIONS, size, extensions, NULL));
    char *cursor = NULL;
    for (char *name = strtok_r(extensions, " ", &cursor); name; name = strtok_r(NULL, " ", &cursor)) {
        if (strstr(name, "command_buffer") || strstr(name, "subgroup_shuffle") ||
            strstr(name, "host_ptr") || strstr(name, "external_memory"))
            printf("%s\n", name);
    }
    free(extensions);
}

static cl_kernel compile(cl_context context, cl_device_id device, const char *path,
                         int half_input) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        perror(path);
        exit(1);
    }
    char source[8192];
    const size_t length = fread(source, 1, sizeof(source), file);
    if (ferror(file) || !length || length == sizeof(source))
        exit(1);
    fclose(file);
    const char *text = source;
    cl_int status;
    cl_program program = clCreateProgramWithSource(context, 1, &text, &length, &status);
    check(status, "clCreateProgramWithSource");
    const char *options = half_input ? "-cl-std=CL2.0 -cl-fast-relaxed-math -DSRC_TYPE=half"
                                     : "-cl-std=CL2.0 -cl-fast-relaxed-math -DSRC_TYPE=float";
    status = clBuildProgram(program, 1, &device, options, NULL, NULL);
    if (status != CL_SUCCESS) {
        size_t size;
        CL(clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, 0, NULL, &size));
        char *log = allocate(size + 1);
        CL(clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, size, log, NULL));
        log[size] = 0;
        fprintf(stderr, "%s\n", log);
        free(log);
        check(status, "clBuildProgram");
    }
    cl_kernel kernel = clCreateKernel(program, "kernel_fwht", &status);
    check(status, "clCreateKernel");
    CL(clReleaseProgram(program));
    return kernel;
}

#ifdef N20_FWHT_FASTRPC

static cl_mem import_ion(cl_context context, void *ptr, int fd, size_t size) {
    const cl_mem_ion_host_ptr ion = {
        .ext_host_ptr = {CL_MEM_ION_HOST_PTR_QCOM, CL_MEM_HOST_UNCACHED_QCOM},
        .ion_filedesc = fd,
        .ion_hostptr = ptr,
    };
    cl_int status;
    cl_mem memory = clCreateBuffer(context, CL_MEM_READ_WRITE | CL_MEM_EXT_HOST_PTR_QCOM | CL_MEM_USE_HOST_PTR,
                                   size, (void *)&ion, &status);
    check(status, "clCreateBuffer(ION)");
    return memory;
}

static void shared_fwht(void *library, uint64_t handle, int (*invoke)(uint64_t, uint32_t, union rpc_arg *),
                        cl_context context, cl_device_id device, cl_command_queue queue,
                        cl_mem x, cl_mem y, cl_kernel gpu, const float *reference, size_t rows, float scale) {
    enum { SYSTEM_ION_HEAP = 25 };
    void *(*rpc_alloc)(int, uint32_t, int) = dlsym(library, "rpcmem_alloc");
    int (*rpc_fd)(void *) = dlsym(library, "rpcmem_to_fd");
    void (*rpc_free)(void *) = dlsym(library, "rpcmem_free");
    if (!rpc_alloc || !rpc_fd || !rpc_free)
        exit(1);
    cl_uint padding, page;
    CL(clGetDeviceInfo(device, CL_DEVICE_EXT_MEM_PADDING_IN_BYTES_QCOM, sizeof(padding), &padding, NULL));
    CL(clGetDeviceInfo(device, CL_DEVICE_PAGE_SIZE_QCOM, sizeof(page), &page, NULL));
    const char *percent = getenv("N20_FWHT_GPU_PERCENT");
    char *end;
    const unsigned long share = percent ? strtoul(percent, &end, 10) : 0;
    if (!page || (percent && (!*percent || *end || share > 95)))
        exit(2);
    const size_t gpu_rows = rows * share / 100;
    const size_t dsp_rows = rows - gpu_rows;
    const size_t count = dsp_rows * FWHT_WIDTH;
    const size_t bytes = count * sizeof(float);
    const size_t output_bytes = rows * FWHT_WIDTH * sizeof(float);
    const size_t packed_bytes = bytes + HVX_BYTES;
    const size_t allocation = ((packed_bytes + padding + page - 1) / page) * page;
    void *qin = rpc_alloc(SYSTEM_ION_HEAP, 0, (int)allocation);
    void *qout = rpc_alloc(SYSTEM_ION_HEAP, 0, (int)allocation);
    if (!qin || !qout) {
        fprintf(stderr, "rpcmem_alloc failed for %zu bytes (page %u padding %u)\n", allocation, page, padding);
        exit(1);
    }
    const int fd_in = rpc_fd(qin), fd_out = rpc_fd(qout);
    if (fd_in < 0 || fd_out < 0 || (uintptr_t)qin % page || (uintptr_t)qout % page) {
        fprintf(stderr, "rpcmem gave unusable file descriptors or alignment\n");
        exit(1);
    }
    cl_mem pack_buffer = import_ion(context, qin, fd_in, packed_bytes);
    cl_mem unpack_buffer = import_ion(context, qout, fd_out, bytes);
    // Compare magnitudes as bits so infinities and NaNs also set the flag.
    // The first word after the packed input is reserved for this flag.
    const char *source =
        "kernel void pack(global const float *x, global int *y, uint start, float gain, float shift, int guard) {"
        "uint i = get_global_id(0); float4 v = vload4(i + start*128,x) + shift;"
        "if (guard && i == 0) v.x = 64.0f;"
        "if (any((as_uint4(v) & 0x7fffffffu) > 0x42000000u))"
        "    atomic_or((volatile global int *)(y + get_global_size(0)*4), 1);"
        "vstore4(convert_int4_sat_rte(clamp(v,-32.0f,32.0f)*gain),i,y);"
        "}"
        "kernel void unpack(global const int *x, global float *y, uint start, float inv) {"
        "uint i = get_global_id(0);"
        "vstore4(convert_float4(vload4(i,x))*inv,i + start*128,y);"
        "}";
    cl_int status;
    cl_program program = clCreateProgramWithSource(context, 1, &source, NULL, &status);
    check(status, "clCreateProgramWithSource(conversion)");
    CL(clBuildProgram(program, 1, &device, "-cl-std=CL2.0", NULL, NULL));
    cl_kernel pack = clCreateKernel(program, "pack", &status);
    check(status, "clCreateKernel(pack)");
    cl_kernel unpack = clCreateKernel(program, "unpack", &status);
    check(status, "clCreateKernel(unpack)");
    const cl_uint base_row = (cl_uint)gpu_rows;
    CL(clSetKernelArg(pack, 0, sizeof(x), &x));
    CL(clSetKernelArg(pack, 1, sizeof(pack_buffer), &pack_buffer));
    CL(clSetKernelArg(pack, 2, sizeof(base_row), &base_row));
    CL(clSetKernelArg(unpack, 0, sizeof(unpack_buffer), &unpack_buffer));
    CL(clSetKernelArg(unpack, 1, sizeof(y), &y));
    CL(clSetKernelArg(unpack, 2, sizeof(base_row), &base_row));
    const size_t count4 = count / 4, group = 64;
    const size_t gpu_global[] = {32, gpu_rows}, gpu_local[] = {32, 1};
    const size_t full_global[] = {32, rows};
    const cl_uint zero = 0;
    float *output = allocate(output_bytes);
    for (int i = 0; i < SAMPLES; ++i) {
        // A magnitude of at most 32 with this gain leaves headroom for 512 terms.
        // Change both sign and DC level to test coherence on every invocation.
        const float gain = i & 1 ? -65536.0f : 65536.0f;
        const float inv = scale / gain;
        const float shift = i / 128.0f;
        const cl_int guard = i == 0;
        CL(clSetKernelArg(pack, 3, sizeof(gain), &gain));
        CL(clSetKernelArg(pack, 4, sizeof(shift), &shift));
        CL(clSetKernelArg(pack, 5, sizeof(guard), &guard));
        CL(clSetKernelArg(unpack, 3, sizeof(inv), &inv));
        const double start = seconds();
        CL(clEnqueueFillBuffer(queue, pack_buffer, &zero, sizeof(zero), bytes, sizeof(zero), 0, NULL, NULL));
        CL(clEnqueueNDRangeKernel(queue, pack, 1, NULL, &count4, &group, 0, NULL, NULL));
        CL(clFinish(queue));
        const double packed = seconds();
        cl_event event = NULL;
        if (gpu_rows) {
            // Submit the independent prefix of rows before entering FastRPC.
            CL(clEnqueueNDRangeKernel(queue, gpu, 2, NULL, gpu_global, gpu_local, 0, NULL, &event));
            CL(clFlush(queue));
        }
        const double rpc = seconds();
        struct projection_timing timing = {0};
        union rpc_arg args[] = {
            {.buffer = {qin, packed_bytes}},
            {.buffer = {&timing, sizeof(timing)}},
            {.buffer = {qout, bytes}},
        };
        status = invoke(handle, RPC_FWHT_BOUNDED, args);
        const double restored = seconds();
        const int fallback = !status && timing.flags == FWHT_INPUT_OUT_OF_RANGE;
        if (fallback != guard || (!fallback && (status || !timing.elapsed_us || timing.hvx_mode != HVX_MODE_128))) {
            fprintf(stderr, "Guarded FastRPC transform failed: guard=%d status=0x%08x\n", guard, (unsigned)status);
            exit(1);
        }
        if (fallback) {
            CL(clEnqueueNDRangeKernel(queue, gpu, 2, NULL, full_global, gpu_local, 0, NULL, NULL));
        } else {
            CL(clEnqueueNDRangeKernel(queue, unpack, 1, NULL, &count4, &group, 0, NULL, NULL));
        }
        CL(clFinish(queue));
        const double end = seconds();
        double gpu_ms = 0;
        if (event) {
            cl_ulong first, last;
            CL(clGetEventProfilingInfo(event, CL_PROFILING_COMMAND_START, sizeof(first), &first, NULL));
            CL(clGetEventProfilingInfo(event, CL_PROFILING_COMMAND_END, sizeof(last), &last, NULL));
            gpu_ms = (last-first) / 1e6;
            CL(clReleaseEvent(event));
        }
        CL(clEnqueueReadBuffer(queue, y, CL_TRUE, 0, output_bytes, output, 0, NULL, NULL));
        double max_error = 0;
        for (size_t j = 0; j < rows * FWHT_WIDTH; ++j) {
            const float shift_dc = (!fallback && j / FWHT_WIDTH >= gpu_rows && j % FWHT_WIDTH == 0) ? shift * FWHT_WIDTH * scale : 0;
            if (!isfinite(output[j]))
                exit(1);
            max_error = fmax(max_error, fabs(output[j] - (reference[j] + shift_dc)));
        }
        printf("shared rows=%zu gpu_rows=%zu sample=%d gpu_ms=%.3f dsp_ms=%.3f rpc_ms=%.3f pack_ms=%.3f "
               "finish_ms=%.3f total_ms=%.3f max_error=%.8g fallback=%d\n",
               rows, gpu_rows, i, gpu_ms, timing.elapsed_us / 1000.0, 1000*(restored-rpc),
               1000*(packed-start),1000*(end-restored),1000*(end-start),max_error,fallback);
        if (max_error > 0.0001)
            exit(1);
    }
    CL(clReleaseKernel(pack)); CL(clReleaseKernel(unpack)); CL(clReleaseProgram(program));
    CL(clReleaseMemObject(pack_buffer)); CL(clReleaseMemObject(unpack_buffer));
    rpc_free(qin); rpc_free(qout);
    free(output);
}
static void hexagon(cl_context context, cl_device_id device, cl_command_queue queue, cl_mem x, cl_mem y, cl_kernel gpu, const float *reference,
                    size_t rows, float scale) {
    void *library = dlopen("libcdsprpc.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        fprintf(stderr, "FastRPC: %s\n", dlerror());
        exit(1);
    }
    int (*control)(uint32_t, void *, uint32_t) = dlsym(library, "remote_session_control");
    int (*open)(const char *, uint64_t *) = dlsym(library, "remote_handle64_open");
    int (*invoke)(uint64_t, uint32_t, union rpc_arg *) = dlsym(library, "remote_handle64_invoke");
    int (*close)(uint64_t) = dlsym(library, "remote_handle64_close");
    if (!control || !open || !invoke || !close)
        exit(1);
    int unsigned_session[] = {3, 1};
    uint64_t handle = 0;
    int status = control(2, unsigned_session, sizeof(unsigned_session));
    if (!status)
        status = open("file:///libn20_projection_skel.so?"
                      "n20_projection_skel_handle_invoke&_modver=1.0&_dom=cdsp", &handle);
    if (status) {
        fprintf(stderr, "FastRPC setup failed: 0x%08x\n", (unsigned)status);
        exit(1);
    }

    if (getenv("N20_FWHT_SHARED")) {
        shared_fwht(library, handle, invoke, context, device, queue, x, y, gpu, reference, rows, scale);
        status = close(handle);
        dlclose(library);
        if (status)
            exit(1);
        return;
    }
    const size_t count = rows * FWHT_WIDTH;
    const size_t bytes = count * sizeof(float);
    float *io = allocate(bytes);
    void *packed_input, *packed_output;
    if (posix_memalign(&packed_input, 4096, bytes) || posix_memalign(&packed_output, 4096, bytes))
        exit(1);
    int32_t *qin = packed_input;
    const int32_t *qout = packed_output;
    for (int i = 0; i < SAMPLES; ++i) {
        const double start = seconds();
        CL(clEnqueueReadBuffer(queue, x, CL_TRUE, 0, bytes, io, 0, NULL, NULL));
        const double read = seconds();
        float32x4_t maximum = vdupq_n_f32(0);
        for (size_t j = 0; j < count; j += 4)
            maximum = vmaxq_f32(maximum, vabsq_f32(vld1q_f32(io + j)));
        // Leave headroom for 512 sums in signed 32-bit arithmetic.
        const float gain = 1048576.0f / fmaxf(vmaxvq_f32(maximum), 1.0e-20f);
        for (size_t j = 0; j < count; j += 4)
            vst1q_s32(qin + j, vcvtnq_s32_f32(vmulq_n_f32(vld1q_f32(io + j), gain)));
        struct projection_timing timing = {0};
        union rpc_arg args[] = {
            {.buffer = {qin, bytes}},
            {.buffer = {&timing, sizeof(timing)}},
            {.buffer = {packed_output, bytes}},
        };
        const double rpc = seconds();
        status = invoke(handle, RPC_FWHT, args);
        const double restored = seconds();
        if (status || !timing.elapsed_us || timing.hvx_mode != HVX_MODE_128) {
            fprintf(stderr, "FastRPC transform failed: 0x%08x\n", (unsigned)status);
            exit(1);
        }
        for (size_t j = 0; j < count; j += 4)
            vst1q_f32(io + j, vmulq_n_f32(vcvtq_f32_s32(vld1q_s32(qout + j)), scale / gain));
        const double write = seconds();
        CL(clEnqueueWriteBuffer(queue, y, CL_TRUE, 0, bytes, io, 0, NULL, NULL));
        printf("dsp rows=%zu sample=%d kernel_ms=%.3f rpc_ms=%.3f read_ms=%.3f "
               "convert_ms=%.3f write_ms=%.3f total_ms=%.3f direct_output=%u\n",
               rows, i, timing.elapsed_us / 1000.0, 1000*(restored-rpc), 1000*(read-start),
               1000*((rpc-read)+(write-restored)), 1000*(seconds()-write),
               1000*(seconds()-start), timing.direct_output);
    }
    CL(clEnqueueReadBuffer(queue, y, CL_TRUE, 0, bytes, io, 0, NULL, NULL));
    double error = 0, signal = 0, max_error = 0;
    for (size_t i = 0; i < count; ++i) {
        const double diff = io[i] - reference[i];
        error += diff * diff;
        signal += (double)reference[i] * reference[i];
        max_error = fmax(max_error, fabs(diff));
    }
    const double nrmse = sqrt(error / fmax(signal, 1e-30));
    printf("dsp max_error=%.8g nrmse=%.8g\n", max_error, nrmse);
    free(packed_output);
    free(packed_input);
    free(io);
    status = close(handle);
    dlclose(library);
    if (status || !isfinite(nrmse) || nrmse > 0.00001)
        exit(1);
}
#endif

int main(int argc, char **argv) {
    const long parsed_n = argc >= 4 ? strtol(argv[2], NULL, 10) : 0;
    const long parsed_rows = argc >= 4 ? strtol(argv[3], NULL, 10) : 0;
    const int half_input = argc == 5 && !strcmp(argv[4], "--f16");
    if ((argc != 4 && !half_input) || parsed_n < 16 || parsed_n > 4096 || (parsed_n & (parsed_n - 1)) ||
        parsed_rows < 1 || parsed_rows > 32768) {
        fprintf(stderr, "Usage: %s KERNEL WIDTH ROWS [--f16]\n", argv[0]);
        return 2;
    }
    const int width = (int)parsed_n;
    const cl_ulong rows = (cl_ulong)parsed_rows;
    const cl_ulong zero = 0, one = 1;
    const cl_ulong stride = width * (half_input ? sizeof(_Float16) : sizeof(float));
    const size_t work_bytes = width * sizeof(float);
    const float scale = 1.0f / sqrtf((float)width);
    const size_t bytes = (size_t)rows * work_bytes;
    float *input = allocate(bytes);
    float *output = allocate(bytes);
    uint32_t random = 917;
    for (size_t i = 0; i < bytes / sizeof(float); ++i) {
        random = random * 1664525u + 1013904223u;
        input[i] = (float)((int)(random >> 16) - 32768) / 16381;
    }

    cl_platform_id platform;
    cl_device_id device;
    CL(clGetPlatformIDs(1, &platform, NULL));
    CL(clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &device, NULL));
    report_extensions(device);
    cl_int status;
    cl_context context = clCreateContext(NULL, 1, &device, NULL, NULL, &status);
    check(status, "clCreateContext");
    const cl_queue_properties properties[] = {CL_QUEUE_PROPERTIES, CL_QUEUE_PROFILING_ENABLE, 0};
    cl_command_queue queue = clCreateCommandQueueWithProperties(context, device, properties, &status);
    check(status, "clCreateCommandQueueWithProperties");
    _Float16 *half = NULL;
    if (half_input) {
        half = allocate((size_t)rows * stride);
        for (size_t i = 0; i < bytes / sizeof(float); ++i) {
            half[i] = input[i];
            input[i] = half[i];
        }
    }
    cl_mem x = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, (size_t)rows * stride,
                              half_input ? (void *)half : (void *)input, &status);
    free(half);
    check(status, "clCreateBuffer(input)");
    cl_mem y = clCreateBuffer(context, CL_MEM_WRITE_ONLY, bytes, NULL, &status);
    check(status, "clCreateBuffer(output)");
    cl_kernel kernel = compile(context, device, argv[1], half_input);
    size_t limit, required[3];
    CL(clGetKernelWorkGroupInfo(kernel, device, CL_KERNEL_WORK_GROUP_SIZE, sizeof(limit), &limit, NULL));
    CL(clGetKernelWorkGroupInfo(kernel, device, CL_KERNEL_COMPILE_WORK_GROUP_SIZE,
                               sizeof(required), required, NULL));
    size_t group = required[0];
#ifdef N20_FWHT_FASTRPC
    if (getenv("N20_FWHT_DSP") && getenv("N20_FWHT_SHARED") &&
        (group != 32 || width != FWHT_WIDTH || half_input || rows > FWHT_MAX_ROWS)) {
        fprintf(stderr, "Shared probe requires the width-512 radix kernel and F32 input\n");
        return 2;
    }
#endif
    if (!group) {
        group = 128;
        while (group > limit || group > (size_t)width / 2)
            group /= 2;
    } else if (group > limit || required[1] != 1 || required[2] != 1 || width != 512) {
        fprintf(stderr, "Fixed workgroup requires width 512 and a compatible device\n");
        return 2;
    }
    const size_t local[] = {group, 1}, global[] = {group, (size_t)rows};

    #define ARG(i, v) CL(clSetKernelArg(kernel, i, sizeof(v), &(v)))
    ARG(0, x); ARG(1, zero); ARG(2, y); ARG(3, zero); ARG(4, width); ARG(5, rows);
    ARG(6, one); ARG(7, stride); ARG(8, zero); ARG(9, zero); ARG(10, scale);
    #undef ARG
    CL(clSetKernelArg(kernel, 11, work_bytes, NULL));
    printf("width=%d rows=%" PRIu64 " group=%zu gpu_ms=", width, (uint64_t)rows, group);
    double wall[SAMPLES];
    for (int i = 0; i < SAMPLES; ++i) {
        cl_event event;
        cl_ulong start, end;
        const double submitted = seconds();
        CL(clEnqueueNDRangeKernel(queue, kernel, 2, NULL, global, local, 0, NULL, &event));
        CL(clWaitForEvents(1, &event));
        wall[i] = 1000 * (seconds() - submitted);
        CL(clGetEventProfilingInfo(event, CL_PROFILING_COMMAND_START, sizeof(start), &start, NULL));
        CL(clGetEventProfilingInfo(event, CL_PROFILING_COMMAND_END, sizeof(end), &end, NULL));
        printf("%s%.3f", i ? "," : "", (double)(end - start) / 1e6);
        CL(clReleaseEvent(event));
    }
    printf(" wall_ms=");
    for (int i = 0; i < SAMPLES; ++i)
        printf("%s%.3f", i ? "," : "", wall[i]);
    putchar('\n');
    CL(clEnqueueReadBuffer(queue, y, CL_TRUE, 0, bytes, output, 0, NULL, NULL));
    double max_error = 0;
    for (size_t row = 0; row < rows; ++row) {
        float *values = input + row * width;
        for (int i = 0; i < width; ++i)
            values[i] *= scale;
        for (int h = 1; h < width; h *= 2) {
            for (int i = 0; i < width; i += 2*h) {
                for (int j = i; j < i+h; ++j) {
                    const float a = values[j], b = values[j+h];
                    values[j] = a + b;
                    values[j+h] = a - b;
                }
            }
        }
        for (int i = 0; i < width; ++i) {
            const float value = output[row * width + i];
            if (!isfinite(value))
                return 1;
            max_error = fmax(max_error, fabs(value - values[i]));
        }
    }
    printf("max_error=%.8g\n", max_error);
#ifdef N20_FWHT_FASTRPC
    if (getenv("N20_FWHT_DSP") && !half_input && width == FWHT_WIDTH && rows <= FWHT_MAX_ROWS)
        hexagon(context, device, queue, x, y, kernel, input, rows, scale);
#endif
    CL(clReleaseMemObject(y)); CL(clReleaseMemObject(x));
    CL(clReleaseKernel(kernel)); CL(clReleaseCommandQueue(queue)); CL(clReleaseContext(context));
    free(output);
    free(input);
    return max_error > 0.0001 ? 1 : 0;
}
