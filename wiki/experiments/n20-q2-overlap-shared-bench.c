/* Guarded GPU/DSP row-shard projection with shared ION input/output.
 * Includes GPU packing, guard synchronization, RPC, GPU GEMM and GPU scatter.
 * Static weight packing, initial allocation/upload, and validation are excluded.
 */
#define CL_TARGET_OPENCL_VERSION 200
#include <CL/cl.h>
#include <CL/cl_ext.h>

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

static cl_mem import_ion(cl_context context, void *pointer, int fd, size_t bytes) {
    const cl_mem_ion_host_ptr ion = {
        .ext_host_ptr = {CL_MEM_ION_HOST_PTR_QCOM, CL_MEM_HOST_UNCACHED_QCOM},
        .ion_filedesc = fd, .ion_hostptr = pointer,
    };
    cl_int status;
    cl_mem result = clCreateBuffer(context, CL_MEM_READ_WRITE | CL_MEM_EXT_HOST_PTR_QCOM | CL_MEM_USE_HOST_PTR,
                                   bytes, (void *)&ion, &status);
    cl_check(status, "clCreateBuffer(ION)");
    return result;
}

static size_t ion_allocation(size_t bytes, cl_uint padding, cl_uint page) {
    if (!page || bytes > INT_MAX || (size_t)padding > INT_MAX - bytes ||
        bytes + padding > INT_MAX - (page - 1)) {
        fputs("Invalid ION allocation size\n", stderr); exit(1);
    }
    return ((bytes + padding + page - 1) / page) * page;
}

static void reference_blocks(const float *exact, int8_t *quantized,
                             struct activation_block *packed, unsigned count) {
    for (unsigned b = 0; b < count; ++b) {
        packed[b] = (struct activation_block){0};
        for (unsigned j = 0; j < Q2_WIDTH; ++j) {
            const size_t i = (size_t)b * Q2_WIDTH + j;
            const int q = (int)lroundf(exact[i] * 127);
            if (q < -127 || q > 127) { fputs("Invalid reference input\n", stderr); exit(1); }
            quantized[i] = (int8_t)q;
            packed[b].sum += q;
            packed[b].groups[j / 16][j % 4] |= (uint32_t)(uint8_t)q << (8 * (j % 16 / 4));
        }
    }
}

static cl_program build_auxiliary(cl_context context, cl_device_id device) {
    const char *source =
        "#pragma OPENCL EXTENSION cl_khr_fp16 : enable\n"
        "kernel void pack(global const half *x, global uint *out, uint blocks, uint padded, uint count) {\n"
        "  uint id = get_global_id(0); if (id >= count) return;\n"
#ifdef N20_PACK_BLOCK_MAJOR
        "  uint tokens = count / blocks; uint token = id % tokens, block = id / tokens;\n"
        "  uint output_id = token * blocks + block; uint groups[16] = {0}; int sum = 0; uint invalid = 0;\n"
#else
        "  uint token = id / blocks, block = id % blocks, output_id = id; uint groups[16] = {0}; int sum = 0; uint invalid = 0;\n"
#endif
        "  for (uint j=0; j<64; ++j) {\n"
        "    float v = convert_float(x[(block*64+j)*padded+token]);\n"
        "    int valid = isfinite(v) && fabs(v) <= 1.0f; invalid |= !valid;\n"
        "    int q = valid ? convert_int_rtz(round(v * 127.0f)) : 0; sum += q;\n"
        "    groups[(j/16)*4+j%4] |= ((uint)(uchar)q) << (8*((j%16)/4));\n"
        "  }\n"
        "  for (uint i=0; i<16; ++i) out[output_id*17+i] = groups[i]; out[output_id*17+16] = as_uint(sum);\n"
        "  if (invalid) atomic_or((volatile global uint *)(out+count*17), 1u);\n"
        "}\n"
        "kernel void scatter(global const int *dsp, global const float *gpu, global float *out,\n"
        "                    uint rows, uint dsp_rows, uint tokens, float scale) {\n"
        "  uint i = get_global_id(0); if (i >= rows*tokens) return;\n"
        "  uint t = i / rows, row = i % rows;\n"
        "  if (row < dsp_rows) out[i] = convert_float(dsp[((row/32)*tokens+t)*32+row%32]) * scale;\n"
        "  else out[i] = gpu[t*(rows-dsp_rows)+row-dsp_rows];\n"
        "}\n";
    cl_int status;
    cl_program program = clCreateProgramWithSource(context, 1, &source, NULL, &status);
    cl_check(status, "clCreateProgramWithSource(auxiliary)");
    status = clBuildProgram(program, 1, &device, "-cl-std=CL2.0", NULL, NULL);
    if (status != CL_SUCCESS) {
        size_t length = 0;
        CL(clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, 0, NULL, &length));
        char *log = allocate(length + 1);
        CL(clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, length, log, NULL));
        log[length] = 0; fprintf(stderr, "%s\n", log); free(log);
        cl_check(status, "clBuildProgram(auxiliary)");
    }
    return program;
}

