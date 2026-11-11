/* Optimistic concurrent GPU/DSP Q2_0 row-shard probe.
 *
 * Both devices receive independently prepacked copies of the same FP16-rounded
 * activations. Weight preparation, input quantization and result scatter are
 * outside the measured interval: this is a feasibility bound, not a model path.
 * Build with Android ARM64 NDK, OpenCL headers/library, -ldl and -lm.
 */
#define CL_TARGET_OPENCL_VERSION 200
#include <CL/cl.h>

/* Reuse the existing DSP transport and its exact fixed-point reference. */
#define main n20_v66_reference_main
#include "v66-q2-bench.c"
#undef main

enum { SAMPLES = 5 };

static void cl_check(cl_int status, const char *operation) {
    if (status != CL_SUCCESS) {
        fprintf(stderr, "%s failed: %d\n", operation, status);
        exit(1);
    }
}
#define CL(call) cl_check((call), #call)

static unsigned parse_number(const char *s, unsigned limit) {
    char *end;
    const unsigned long n = strtoul(s, &end, 10);
    if (!*s || *end || n > limit) {
        fprintf(stderr, "Invalid dimension: %s\n", s);
        exit(2);
    }
    return (unsigned)n;
}

static char *read_source(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    if (!file || fseek(file, 0, SEEK_END)) { perror(path); exit(1); }
    const long length = ftell(file);
    if (length <= 0 || length > 1048576 || fseek(file, 0, SEEK_SET)) exit(1);
    char *text = allocate((size_t)length + 1);
    if (fread(text, 1, (size_t)length, file) != (size_t)length || fclose(file)) exit(1);
    text[length] = 0;
    *size = (size_t)length;
    return text;
}

static cl_mem copy_buffer(cl_context context, const void *data, size_t bytes) {
    cl_int status;
    cl_mem result = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                   bytes, (void *)data, &status);
    cl_check(status, "clCreateBuffer(input)");
    return result;
}

struct gpu_projection {
    cl_mem codes, scales, output;
    cl_kernel kernel;
    size_t global[2], local[2];
    unsigned rows;
};

static struct gpu_projection make_gpu_projection(
    cl_context context, cl_program program, cl_mem image, const uint8_t *raw,
    unsigned row_start, unsigned rows, unsigned blocks, unsigned tokens, unsigned padded
) {
    const size_t count = (size_t)rows * blocks;
    uint8_t *codes = allocate(count * 16);
    uint16_t *scales = allocate(count * sizeof(*scales));
    for (unsigned row = 0; row < rows; ++row) {
        for (unsigned block = 0; block < blocks; ++block) {
            const uint8_t *source = raw + ((size_t)(row_start + row) * blocks + block) * Q2_BYTES;
            memcpy(scales + (size_t)block * rows + row, source, sizeof(*scales));
            for (unsigned i = 0; i < 16; ++i)
                codes[((size_t)block * 16 + i) * rows + row] = source[2 + i];
        }
    }
    struct gpu_projection p = {0};
    p.rows = rows;
    p.codes = copy_buffer(context, codes, count * 16);
    p.scales = copy_buffer(context, scales, count * sizeof(*scales));
    free(codes);
    free(scales);
    cl_int status;
    p.output = clCreateBuffer(context, CL_MEM_WRITE_ONLY, (size_t)rows * tokens * sizeof(float), NULL, &status);
    cl_check(status, "clCreateBuffer(output)");
    p.kernel = clCreateKernel(program, "kernel_gemm_noshuffle_q2_0_f32", &status);
    cl_check(status, "clCreateKernel");
    const cl_ulong offset = 0;
    const int m = (int)rows, n = (int)tokens, padded_n = (int)padded, columns = (int)(blocks * Q2_WIDTH);
    #define ARG(i, value) CL(clSetKernelArg(p.kernel, (i), sizeof(value), &(value)))
    ARG(0, p.codes); ARG(1, p.scales); ARG(2, image); ARG(3, p.output);
    ARG(4, offset); ARG(5, m); ARG(6, padded_n); ARG(7, columns); ARG(8, n);
    #undef ARG
    p.local[0] = 1; p.local[1] = 128;
    p.global[0] = (tokens + 7) / 8;
    p.global[1] = ((size_t)rows + 511) / 512 * 128;
    return p;
}

