/* Experimental per-token dynamic Q8 GPU/DSP row-view projection.
 * Includes GPU reduction, packing, guard synchronization, RPC, GEMM and scatter.
 * Static weight packing, initial allocation/upload, and validation are excluded.
 * The finite |activation| <= 16 guard is a provisional empirical test limit,
 * not a proof that every tensor or input below it is safe or accurate.
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
    unsigned row_start, unsigned rows, unsigned blocks, unsigned tokens, unsigned padded,
    const struct gpu_projection *backing, cl_mem shared_output
) {
    struct gpu_projection p = {0};
    p.rows = rows;
    cl_int status;
    if (backing) {
        if (!shared_output || row_start + rows > backing->rows) { fputs("Invalid row view\n", stderr); exit(1); }
        p.codes = backing->codes; p.scales = backing->scales; p.output = shared_output;
        CL(clRetainMemObject(p.codes)); CL(clRetainMemObject(p.scales)); CL(clRetainMemObject(p.output));
    } else {
        if (row_start || shared_output) { fputs("Invalid full projection\n", stderr); exit(1); }
        const size_t count = (size_t)rows * blocks;
        uint8_t *codes = allocate(count * 16);
        uint16_t *scales = allocate(count * sizeof(*scales));
        for (unsigned row = 0; row < rows; ++row) {
            for (unsigned block = 0; block < blocks; ++block) {
                const uint8_t *source = raw + ((size_t)row * blocks + block) * Q2_BYTES;
                memcpy(scales + (size_t)block * rows + row, source, sizeof(*scales));
                for (unsigned i = 0; i < 16; ++i)
                    codes[((size_t)block * 16 + i) * rows + row] = source[2 + i];
            }
        }
        p.codes = copy_buffer(context, codes, count * 16);
        p.scales = copy_buffer(context, scales, count * sizeof(*scales));
        free(codes); free(scales);
        p.output = clCreateBuffer(context, CL_MEM_WRITE_ONLY, (size_t)rows * tokens * sizeof(float), NULL, &status);
        cl_check(status, "clCreateBuffer(output)");
    }
    p.kernel = clCreateKernel(program, "kernel_gemm_noshuffle_q2_0_f32", &status);
    cl_check(status, "clCreateKernel");
    const cl_ulong offset = 0;
    const int m = (int)rows, n = (int)tokens, padded_n = (int)padded, columns = (int)(blocks * Q2_WIDTH);
    const int physical_rows = (int)(backing ? backing->rows : rows), first_row = (int)row_start;
    const int output_rows = physical_rows, output_first_row = first_row;
    #define ARG(i, value) CL(clSetKernelArg(p.kernel, (i), sizeof(value), &(value)))
    ARG(0, p.codes); ARG(1, p.scales); ARG(2, image); ARG(3, p.output);
    ARG(4, offset); ARG(5, m); ARG(6, padded_n); ARG(7, columns); ARG(8, n);
    ARG(9, physical_rows); ARG(10, first_row); ARG(11, output_rows); ARG(12, output_first_row);
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
                          const float *gpu, const int32_t *dsp, const float *maxima, float unit,
                          unsigned rows, unsigned dsp_rows, unsigned blocks, unsigned tokens) {
    double full_error = 0, hybrid_error = 0, signal = 0, max_error = 0;
    unsigned checked = 0;
    const unsigned columns = blocks * Q2_WIDTH;
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
                ? dsp[((size_t)(row / HVX_ROWS) * tokens + token) * HVX_ROWS + row % HVX_ROWS] * (double)unit * maxima[token] / 127
                : gpu[(size_t)token * rows + row];
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

static void reference_blocks(const float *exact, float *maxima, int8_t *quantized,
                             struct activation_block *packed, unsigned tokens, unsigned blocks) {
    for (unsigned t = 0; t < tokens; ++t) {
        float maximum = 0;
        for (unsigned k = 0; k < blocks * Q2_WIDTH; ++k)
            maximum = fmaxf(maximum, fabsf(exact[(size_t)t * blocks * Q2_WIDTH + k]));
        maxima[t] = maximum;
        const float multiplier = maximum > 0 ? 127.0f / maximum : 0.0f;
        for (unsigned b = 0; b < blocks; ++b) {
            const size_t index = (size_t)t * blocks + b;
            packed[index] = (struct activation_block){0};
            for (unsigned j = 0; j < Q2_WIDTH; ++j) {
                const size_t i = index * Q2_WIDTH + j;
                int q = (int)lroundf(exact[i] * multiplier);
                if (q < -127) q = -127;
                if (q > 127) q = 127;
                quantized[i] = (int8_t)q;
                packed[index].sum += q;
                packed[index].groups[j / 16][j % 4] |= (uint32_t)(uint8_t)q << (8 * (j % 16 / 4));
            }
        }
    }
}

static int verify_dynamic(const uint8_t *raw, const struct weight_tile *weights, const float *exact,
                          const int8_t *quantized, const int32_t *output, const float *maxima,
                          uint32_t rows, uint32_t blocks, uint32_t tokens, float unit) {
    const int all = getenv("N20_FULL_REFERENCE") != NULL;
    double error = 0, signal = 0;
    uint64_t checked = 0;
    for (uint32_t token = 0; token < tokens; ++token) {
        const uint32_t stride = all ? 1 : (tokens == 1 ? 1 : rows / 64 + 1);
        for (uint32_t row = 0; row < rows; row += stride) {
            int32_t expected = 0;
            double reference = 0;
            for (uint32_t b = 0; b < blocks; ++b) {
                const uint8_t *source = raw + ((size_t)row * blocks + b) * Q2_BYTES;
                const unsigned scale = weights[row / HVX_ROWS * blocks + b].scales[row % HVX_ROWS];
                const size_t start = ((size_t)token * blocks + b) * Q2_WIDTH;
                int32_t dot = 0;
                double exact_dot = 0;
                for (unsigned j = 0; j < Q2_WIDTH; ++j) {
                    const int weight = (int)code(source, j) - 1;
                    dot += weight * quantized[start + j];
                    exact_dot += weight * exact[start + j];
                }
                expected += dot * (int32_t)scale;
                reference += exact_dot * weight_scale(source);
            }
            const int32_t got = output[((size_t)row / HVX_ROWS * tokens + token) * HVX_ROWS + row % HVX_ROWS];
            if (got != expected) {
                fprintf(stderr, "Mismatch at token %u row %u: %" PRId32 " vs %" PRId32 "\n", token, row, got, expected);
                return 1;
            }
            const double difference = got * (double)unit * maxima[token] / 127 - reference;
            error += difference * difference; signal += reference * reference; ++checked;
        }
    }
    printf("checked=%" PRIu64 " full_reference=%d normalized_rms_error=%.6f\n", checked, all,
           signal ? sqrt(error / signal) : sqrt(error / checked));
    return 0;
}

static cl_program build_auxiliary(cl_context context, cl_device_id device) {
    const char *source =
        "#pragma OPENCL EXTENSION cl_khr_fp16 : enable\n"
        "kernel void reduce_max(global const half *x, global float *maxima, global float *inverse, global uint *guard,\n"
        "                       uint columns, uint padded) {\n"
        "  uint token=get_group_id(0), lane=get_local_id(0);\n"
        "  local float vals[64]; local uint bad[64]; float m=0.0f; uint invalid=0;\n"
        "  for (uint k=lane; k<columns; k+=64) {\n"
        "    float v=convert_float(x[k*padded+token]);\n"
        "    uint bits=as_uint(v)&0x7fffffffu; invalid |= bits > 0x41800000u;\n"
        "    if (bits < 0x7f800000u) m=fmax(m,fabs(v));\n"
        "  }\n"
        "  vals[lane]=m; bad[lane]=invalid; barrier(CLK_LOCAL_MEM_FENCE);\n"
        "  for (uint d=32; d; d>>=1) {\n"
        "    if (lane<d) { vals[lane]=fmax(vals[lane],vals[lane+d]); bad[lane]|=bad[lane+d]; }\n"
        "    barrier(CLK_LOCAL_MEM_FENCE);\n"
        "  }\n"
        "  if (!lane) { maxima[token]=vals[0]; inverse[token]=vals[0]>0.0f ? 127.0f/vals[0] : 0.0f;\n"
        "    if (bad[0]) atomic_or((volatile global uint *)guard,1u); }\n"
        "}\n"
        "kernel void reduce_max_token8(global const half *x, global float *maxima, global float *inverse, global uint *guard,\n"
        "                              uint columns, uint padded, uint tokens) {\n"
        "  uint lane=get_local_id(0), token=get_group_id(0)*8+lane%8, column_lane=lane/8;\n"
        "  local float vals[64]; local uint bad[64]; float m=0.0f; uint invalid=0;\n"
        "  if (token<tokens) for (uint k=column_lane; k<columns; k+=8) {\n"
        "    float v=convert_float(x[k*padded+token]); uint bits=as_uint(v)&0x7fffffffu;\n"
        "    invalid |= bits>0x41800000u; if (bits<0x7f800000u) m=fmax(m,fabs(v));\n"
        "  }\n"
        "  vals[lane]=m; bad[lane]=invalid; barrier(CLK_LOCAL_MEM_FENCE);\n"
        "  for (uint d=4; d; d>>=1) {\n"
        "    if (column_lane<d) { vals[lane]=fmax(vals[lane],vals[lane+d*8]); bad[lane]|=bad[lane+d*8]; }\n"
        "    barrier(CLK_LOCAL_MEM_FENCE);\n"
        "  }\n"
        "  if (!column_lane && token<tokens) { maxima[token]=vals[lane];\n"
        "    inverse[token]=vals[lane]>0.0f ? 127.0f/vals[lane] : 0.0f;\n"
        "    if (bad[lane]) atomic_or((volatile global uint *)guard,1u); }\n"
        "}\n"
        "kernel void pack(global const half *x, global uint *out, global const float *inverse, uint blocks, uint padded, uint count, uint tokens, uint token_fast) {\n"
        "  uint id = get_global_id(0); if (id >= count) return;\n"
        "  uint token = token_fast ? id % tokens : id / blocks;\n"
        "  uint block = token_fast ? id / tokens : id % blocks;\n"
        "  uint output_id=token*blocks+block; uint groups[16] = {0}; int sum = 0;\n"
        "  float multiplier=inverse[token];\n"
        "  for (uint j=0; j<64; ++j) {\n"
        "    float v = convert_float(x[(block*64+j)*padded+token]);\n"
        "    uint bits=as_uint(v)&0x7fffffffu;\n"
        "    int q = bits < 0x7f800000u ? clamp(convert_int_rtz(round(v * multiplier)), -127, 127) : 0; sum += q;\n"
        "    groups[(j/16)*4+j%4] |= ((uint)(uchar)q) << (8*((j%16)/4));\n"
        "  }\n"
        "  for (uint i=0; i<16; ++i) out[output_id*17+i] = groups[i]; out[output_id*17+16] = as_uint(sum);\n"
        "}\n"
        "kernel void scatter(global const int *dsp, global float *out, global const float *maxima,\n"
        "                    uint rows, uint dsp_rows, uint tokens, float scale) {\n"
        "  uint i = get_global_id(0); if (i >= dsp_rows*tokens) return;\n"
        "  uint t = i / dsp_rows, row = i % dsp_rows;\n"
        "  out[t*rows+row] = convert_float(dsp[((row/32)*tokens+t)*32+row%32]) * scale * maxima[t];\n"
        "}\n";
    cl_int status;
    const char *parallel_path = getenv("N20_PARALLEL_PACK_SOURCE");
    size_t parallel_length = 0;
    char *parallel_source = parallel_path ? read_source(parallel_path, &parallel_length) : NULL;
    const char *sources[] = {source, parallel_source};
    const size_t lengths[] = {strlen(source), parallel_length};
    cl_program program = clCreateProgramWithSource(context, parallel_source ? 2 : 1, sources, lengths, &status);
    free(parallel_source);
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

#ifndef N20_PACK_ONLY
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
    const int inject_inf = guard_test && !strcmp(guard_test, "inf");
    const int inject_large = guard_test && !strcmp(guard_test, "large");
    const int inject_boundary = guard_test && !strcmp(guard_test, "boundary");
    const int edge = getenv("N20_EDGE_INPUT") != NULL;
    const int extreme = getenv("N20_EXTREME_INPUT") != NULL;
    const int alternate = getenv("N20_ALTERNATE_INPUT") != NULL;
    const int reduce_token8 = getenv("N20_REDUCE_TOKEN8") != NULL;
    if (!rows || rows % HVX_ROWS || !columns || columns % Q2_WIDTH || !tokens ||
        !dsp_rows || dsp_rows % HVX_ROWS || dsp_rows >= rows ||
        (guard_test && !inject_range && !inject_nan && !inject_inf && !inject_large && !inject_boundary) || (edge && guard_test) ||
        (alternate && (inject_nan || inject_inf || inject_large || extreme)) ||
        (extreme && (edge || guard_test || tokens < 3))) {
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
    float *maxima = allocate((size_t)tokens * sizeof(*maxima));
    int8_t *quantized = allocate((size_t)tokens * columns);
    struct activation_block *expected_input = allocate(input_bytes);
    make_activations(exact, quantized, expected_input, tokens * blocks);
    for (unsigned t = 0; t < tokens; ++t) {
        static const float factors[] = {0.0f, 1.0f, 2.0f, 4.0f, 8.0f, 0.25f};
        for (unsigned k = 0; k < columns; ++k) {
            const size_t i = (size_t)t * columns + k;
            exact[i] = (float)(_Float16)(exact[i] * factors[t % (sizeof(factors) / sizeof(factors[0]))]);
        }
    }
    if (edge) {
        static const float values[] = {-8.0f, 8.0f, -4.0f, 4.0f, -1.0f, 1.0f, -0.0f, 0.0f};
        for (size_t i = 0; i < (size_t)tokens * columns; ++i) exact[i] = values[i % (sizeof(values) / sizeof(values[0]))];
    }
    if (extreme) {
        for (unsigned t = 0; t < tokens; ++t) for (unsigned k = 0; k < columns; ++k) {
            const float magnitude = t % 3 == 0 ? 0.0f : (t % 3 == 1 ? 0x1p-24f : 65504.0f);
            exact[(size_t)t * columns + k] = k % 2 ? magnitude : -magnitude;
        }
    }
    if (inject_range) exact[0] = 1.5f;
    if (inject_nan) exact[0] = NAN;
    if (inject_inf) exact[0] = INFINITY;
    if (inject_large) exact[0] = 16.015625f; /* Next FP16 value above the empirical limit. */
    if (inject_boundary) exact[0] = 16.0f;
    if (!inject_nan && !inject_inf) reference_blocks(exact, maxima, quantized, expected_input, tokens, blocks);
    _Float16 *input = allocate((size_t)columns * padded * sizeof(*input));
    for (unsigned column = 0; column < columns; ++column)
        for (unsigned token = 0; token < padded; ++token)
            input[(size_t)column * padded + token] = token < tokens ? (_Float16)exact[(size_t)token * columns + column] : 0;
    float *exact_alt = NULL, *maxima_alt = NULL;
    int8_t *quantized_alt = NULL;
    struct activation_block *expected_alt = NULL;
    _Float16 *input_alt = NULL;
    if (alternate) {
        exact_alt = allocate((size_t)tokens * columns * sizeof(*exact_alt));
        maxima_alt = allocate((size_t)tokens * sizeof(*maxima_alt));
        quantized_alt = allocate((size_t)tokens * columns);
        expected_alt = allocate(input_bytes);
        input_alt = allocate((size_t)columns * padded * sizeof(*input_alt));
        for (size_t i = 0; i < (size_t)tokens * columns; ++i) exact_alt[i] = (float)(_Float16)(-2.0f * exact[i]);
        reference_blocks(exact_alt, maxima_alt, quantized_alt, expected_alt, tokens, blocks);
        for (unsigned column = 0; column < columns; ++column)
            for (unsigned token = 0; token < padded; ++token)
                input_alt[(size_t)column * padded + token] = token < tokens ? (_Float16)exact_alt[(size_t)token * columns + column] : 0;
    }

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
    struct gpu_projection full = make_gpu_projection(context, program, image, raw, 0, rows, blocks, tokens, padded, NULL, NULL);
    cl_mem combined = clCreateBuffer(context, CL_MEM_READ_WRITE, full_bytes, NULL, &cl_status);
    cl_check(cl_status, "clCreateBuffer(combined)");
    struct gpu_projection shard = make_gpu_projection(context, program, image, raw, dsp_rows, gpu_rows, blocks, tokens, padded, &full, combined);
    cl_mem scale_buffer = clCreateBuffer(context, CL_MEM_READ_WRITE, (size_t)tokens * sizeof(float), NULL, &cl_status);
    cl_check(cl_status, "clCreateBuffer(scales)");
    cl_mem inverse_buffer = clCreateBuffer(context, CL_MEM_READ_WRITE, (size_t)tokens * sizeof(float), NULL, &cl_status);
    cl_check(cl_status, "clCreateBuffer(inverse)");
    cl_mem guard_buffer = clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(cl_uint), NULL, &cl_status);
    cl_check(cl_status, "clCreateBuffer(guard)");
    cl_program auxiliary = build_auxiliary(context, device);
    cl_kernel reduce = clCreateKernel(auxiliary, reduce_token8 ? "reduce_max_token8" : "reduce_max", &cl_status);
    cl_check(cl_status, "clCreateKernel(reduce_max)");
    const int parallel_pack = getenv("N20_PARALLEL_PACK_SOURCE") != NULL;
    cl_kernel pack = clCreateKernel(auxiliary, parallel_pack ? "parallel_pack" : "pack", &cl_status);
    cl_check(cl_status, "clCreateKernel(pack)");
    cl_kernel scatter = clCreateKernel(auxiliary, "scatter", &cl_status);
    cl_check(cl_status, "clCreateKernel(scatter)");
    const cl_uint packed_count = tokens * blocks, padded_arg = padded, blocks_arg = blocks;
    const cl_uint rows_arg = rows, dsp_rows_arg = dsp_rows, tokens_arg = tokens;
    const cl_uint columns_arg = columns;
    const cl_uint token_fast = getenv("N20_TOKEN_FAST") != NULL;
    const float scale = unit / 127.0f;
    #define PARG(kernel, i, value) CL(clSetKernelArg((kernel), (i), sizeof(value), &(value)))
    PARG(reduce, 0, x); PARG(reduce, 1, scale_buffer); PARG(reduce, 2, inverse_buffer); PARG(reduce, 3, guard_buffer);
    PARG(reduce, 4, columns_arg); PARG(reduce, 5, padded_arg);
    if (reduce_token8) PARG(reduce, 6, tokens_arg);
    PARG(pack, 0, x); PARG(pack, 1, packed); PARG(pack, 2, inverse_buffer); PARG(pack, 3, blocks_arg);
    PARG(pack, 4, padded_arg);
    if (parallel_pack) {
        PARG(pack, 5, tokens_arg);
    } else {
        PARG(pack, 5, packed_count); PARG(pack, 6, tokens_arg); PARG(pack, 7, token_fast);
    }
    PARG(scatter, 0, dsp_output); PARG(scatter, 1, combined); PARG(scatter, 2, scale_buffer);
    PARG(scatter, 3, rows_arg); PARG(scatter, 4, dsp_rows_arg); PARG(scatter, 5, tokens_arg); PARG(scatter, 6, scale);
    #undef PARG
    printf("rows=%u columns=%u tokens=%u dsp_rows=%u gpu_rows=%u page=%u padding=%u guard_test=%s edge=%d extreme=%d alternate=%d token_fast=%u reduce_token8=%d empirical_abs_limit=16\n",
           rows, columns, tokens, dsp_rows, gpu_rows, page, padding, guard_test ? guard_test : "none", edge, extreme, alternate, token_fast, reduce_token8);
    baseline(queue, &full, x, image, padded, columns, "baseline-before");
    float *full_before = allocate(full_bytes), *final_output = allocate(full_bytes);
    CL(clEnqueueReadBuffer(queue, full.output, CL_TRUE, 0, full_bytes, full_before, 0, NULL, NULL));
    int fallback_count = 0;
    for (int sample = -1; sample < SAMPLES; ++sample) {
        const int use_alt = alternate && ((sample + 2) % 2);
        if (alternate)
            CL(clEnqueueWriteBuffer(queue, x, CL_TRUE, 0, (size_t)columns * padded * sizeof(*input),
                                    use_alt ? input_alt : input, 0, NULL, NULL));
        const cl_uint zero = 0;
        cl_uint guard = 0;
        cl_event reduce_event, pack_event, guard_event, copy_event, gpu_event, finish_event;
        const size_t count = packed_count, total = (size_t)dsp_rows * tokens;
        const size_t reduce_global = (size_t)(reduce_token8 ? (tokens + 7) / 8 : tokens) * 64, reduce_local = 64;
        struct projection_timing timing = {0};
        struct projection_request request = {tokens};
        union rpc_arg args[] = {{.buffer = {&request, sizeof(request)}},
                                {.buffer = {qin, input_bytes}},
                                {.buffer = {&timing, sizeof(timing)}},
                                {.buffer = {qout, dsp_bytes}}};
        const uint64_t start = now_us();
        CL(clEnqueueFillBuffer(queue, guard_buffer, &zero, sizeof(zero), 0, sizeof(zero), 0, NULL, NULL));
        CL(clEnqueueNDRangeKernel(queue, reduce, 1, NULL, &reduce_global, &reduce_local, 0, NULL, &reduce_event));
        if (parallel_pack) {
            const size_t local = 128, global = (size_t)((tokens + 7) / 8) * blocks * local;
            CL(clEnqueueNDRangeKernel(queue, pack, 1, NULL, &global, &local, 0, NULL, &pack_event));
        } else {
            CL(clEnqueueNDRangeKernel(queue, pack, 1, NULL, &count, NULL, 0, NULL, &pack_event));
        }
        CL(clEnqueueReadBuffer(queue, guard_buffer, CL_FALSE, 0, sizeof(guard), &guard, 0, NULL, &guard_event));
        // Flush and finish the guard before submitting the independent GPU
        // shard. Some drivers wait for later submitted commands as well.
        CL(clFlush(queue));
        CL(clWaitForEvents(1, &guard_event));
        const uint64_t ready = now_us();
        const int fallback = guard != 0;
        if (fallback != (inject_nan || inject_inf || inject_large || extreme)) {
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
            printf("arm=shared sample=%d wall_ms=%.3f reduce_ms=%.3f pack_ms=%.3f ready_ms=%.3f gpu_copy_kernel_ms=%.3f "
                   "rpc_enqueue_ms=%.3f dsp_kernel_ms=%.3f finish_ms=%.3f fallback=%d\n",
                   sample, (end - start) / 1000.0, event_ms(reduce_event, reduce_event), event_ms(pack_event, pack_event), (ready - start) / 1000.0,
                   event_ms(copy_event, gpu_event), (rpc_end - ready) / 1000.0,
                   fallback ? 0.0 : timing.elapsed_us / 1000.0, (end - rpc_end) / 1000.0, fallback);
            fflush(stdout);
        }
        CL(clReleaseEvent(reduce_event)); CL(clReleaseEvent(pack_event)); CL(clReleaseEvent(guard_event));
        CL(clReleaseEvent(copy_event)); CL(clReleaseEvent(gpu_event)); CL(clReleaseEvent(finish_event));
        if (alternate) {
            const float *active_maxima = use_alt ? maxima_alt : maxima;
            const float *active_exact = use_alt ? exact_alt : exact;
            const int8_t *active_quantized = use_alt ? quantized_alt : quantized;
            const struct activation_block *active_packed = use_alt ? expected_alt : expected_input;
            float *observed_maxima = allocate((size_t)tokens * sizeof(*observed_maxima));
            struct activation_block *observed_packed = allocate(input_bytes);
            int32_t *observed_dsp = allocate(dsp_bytes);
            CL(clEnqueueReadBuffer(queue, scale_buffer, CL_TRUE, 0, (size_t)tokens * sizeof(*observed_maxima), observed_maxima, 0, NULL, NULL));
            CL(clEnqueueReadBuffer(queue, packed, CL_TRUE, 0, input_bytes, observed_packed, 0, NULL, NULL));
            CL(clEnqueueReadBuffer(queue, dsp_output, CL_TRUE, 0, dsp_bytes, observed_dsp, 0, NULL, NULL));
            CL(clEnqueueReadBuffer(queue, combined, CL_TRUE, 0, full_bytes, final_output, 0, NULL, NULL));
            if (memcmp(active_maxima, observed_maxima, (size_t)tokens * sizeof(*active_maxima)) ||
                memcmp(active_packed, observed_packed, input_bytes) || memcmp(active_packed, qin, input_bytes) ||
                memcmp(observed_dsp, qout, dsp_bytes)) {
                fputs("Alternate-input shared visibility mismatch\n", stderr); return 1;
            }
            if (verify_dynamic(raw, weights, active_exact, active_quantized, observed_dsp, active_maxima,
                               dsp_rows, blocks, tokens, unit)) return 1;
            for (unsigned t = 0; t < tokens; ++t) for (unsigned r = 0; r < dsp_rows; ++r) {
                const int32_t q = observed_dsp[((size_t)(r / HVX_ROWS) * tokens + t) * HVX_ROWS + r % HVX_ROWS];
                const float expected = (float)((double)q * unit * active_maxima[t] / 127.0);
                const float got = final_output[(size_t)t * rows + r];
                if (!isfinite(got) || fabsf(got - expected) > 0.000002f * fmaxf(1.0f, fabsf(expected))) {
                    fputs("Alternate-input scatter mismatch\n", stderr); return 1;
                }
            }
            cl_event copy_check, gpu_check;
            queue_projection(queue, &full, x, image, padded, columns, &copy_check, &gpu_check);
            CL(clEnqueueReadBuffer(queue, full.output, CL_TRUE, 0, full_bytes, full_before, 0, NULL, NULL));
            for (unsigned t = 0; t < tokens; ++t) for (unsigned r = dsp_rows; r < rows; ++r) {
                const size_t i = (size_t)t * rows + r;
                if (memcmp(full_before + i, final_output + i, sizeof(float))) {
                    fputs("Alternate-input GPU shard mismatch\n", stderr); return 1;
                }
            }
            CL(clReleaseEvent(copy_check)); CL(clReleaseEvent(gpu_check));
            free(observed_dsp); free(observed_packed); free(observed_maxima);
            printf("alternate_check sample=%d variant=%d passed=1\n", sample, use_alt);
        }
    }
    CL(clEnqueueReadBuffer(queue, combined, CL_TRUE, 0, full_bytes, final_output, 0, NULL, NULL));
    if (inject_nan || inject_inf || inject_large || extreme) {
        unsigned nan_count = 0, inf_count = 0;
        for (size_t i = 0; i < (size_t)tokens * rows; ++i) {
            if (isnan(full_before[i])) {
                ++nan_count;
                if (!isnan(final_output[i])) { fputs("Fallback NaN mismatch\n", stderr); return 1; }
            } else if (memcmp(&full_before[i], &final_output[i], sizeof(float))) {
                fputs("Fallback output mismatch\n", stderr); return 1;
            } else if (isinf(full_before[i])) ++inf_count;
        }
        if (fallback_count != SAMPLES + 1) { fputs("Guard failed to trigger every time\n", stderr); return 1; }
        printf("guard_fallbacks=%d non_nan_values_bitwise_match=1 baseline_nan_count=%u baseline_inf_count=%u nan_masks_match=1\n",
               fallback_count, nan_count, inf_count);
    } else {
        struct activation_block *observed_input = allocate(input_bytes);
        int32_t *observed_dsp = allocate(dsp_bytes);
        float *observed_maxima = allocate((size_t)tokens * sizeof(*observed_maxima));
        CL(clEnqueueReadBuffer(queue, scale_buffer, CL_TRUE, 0, (size_t)tokens * sizeof(*observed_maxima), observed_maxima, 0, NULL, NULL));
        if (memcmp(maxima, observed_maxima, (size_t)tokens * sizeof(*maxima))) {
            fputs("GPU token maxima differ from CPU reference\n", stderr); return 1;
        }
        CL(clEnqueueReadBuffer(queue, packed, CL_TRUE, 0, input_bytes, observed_input, 0, NULL, NULL));
        CL(clEnqueueReadBuffer(queue, dsp_output, CL_TRUE, 0, dsp_bytes, observed_dsp, 0, NULL, NULL));
        if (memcmp(expected_input, observed_input, input_bytes) || memcmp(expected_input, qin, input_bytes)) {
            fputs("GPU-packed input differs from CPU reference or host-visible ION\n", stderr); return 1;
        }
        if (memcmp(observed_dsp, qout, dsp_bytes)) { fputs("GPU and host disagree on DSP output\n", stderr); return 1; }
        if (verify_dynamic(raw, weights, exact, quantized, observed_dsp, maxima, dsp_rows, blocks, tokens, unit)) return 1;
        for (unsigned t = 0; t < tokens; ++t) {
            for (unsigned r = 0; r < rows; ++r) {
                const size_t i = (size_t)t * rows + r;
                if (r >= dsp_rows) {
                    if (memcmp(&full_before[i], &final_output[i], sizeof(float))) {
                        fprintf(stderr, "GPU/scatter mismatch token=%u row=%u\n", t, r); return 1;
                    }
                } else {
                    const int32_t q = observed_dsp[((size_t)(r / HVX_ROWS) * tokens + t) * HVX_ROWS + r % HVX_ROWS];
                    const float expected = (float)((double)q * unit * maxima[t] / 127.0);
                    if (!isfinite(final_output[i]) || fabsf(final_output[i] - expected) > 0.000002f * fmaxf(1.0f, fabsf(expected))) {
                        fprintf(stderr, "DSP scatter mismatch token=%u row=%u\n", t, r); return 1;
                    }
                }
            }
        }
        puts("gpu_pack_full_byte_match=1 ion_visibility_match=1 gpu_shard_full_bitwise_match=1 dsp_scatter_match=1");
        check_sampled(raw, exact, full_before, final_output, observed_dsp, maxima, unit, rows, dsp_rows, blocks, tokens);
        free(observed_maxima); free(observed_dsp); free(observed_input);
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
    CL(clReleaseKernel(scatter)); CL(clReleaseKernel(pack)); CL(clReleaseKernel(reduce)); CL(clReleaseProgram(auxiliary));
    CL(clReleaseMemObject(guard_buffer)); CL(clReleaseMemObject(inverse_buffer)); CL(clReleaseMemObject(scale_buffer));
    CL(clReleaseMemObject(combined)); release_gpu_projection(&shard); release_gpu_projection(&full);
    CL(clReleaseMemObject(image)); CL(clReleaseMemObject(x)); CL(clReleaseProgram(program));
    CL(clReleaseMemObject(dsp_output)); CL(clReleaseMemObject(packed));
    CL(clReleaseCommandQueue(queue)); CL(clReleaseContext(context));
    rpc_free(qout); rpc_free(qin); free(input_alt); free(expected_alt); free(quantized_alt); free(maxima_alt); free(exact_alt);
    free(input); free(maxima); free(expected_input); free(quantized); free(exact); free(weights); free(raw);
    if (close_rpc(handle)) { fputs("FastRPC close failed\n", stderr); return 1; }
    dlclose(library);
    return 0;
}
#endif