static void baseline(cl_command_queue queue, const struct gpu_projection *full, cl_mem x, cl_mem image,
                     unsigned padded, unsigned columns, const char *arm) {
    for (int sample = -1; sample < SAMPLES; ++sample) {
        cl_event copy, event;
        const uint64_t start = now_us();
        queue_projection(queue, full, x, image, padded, columns, &copy, &event);
        CL(clFinish(queue));
        const uint64_t end = now_us();
        if (sample >= 0)
            printf("arm=%s sample=%d wall_ms=%.3f gpu_copy_kernel_ms=%.3f\n",
                   arm, sample, (end - start) / 1000.0, event_ms(copy, event));
        CL(clReleaseEvent(copy)); CL(clReleaseEvent(event));
    }
    fflush(stdout);
}

int main(int argc, char **argv) {
    if (argc != 7) {
        fprintf(stderr, "usage: %s TEXTURE_KERNEL Q2_FILE ROWS COLUMNS TOKENS DSP_ROWS\n", argv[0]);
        return 2;
    }
    const unsigned rows = parse_number(argv[3], MAX_ROWS), columns = parse_number(argv[4], MAX_COLS);
    const unsigned tokens = parse_number(argv[5], MAX_TOKENS), dsp_rows = parse_number(argv[6], MAX_ROWS);
    const char *guard_test = getenv("N20_GUARD_TEST");
    const int inject_range = guard_test && !strcmp(guard_test, "range");
    const int inject_nan = guard_test && !strcmp(guard_test, "nan");
    const int edge = getenv("N20_EDGE_INPUT") != NULL;
    if (!rows || rows % HVX_ROWS || !columns || columns % Q2_WIDTH || !tokens ||
        !dsp_rows || dsp_rows % HVX_ROWS || dsp_rows >= rows ||
        (guard_test && !inject_range && !inject_nan) || (edge && guard_test)) {
        fputs("Invalid dimensions or test flags\n", stderr); return 2;
    }
    _Static_assert(sizeof(struct activation_block) == 17 * sizeof(uint32_t), "GPU activation layout");
    const unsigned blocks = columns / Q2_WIDTH, gpu_rows = rows - dsp_rows, padded = (tokens + 7) / 8 * 8;
    const size_t raw_bytes = (size_t)rows * blocks * Q2_BYTES;
    const size_t input_bytes = (size_t)tokens * blocks * sizeof(struct activation_block);
    const size_t dsp_bytes = (size_t)tokens * dsp_rows * sizeof(int32_t);
    const size_t full_bytes = (size_t)tokens * rows * sizeof(float);
    uint8_t *raw = allocate(raw_bytes);
    FILE *file = fopen(argv[2], "rb");
    if (!file || fread(raw, 1, raw_bytes, file) != raw_bytes || fgetc(file) != EOF || fclose(file)) {
        fputs("Cannot read tensor or tensor size mismatch\n", stderr); return 1;
    }
    float unit;
    struct weight_tile *weights = pack_weights(raw, dsp_rows, blocks, &unit);
    const size_t weight_bytes = (size_t)(dsp_rows / HVX_ROWS) * blocks * sizeof(*weights);
    float *exact = allocate((size_t)tokens * columns * sizeof(*exact));
    int8_t *quantized = allocate((size_t)tokens * columns);
    struct activation_block *expected_input = allocate(input_bytes);
    rounded_activations(exact, quantized, expected_input, tokens * blocks);
    if (edge) {
        static const float values[] = {-1.0f, 1.0f, -0.5f, 0.5f, -0.0f, 0.0f, -0.0078125f, 0.0078125f};
        for (size_t i = 0; i < (size_t)tokens * columns; ++i) exact[i] = values[i % (sizeof(values) / sizeof(values[0]))];
        reference_blocks(exact, quantized, expected_input, tokens * blocks);
    }
    if (inject_range) exact[0] = 1.5f;
    if (inject_nan) exact[0] = NAN;
    _Float16 *input = allocate((size_t)columns * padded * sizeof(*input));
    for (unsigned column = 0; column < columns; ++column)
        for (unsigned token = 0; token < padded; ++token)
            input[(size_t)column * padded + token] = token < tokens ? (_Float16)exact[(size_t)token * columns + column] : 0;

    void *library = dlopen("libcdsprpc.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    int (*control)(uint32_t, void *, uint32_t) = dlsym(library, "remote_session_control");
    int (*open_rpc)(const char *, uint64_t *) = dlsym(library, "remote_handle64_open");
    int (*invoke)(uint64_t, uint32_t, union rpc_arg *) = dlsym(library, "remote_handle64_invoke");
    int (*close_rpc)(uint64_t) = dlsym(library, "remote_handle64_close");
    void *(*rpc_alloc)(int, uint32_t, int) = dlsym(library, "rpcmem_alloc");
    int (*rpc_fd)(void *) = dlsym(library, "rpcmem_to_fd");
    void (*rpc_free)(void *) = dlsym(library, "rpcmem_free");
    if (!control || !open_rpc || !invoke || !close_rpc || !rpc_alloc || !rpc_fd || !rpc_free) {
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
    cl_uint padding = 0, page = 0;
    CL(clGetDeviceInfo(device, CL_DEVICE_EXT_MEM_PADDING_IN_BYTES_QCOM, sizeof(padding), &padding, NULL));
    CL(clGetDeviceInfo(device, CL_DEVICE_PAGE_SIZE_QCOM, sizeof(page), &page, NULL));
    const size_t qin_size = input_bytes + HVX_BYTES;
    const size_t qin_alloc = ion_allocation(qin_size, padding, page), qout_alloc = ion_allocation(dsp_bytes, padding, page);
    void *qin = rpc_alloc(25, 0, (int)qin_alloc), *qout = rpc_alloc(25, 0, (int)qout_alloc);
    if (!qin || !qout || (uintptr_t)qin % page || (uintptr_t)qout % page || (uintptr_t)qout % HVX_BYTES) {
        fputs("ION allocation or alignment failed\n", stderr); return 1;
    }
    const int qin_fd = rpc_fd(qin), qout_fd = rpc_fd(qout);
    if (qin_fd < 0 || qout_fd < 0) { fputs("ION file descriptor unavailable\n", stderr); return 1; }
    memset(qin, 0, qin_alloc); memset(qout, 0, qout_alloc);
    cl_mem packed = import_ion(context, qin, qin_fd, qin_size);
    cl_mem dsp_output = import_ion(context, qout, qout_fd, dsp_bytes);

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
    // The shared scatter kernel reads the shard; the optimistic helper used WRITE_ONLY.
    CL(clReleaseMemObject(shard.output));
    shard.output = clCreateBuffer(context, CL_MEM_READ_WRITE, (size_t)tokens * gpu_rows * sizeof(float), NULL, &cl_status);
    cl_check(cl_status, "clCreateBuffer(shard output)");
    CL(clSetKernelArg(shard.kernel, 3, sizeof(shard.output), &shard.output));
    cl_mem combined = clCreateBuffer(context, CL_MEM_READ_WRITE, full_bytes, NULL, &cl_status);
    cl_check(cl_status, "clCreateBuffer(combined)");
    cl_program auxiliary = build_auxiliary(context, device);
    cl_kernel pack = clCreateKernel(auxiliary, "pack", &cl_status);
    cl_check(cl_status, "clCreateKernel(pack)");
    cl_kernel scatter = clCreateKernel(auxiliary, "scatter", &cl_status);
    cl_check(cl_status, "clCreateKernel(scatter)");
    const cl_uint packed_count = tokens * blocks, padded_arg = padded, blocks_arg = blocks;
    const cl_uint rows_arg = rows, dsp_rows_arg = dsp_rows, tokens_arg = tokens;
    const float scale = unit / 127.0f;
    #define PARG(kernel, i, value) CL(clSetKernelArg((kernel), (i), sizeof(value), &(value)))
    PARG(pack, 0, x); PARG(pack, 1, packed); PARG(pack, 2, blocks_arg);
    PARG(pack, 3, padded_arg); PARG(pack, 4, packed_count);
    PARG(scatter, 0, dsp_output); PARG(scatter, 1, shard.output); PARG(scatter, 2, combined);
    PARG(scatter, 3, rows_arg); PARG(scatter, 4, dsp_rows_arg); PARG(scatter, 5, tokens_arg); PARG(scatter, 6, scale);
    #undef PARG
    printf("rows=%u columns=%u tokens=%u dsp_rows=%u gpu_rows=%u page=%u padding=%u guard_test=%s edge=%d\n",
           rows, columns, tokens, dsp_rows, gpu_rows, page, padding, guard_test ? guard_test : "none", edge);
    baseline(queue, &full, x, image, padded, columns, "baseline-before");
    float *full_before = allocate(full_bytes), *final_output = allocate(full_bytes);
    CL(clEnqueueReadBuffer(queue, full.output, CL_TRUE, 0, full_bytes, full_before, 0, NULL, NULL));
    int fallback_count = 0;
    for (int sample = -1; sample < SAMPLES; ++sample) {
        const cl_uint zero = 0;
        cl_uint guard = 0;
        cl_event pack_event, guard_event, copy_event, gpu_event, finish_event;
        const size_t count = packed_count, total = (size_t)rows * tokens;
        struct projection_timing timing = {0};
        struct projection_request request = {tokens};
        union rpc_arg args[] = {{.buffer = {&request, sizeof(request)}},
                                {.buffer = {qin, input_bytes}},
                                {.buffer = {&timing, sizeof(timing)}},
                                {.buffer = {qout, dsp_bytes}}};
        const uint64_t start = now_us();
        CL(clEnqueueFillBuffer(queue, packed, &zero, sizeof(zero), input_bytes, sizeof(zero), 0, NULL, NULL));
        CL(clEnqueueNDRangeKernel(queue, pack, 1, NULL, &count, NULL, 0, NULL, &pack_event));
        CL(clEnqueueReadBuffer(queue, packed, CL_FALSE, input_bytes, sizeof(guard), &guard, 0, NULL, &guard_event));
        // Flush and finish the guard before submitting the independent GPU
        // shard. Some drivers wait for later submitted commands as well.
        CL(clFlush(queue));
        CL(clWaitForEvents(1, &guard_event));
        const uint64_t ready = now_us();
        const int fallback = guard != 0;
        if (fallback != (inject_range || inject_nan)) {
            fprintf(stderr, "Unexpected guard=%u\n", guard); return 1;
        }
        if (fallback) {
            ++fallback_count;
            queue_projection(queue, &full, x, image, padded, columns, &copy_event, &gpu_event);
            CL(clEnqueueCopyBuffer(queue, full.output, combined, 0, 0, full_bytes, 0, NULL, &finish_event));
        } else {
            queue_projection(queue, &shard, x, image, padded, columns, &copy_event, &gpu_event);
            status = invoke(handle, RPC_PROJECT, args);
            if (status || !timing.elapsed_us || timing.hvx_mode != HVX_MODE_128 || !timing.direct_output) {
                fprintf(stderr, "FastRPC projection failed: status=0x%08x mode=%d direct=%u\n",
                        (unsigned)status, timing.hvx_mode, timing.direct_output); return 1;
            }
            CL(clEnqueueNDRangeKernel(queue, scatter, 1, NULL, &total, NULL, 0, NULL, &finish_event));
        }
        const uint64_t rpc_end = now_us();
        CL(clFinish(queue));
        const uint64_t end = now_us();
        if (sample >= 0) {
            printf("arm=shared sample=%d wall_ms=%.3f pack_ms=%.3f ready_ms=%.3f gpu_copy_kernel_ms=%.3f "
                   "rpc_enqueue_ms=%.3f dsp_kernel_ms=%.3f finish_ms=%.3f fallback=%d\n",
                   sample, (end - start) / 1000.0, event_ms(pack_event, pack_event), (ready - start) / 1000.0,
                   event_ms(copy_event, gpu_event), (rpc_end - ready) / 1000.0,
                   fallback ? 0.0 : timing.elapsed_us / 1000.0, (end - rpc_end) / 1000.0, fallback);
            fflush(stdout);
        }
        CL(clReleaseEvent(pack_event)); CL(clReleaseEvent(guard_event));
        CL(clReleaseEvent(copy_event)); CL(clReleaseEvent(gpu_event)); CL(clReleaseEvent(finish_event));
    }
    CL(clEnqueueReadBuffer(queue, combined, CL_TRUE, 0, full_bytes, final_output, 0, NULL, NULL));
    if (guard_test) {
        unsigned nan_count = 0;
        for (size_t i = 0; i < (size_t)tokens * rows; ++i) {
            if (isnan(full_before[i])) {
                ++nan_count;
                if (!isnan(final_output[i])) { fputs("Fallback NaN mismatch\n", stderr); return 1; }
            } else if (memcmp(&full_before[i], &final_output[i], sizeof(float))) {
                fputs("Fallback output mismatch\n", stderr); return 1;
            }
        }
        if (fallback_count != SAMPLES + 1) { fputs("Guard failed to trigger every time\n", stderr); return 1; }
        printf("guard_fallbacks=%d finite_values_bitwise_match=1 baseline_nan_count=%u nan_masks_match=1\n",
               fallback_count, nan_count);
    } else {
        struct activation_block *observed_input = allocate(input_bytes);
        int32_t *observed_dsp = allocate(dsp_bytes);
        float *gpu_output = allocate((size_t)tokens * gpu_rows * sizeof(*gpu_output));
        CL(clEnqueueReadBuffer(queue, packed, CL_TRUE, 0, input_bytes, observed_input, 0, NULL, NULL));
        CL(clEnqueueReadBuffer(queue, dsp_output, CL_TRUE, 0, dsp_bytes, observed_dsp, 0, NULL, NULL));
        CL(clEnqueueReadBuffer(queue, shard.output, CL_TRUE, 0, (size_t)tokens * gpu_rows * sizeof(*gpu_output), gpu_output, 0, NULL, NULL));
        if (memcmp(expected_input, observed_input, input_bytes) || memcmp(expected_input, qin, input_bytes)) {
            fputs("GPU-packed input differs from CPU reference or host-visible ION\n", stderr); return 1;
        }
        if (memcmp(observed_dsp, qout, dsp_bytes)) { fputs("GPU and host disagree on DSP output\n", stderr); return 1; }
        if (verify(raw, weights, exact, quantized, observed_dsp, dsp_rows, blocks, tokens, unit)) return 1;
        for (unsigned t = 0; t < tokens; ++t) {
            for (unsigned r = 0; r < rows; ++r) {
                const size_t i = (size_t)t * rows + r;
                if (r >= dsp_rows) {
                    if (memcmp(&full_before[i], &final_output[i], sizeof(float)) ||
                        memcmp(&gpu_output[(size_t)t * gpu_rows + r - dsp_rows], &final_output[i], sizeof(float))) {
                        fprintf(stderr, "GPU/scatter mismatch token=%u row=%u\n", t, r); return 1;
                    }
                } else {
                    const int32_t q = observed_dsp[((size_t)(r / HVX_ROWS) * tokens + t) * HVX_ROWS + r % HVX_ROWS];
                    const float expected = (float)((double)q * unit / 127.0);
                    if (!isfinite(final_output[i]) || fabsf(final_output[i] - expected) > 0.000002f * fmaxf(1.0f, fabsf(expected))) {
                        fprintf(stderr, "DSP scatter mismatch token=%u row=%u\n", t, r); return 1;
                    }
                }
            }
        }
        puts("gpu_pack_full_byte_match=1 ion_visibility_match=1 gpu_shard_full_bitwise_match=1 dsp_scatter_match=1");
        check_sampled(raw, exact, full_before, gpu_output, observed_dsp, unit, rows, dsp_rows, blocks, tokens);
        free(gpu_output); free(observed_dsp); free(observed_input);
    }
    baseline(queue, &full, x, image, padded, columns, "baseline-after");
    CL(clEnqueueReadBuffer(queue, full.output, CL_TRUE, 0, full_bytes, final_output, 0, NULL, NULL));
    for (size_t i = 0; i < (size_t)tokens * rows; ++i) {
        if (isnan(full_before[i])) {
            if (!isnan(final_output[i])) { fputs("Baseline NaN mask changed\n", stderr); return 1; }
        } else if (memcmp(&full_before[i], &final_output[i], sizeof(float))) {
            fputs("Baseline output changed\n", stderr); return 1;
        }
    }
    puts("baseline_before_after_match=1");
    free(final_output); free(full_before);
    CL(clReleaseKernel(scatter)); CL(clReleaseKernel(pack)); CL(clReleaseProgram(auxiliary));
    CL(clReleaseMemObject(combined)); release_gpu_projection(&shard); release_gpu_projection(&full);
    CL(clReleaseMemObject(image)); CL(clReleaseMemObject(x)); CL(clReleaseProgram(program));
    CL(clReleaseMemObject(dsp_output)); CL(clReleaseMemObject(packed));
    CL(clReleaseCommandQueue(queue)); CL(clReleaseContext(context));
    rpc_free(qout); rpc_free(qin); free(input); free(expected_input); free(quantized); free(exact); free(weights); free(raw);
    if (close_rpc(handle)) { fputs("FastRPC close failed\n", stderr); return 1; }
    dlclose(library);
    return 0;
}