static void release_gpu_projection(struct gpu_projection *p) {
    CL(clReleaseKernel(p->kernel));
    CL(clReleaseMemObject(p->codes));
    CL(clReleaseMemObject(p->scales));
    CL(clReleaseMemObject(p->output));
}

static void rounded_activations(float *exact, int8_t *quantized,
                                struct activation_block *packed, unsigned count) {
    make_activations(exact, quantized, packed, count);
    for (unsigned b = 0; b < count; ++b) {
        packed[b] = (struct activation_block){0};
        for (unsigned j = 0; j < Q2_WIDTH; ++j) {
            const size_t i = (size_t)b * Q2_WIDTH + j;
            exact[i] = (float)(_Float16)exact[i];
            const int q = (int)lroundf(exact[i] * 127);
            if (q < -127 || q > 127) { fputs("Activation outside fixed test range\n", stderr); exit(1); }
            quantized[i] = (int8_t)q;
            packed[b].sum += q;
            packed[b].groups[j / 16][j % 4] |= (uint32_t)(uint8_t)q << (8 * (j % 16 / 4));
        }
    }
}

static void queue_projection(cl_command_queue queue, const struct gpu_projection *p,
                             cl_mem x, cl_mem image, unsigned padded, unsigned columns,
                             cl_event *copy, cl_event *event) {
    const size_t origin[] = {0, 0, 0}, region[] = {padded / 4, columns, 1};
    CL(clEnqueueCopyBufferToImage(queue, x, image, 0, origin, region, 0, NULL, copy));
    CL(clEnqueueNDRangeKernel(queue, p->kernel, 2, NULL, p->global, p->local, 0, NULL, event));
    CL(clFlush(queue));
}

static double event_ms(cl_event start_event, cl_event end_event) {
    cl_ulong start, end;
    CL(clGetEventProfilingInfo(start_event, CL_PROFILING_COMMAND_START, sizeof(start), &start, NULL));
    CL(clGetEventProfilingInfo(end_event, CL_PROFILING_COMMAND_END, sizeof(end), &end, NULL));
    if (end < start) { fputs("Invalid OpenCL event times\n", stderr); exit(1); }
    return (double)(end - start) / 1e6;
}

static void check_sampled(const uint8_t *raw, const float *input, const float *full,
                          const float *gpu, const int32_t *dsp, float unit,
                          unsigned rows, unsigned dsp_rows, unsigned blocks, unsigned tokens) {
    double full_error = 0, hybrid_error = 0, signal = 0, max_error = 0;
    unsigned checked = 0;
    const unsigned gpu_rows = rows - dsp_rows, columns = blocks * Q2_WIDTH;
    for (unsigned token = 0; token < tokens; token += tokens > 16 ? 7 : 1) {
        for (unsigned row = 0; row < rows; row += rows > 1024 ? 97 : 1) {
            double expected = 0;
            for (unsigned b = 0; b < blocks; ++b) {
                const uint8_t *block = raw + ((size_t)row * blocks + b) * Q2_BYTES;
                const float scale = weight_scale(block);
                for (unsigned k = 0; k < Q2_WIDTH; ++k)
                    expected += (double)scale * ((int)code(block, k) - 1) *
                                input[(size_t)token * columns + b * Q2_WIDTH + k];
            }
            const float a = full[(size_t)token * rows + row];
            const double b = row < dsp_rows
                ? dsp[((size_t)(row / HVX_ROWS) * tokens + token) * HVX_ROWS + row % HVX_ROWS] * (double)unit / 127
                : gpu[(size_t)token * gpu_rows + row - dsp_rows];
            if (!isfinite(a) || !isfinite(b)) { fputs("Non-finite output\n", stderr); exit(1); }
            const double da = a - expected, db = b - expected;
            full_error += da * da; hybrid_error += db * db; signal += expected * expected;
            max_error = fmax(max_error, fabs(db));
            ++checked;
        }
    }
    const double full_nrmse = sqrt(full_error / fmax(signal, 1e-30));
    const double hybrid_nrmse = sqrt(hybrid_error / fmax(signal, 1e-30));
    printf("sampled=%u gpu_nrmse=%.7g hybrid_nrmse=%.7g hybrid_max_error=%.7g\n",
           checked, full_nrmse, hybrid_nrmse, max_error);
    if (full_nrmse > 0.003 || hybrid_nrmse > 0.01) {
        fputs("Sampled reference error exceeded test threshold\n", stderr);
        exit(1);
    }
}

