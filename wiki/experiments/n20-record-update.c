/* Check Qualcomm command recording updates on the installed Adreno driver. */
#define CL_TARGET_OPENCL_VERSION 200
#define CL_USE_DEPRECATED_OPENCL_1_2_APIS
#include <CL/cl.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct _cl_recording_qcom *recording;
typedef struct {
    cl_uint dispatch_index;
    cl_uint arg_index;
    size_t arg_size;
    const void *arg_value;
} recording_arg;
typedef struct {
    cl_uint dispatch_index;
    const size_t *workgroup_size;
} recording_workgroup;
typedef recording (*new_recording_fn)(cl_command_queue, cl_int *);
typedef cl_int (*end_recording_fn)(recording);
typedef cl_int (*release_recording_fn)(recording);
typedef cl_int (*enqueue_recording_fn)(cl_command_queue, recording, size_t,
    const recording_arg *, size_t, const void *, size_t, const recording_workgroup *,
    size_t, const recording_workgroup *, cl_uint, const cl_event *, cl_event *);

static void check(cl_int status, const char *operation) {
    if (status != CL_SUCCESS) {
        fprintf(stderr, "%s failed: %d\n", operation, status);
        exit(1);
    }
}
#define CHECK(call) check((call), #call)

int main(void) {
    void *library = dlopen("libOpenCL.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) return 1;
    new_recording_fn begin = (new_recording_fn)dlsym(library, "clNewRecordingQCOM");
    end_recording_fn end = (end_recording_fn)dlsym(library, "clEndRecordingQCOM");
    release_recording_fn release = (release_recording_fn)dlsym(library, "clReleaseRecordingQCOM");
    enqueue_recording_fn submit = (enqueue_recording_fn)dlsym(library, "clEnqueueRecordingQCOM");
    if (!begin || !end || !release || !submit) return 1;
    cl_platform_id platform;
    cl_device_id device;
    cl_int error;
    CHECK(clGetPlatformIDs(1, &platform, NULL));
    CHECK(clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &device, NULL));
    cl_context context = clCreateContext(NULL, 1, &device, NULL, NULL, &error);
    check(error, "context");
    cl_command_queue live = clCreateCommandQueue(context, device, 0, &error);
    check(error, "live queue");
    cl_command_queue queue = clCreateCommandQueue(context, device, 1u << 30, &error);
    check(error, "recordable queue");
    const char *source = "kernel void n20_record_update(global uint *x, uint add) {"
                         " size_t i=get_global_id(0); x[i]=x[i]*3+add; }";
    cl_program program = clCreateProgramWithSource(context, 1, &source, NULL, &error);
    check(error, "program");
    CHECK(clBuildProgram(program, 1, &device, "-cl-std=CL2.0", NULL, NULL));
    cl_kernel kernel = clCreateKernel(program, "n20_record_update", &error);
    check(error, "kernel");
    cl_mem buffers[2];
    for (int i = 0; i < 2; ++i) {
        buffers[i] = clCreateBuffer(context, CL_MEM_READ_WRITE, 64 * sizeof(cl_uint), NULL, &error);
        check(error, "buffer");
    }
    cl_uint initial = 1;
    CHECK(clSetKernelArg(kernel, 0, sizeof(cl_mem), &buffers[0]));
    CHECK(clSetKernelArg(kernel, 1, sizeof(initial), &initial));
    recording command = begin(queue, &error);
    check(error, "begin recording");
    size_t global = 32, local = 32;
    CHECK(clEnqueueNDRangeKernel(queue, kernel, 1, NULL, &global, &local, 0, NULL, NULL));
    CHECK(end(command));

    for (int run = 0; run < 3; ++run) {
        cl_uint zero = 0, value = run == 1 ? 7 : 1, output[64];
        for (int i = 0; i < 2; ++i)
            CHECK(clEnqueueFillBuffer(live, buffers[i], &zero, sizeof(zero), 0, sizeof(output), 0, NULL, NULL));
        cl_mem target = buffers[run == 1];
        size_t new_global = run == 1 ? 64 : 32;
        recording_arg args[] = {{0, 0, sizeof(target), &target}, {0, 1, sizeof(value), &value}};
        recording_workgroup work = {0, &new_global};
        CHECK(submit(live, command, 2, args, 0, NULL, 1, &work, 0, NULL, 0, NULL, NULL));
        for (int i = 0; i < 2; ++i) {
            CHECK(clEnqueueReadBuffer(live, buffers[i], CL_TRUE, 0, sizeof(output), output, 0, NULL, NULL));
            for (size_t j = 0; j < 64; ++j) {
                const cl_uint expected = i == (run == 1) && j < new_global ? value : 0;
                if (output[j] != expected) {
                    fprintf(stderr, "run %d buffer %d index %zu got %u expected %u\n",
                            run, i, j, output[j], expected);
                    return 1;
                }
            }
        }
        printf("run %d buffer %d add %u global %zu passed\n", run, run == 1, value, new_global);
    }
    CHECK(clFinish(live));
    CHECK(release(command));
    for (int i = 0; i < 2; ++i) CHECK(clReleaseMemObject(buffers[i]));
    CHECK(clReleaseKernel(kernel));
    CHECK(clReleaseProgram(program));
    CHECK(clReleaseCommandQueue(queue));
    CHECK(clReleaseCommandQueue(live));
    CHECK(clReleaseContext(context));
    dlclose(library);
    return 0;
}