int main(int argc, char **argv) {
    if (argc != 7) {
        fprintf(stderr, "usage: %s TEXTURE_KERNEL Q2_FILE ROWS COLUMNS TOKENS DSP_ROWS\n", argv[0]);
        return 2;
    }
    const unsigned rows = parse_number(argv[3], MAX_ROWS), columns = parse_number(argv[4], MAX_COLS);
    const unsigned tokens = parse_number(argv[5], MAX_TOKENS), dsp_rows = parse_number(argv[6], MAX_ROWS);
    if (!rows || rows % HVX_ROWS || !columns || columns % Q2_WIDTH || !tokens ||
        !dsp_rows || dsp_rows % HVX_ROWS || dsp_rows >= rows) {
        fputs("Dimensions require nonzero rows/DSP rows divisible by 32, columns by 64, and DSP rows < rows\n", stderr);
        return 2;
    }
    const unsigned blocks = columns / Q2_WIDTH, gpu_rows = rows - dsp_rows, padded = (tokens + 7) / 8 * 8;
    const size_t raw_bytes = (size_t)rows * blocks * Q2_BYTES;
    uint8_t *raw = allocate(raw_bytes);
    FILE *file = fopen(argv[2], "rb");
    if (!file || fread(raw, 1, raw_bytes, file) != raw_bytes || fgetc(file) != EOF || fclose(file)) {
        fputs("Cannot read tensor or tensor size mismatch\n", stderr); return 1;
    }
    float unit;
    struct weight_tile *weights = pack_weights(raw, dsp_rows, blocks, &unit);
    const size_t weight_bytes = (size_t)(dsp_rows / HVX_ROWS) * blocks * sizeof(*weights);

    void *library = dlopen("libcdsprpc.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    int (*control)(uint32_t, void *, uint32_t) = dlsym(library, "remote_session_control");
    int (*open_rpc)(const char *, uint64_t *) = dlsym(library, "remote_handle64_open");
    int (*invoke)(uint64_t, uint32_t, union rpc_arg *) = dlsym(library, "remote_handle64_invoke");
    int (*close_rpc)(uint64_t) = dlsym(library, "remote_handle64_close");
    void *(*rpc_alloc)(int, uint32_t, int) = dlsym(library, "rpcmem_alloc");
    void (*rpc_free)(void *) = dlsym(library, "rpcmem_free");
    if (!control || !open_rpc || !invoke || !close_rpc || !rpc_alloc || !rpc_free) {
        fputs("Missing FastRPC functions\n", stderr); return 1;
    }
    int unsigned_module[] = {3, 1};
    int status = control(2, unsigned_module, sizeof(unsigned_module));
    uint64_t handle = 0;
    if (!status) status = open_rpc("file:///libn20_projection_skel.so?"
                                   "n20_projection_skel_handle_invoke&_modver=1.0&_dom=cdsp", &handle);
    if (status) { fprintf(stderr, "FastRPC setup: 0x%08x\n", (unsigned)status); return 1; }
    for (size_t offset = 0; offset < weight_bytes; offset += MAX_UPLOAD) {
        struct upload request = {dsp_rows, columns, (uint32_t)offset};
        const size_t remaining = weight_bytes - offset;
        union rpc_arg args[] = {{.buffer = {&request, sizeof(request)}},
                                {.buffer = {(uint8_t *)weights + offset, remaining < MAX_UPLOAD ? remaining : MAX_UPLOAD}}};
        status = invoke(handle, RPC_UPLOAD, args);
        if (status) { fprintf(stderr, "FastRPC upload: 0x%08x\n", (unsigned)status); return 1; }
    }

    const size_t input_bytes = (size_t)tokens * blocks * sizeof(struct activation_block);
    const size_t dsp_bytes = (size_t)tokens * dsp_rows * sizeof(int32_t);
    struct activation_block *dsp_input = rpc_alloc(25, 0, (int)input_bytes);
    int32_t *dsp_output = rpc_alloc(25, 0, (int)dsp_bytes);
    float *exact = allocate((size_t)tokens * columns * sizeof(*exact));
    int8_t *quantized = allocate((size_t)tokens * columns);
    if (!dsp_input || !dsp_output || (uintptr_t)dsp_output % HVX_BYTES) {
        fputs("ION allocation or DSP output alignment failed\n", stderr); return 1;
    }
    rounded_activations(exact, quantized, dsp_input, tokens * blocks);
    _Float16 *input = allocate((size_t)columns * padded * sizeof(*input));
    for (unsigned column = 0; column < columns; ++column)
        for (unsigned token = 0; token < padded; ++token)
            input[(size_t)column * padded + token] = token < tokens ? (_Float16)exact[(size_t)token * columns + column] : 0;

    cl_platform_id platform;
    cl_device_id device;
    CL(clGetPlatformIDs(1, &platform, NULL));
    CL(clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &device, NULL));
    cl_int cl_status;
    cl_context context = clCreateContext(NULL, 1, &device, NULL, NULL, &cl_status);
    cl_check(cl_status, "clCreateContext");
    const cl_queue_properties properties[] = {CL_QUEUE_PROPERTIES, CL_QUEUE_PROFILING_ENABLE, 0};
    cl_command_queue queue = clCreateCommandQueueWithProperties(context, device, properties, &cl_status);
    cl_check(cl_status, "clCreateCommandQueueWithProperties");
    size_t source_size;
    char *source = read_source(argv[1], &source_size);
    const char *source_ptr = source;
    cl_program program = clCreateProgramWithSource(context, 1, &source_ptr, &source_size, &cl_status);
    cl_check(cl_status, "clCreateProgramWithSource");
    cl_status = clBuildProgram(program, 1, &device, "-cl-fast-relaxed-math", NULL, NULL);
    if (cl_status != CL_SUCCESS) {
        size_t size = 0;
        CL(clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, 0, NULL, &size));
        char *log = allocate(size + 1);
        CL(clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, size, log, NULL));
        log[size] = 0; fprintf(stderr, "%s\n", log); free(log);
        cl_check(cl_status, "clBuildProgram");
    }
    free(source);
    cl_mem x = copy_buffer(context, input, (size_t)columns * padded * sizeof(*input));
    const cl_image_format format = {CL_RGBA, CL_HALF_FLOAT};
    const cl_image_desc description = {.image_type = CL_MEM_OBJECT_IMAGE2D,
                                       .image_width = padded / 4, .image_height = columns};
    cl_mem image = clCreateImage(context, CL_MEM_READ_ONLY, &format, &description, NULL, &cl_status);
    cl_check(cl_status, "clCreateImage");
    struct gpu_projection full = make_gpu_projection(context, program, image, raw, 0, rows, blocks, tokens, padded);
    struct gpu_projection shard = make_gpu_projection(context, program, image, raw, dsp_rows, gpu_rows, blocks, tokens, padded);
    printf("rows=%u columns=%u tokens=%u dsp_rows=%u gpu_rows=%u prepacked_input=1\n",
           rows, columns, tokens, dsp_rows, gpu_rows);
    for (int arm = 0; arm < 3; ++arm) {
        const int hybrid = arm == 1;
        const struct gpu_projection *projection = hybrid ? &shard : &full;
        for (int sample = -1; sample < SAMPLES; ++sample) {
            cl_event copy, event;
            struct projection_timing timing = {0};
            struct projection_request request = {tokens};
            union rpc_arg args[] = {{.buffer = {&request, sizeof(request)}},
                                    {.buffer = {dsp_input, input_bytes}},
                                    {.buffer = {&timing, sizeof(timing)}},
                                    {.buffer = {dsp_output, dsp_bytes}}};
            const uint64_t start = now_us();
            queue_projection(queue, projection, x, image, padded, columns, &copy, &event);
            const uint64_t rpc_start = now_us();
            if (hybrid) {
                status = invoke(handle, RPC_PROJECT, args);
                if (status || !timing.elapsed_us || timing.hvx_mode != HVX_MODE_128 || !timing.direct_output) {
                    fprintf(stderr, "FastRPC projection failed: status=0x%08x mode=%d direct=%u\n",
                            (unsigned)status, timing.hvx_mode, timing.direct_output); return 1;
                }
            }
            const uint64_t rpc_end = now_us();
            CL(clFinish(queue));
            const uint64_t end = now_us();
            const double gpu_ms = event_ms(copy, event);
            if (sample >= 0) {
                printf("arm=%s sample=%d wall_ms=%.3f gpu_copy_kernel_ms=%.3f rpc_ms=%.3f dsp_kernel_ms=%.3f\n",
                       arm == 0 ? "baseline-before" : hybrid ? "overlap" : "baseline-after", sample,
                       (end - start) / 1000.0, gpu_ms, hybrid ? (rpc_end - rpc_start) / 1000.0 : 0.0,
                       hybrid ? timing.elapsed_us / 1000.0 : 0.0);
                fflush(stdout);
            }
            CL(clReleaseEvent(copy)); CL(clReleaseEvent(event));
        }
    }
    float *full_output = allocate((size_t)tokens * rows * sizeof(*full_output));
    float *gpu_output = allocate((size_t)tokens * gpu_rows * sizeof(*gpu_output));
    CL(clEnqueueReadBuffer(queue, full.output, CL_TRUE, 0, (size_t)tokens * rows * sizeof(*full_output), full_output, 0, NULL, NULL));
    CL(clEnqueueReadBuffer(queue, shard.output, CL_TRUE, 0, (size_t)tokens * gpu_rows * sizeof(*gpu_output), gpu_output, 0, NULL, NULL));
    for (unsigned t = 0; t < tokens; ++t)
        for (unsigned r = 0; r < gpu_rows; ++r)
            if (memcmp(&full_output[(size_t)t * rows + dsp_rows + r], &gpu_output[(size_t)t * gpu_rows + r], sizeof(float))) {
                fprintf(stderr, "GPU shard mismatch at token=%u row=%u\n", t, r + dsp_rows); return 1;
            }
    puts("gpu_shard_full_output_bitwise_match=1");
    if (verify(raw, weights, exact, quantized, dsp_output, dsp_rows, blocks, tokens, unit)) return 1;
    check_sampled(raw, exact, full_output, gpu_output, dsp_output, unit, rows, dsp_rows, blocks, tokens);

    free(gpu_output); free(full_output);
    release_gpu_projection(&shard); release_gpu_projection(&full);
    CL(clReleaseMemObject(image)); CL(clReleaseMemObject(x)); CL(clReleaseProgram(program));
    CL(clReleaseCommandQueue(queue)); CL(clReleaseContext(context));
    free(input); free(quantized); free(exact); rpc_free(dsp_output); rpc_free(dsp_input);
    free(weights); free(raw);
    if (close_rpc(handle)) { fputs("FastRPC close failed\n", stderr); return 1; }
    dlclose(library);
    return 0;
}
