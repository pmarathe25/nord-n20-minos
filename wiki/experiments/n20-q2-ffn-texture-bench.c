/* Compare two Q2_0 projections plus SwiGLU with one fused texture kernel. */
#define CL_TARGET_OPENCL_VERSION 200
#include <CL/cl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { BLOCK_COLUMNS = 64, BLOCK_BYTES = 18, RUNS = 5 };

static void check(cl_int status, const char *call) {
    if (status != CL_SUCCESS) {
        fprintf(stderr, "%s failed (%d)\n", call, status);
        exit(1);
    }
}

#define CL(call) check((call), #call)

static void *alloc(size_t size) {
    void *result = malloc(size);
    if (!result) {
        perror("malloc");
        exit(1);
    }
    return result;
}

static unsigned char *read_file(const char *path, size_t expected, size_t *length) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        perror(path);
        exit(1);
    }
    if (fseek(file, 0, SEEK_END) || ftell(file) < 0) {
        perror(path);
        exit(1);
    }
    const size_t size = ftell(file);
    rewind(file);
    unsigned char *data = alloc(size);
    if ((expected && size != expected) || fread(data, 1, size, file) != size) {
        fprintf(stderr, "Unexpected file length for %s\n", path);
        exit(1);
    }
    fclose(file);
    if (length)
        *length = size;
    return data;
}

static int dimension(const char *value, int multiple, long maximum) {
    char *end;
    const long result = strtol(value, &end, 10);
    return *value && !*end && result > 0 && result <= maximum && result % multiple == 0
        ? (int)result : 0;
}

static cl_mem upload(cl_context context, void *data, size_t size) {
    cl_int status;
    cl_mem buffer = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                   size, data, &status);
    check(status, "clCreateBuffer(input)");
    return buffer;
}

static float weight_scale(const unsigned char *block) {
    _Float16 scale;
    memcpy(&scale, block, sizeof(scale));
    return scale;
}

static double verify(const unsigned char *raw, const unsigned char *raw_up, const _Float16 *input, const float *output,
                     int rows, int columns, int tokens, int padded) {
    const int blocks = columns / BLOCK_COLUMNS;
    const int stride = rows <= 1024 ? 1 : 97;
    double error = 0, signal = 0, maximum = 0;
    unsigned count = 0;
    for (int token = 0; token < tokens; token += tokens > 16 ? 7 : 1) {
        for (int row = 0; row < rows; row += stride) {
            double expected = 0, up = 0;
            for (int b = 0; b < blocks; ++b) {
                const unsigned char *block = raw + ((size_t)row * blocks + b) * BLOCK_BYTES;
                const unsigned char *other = raw_up + ((size_t)row * blocks + b) * BLOCK_BYTES;
                const float scale = weight_scale(block);
                for (int k = 0; k < BLOCK_COLUMNS; ++k) {
                    const int weight = ((block[2 + k / 4] >> (2 * (k % 4))) & 3) - 1;
                    expected += (double)scale * weight * input[(b * BLOCK_COLUMNS + k) * padded + token];
                    const int uw = ((other[2 + k / 4] >> (2 * (k % 4))) & 3) - 1;
                    up += (double)weight_scale(other) * uw * input[(b * BLOCK_COLUMNS + k) * padded + token];
                }
            }
            expected = expected / (1.0 + exp(-expected)) * up;
            const double difference = output[(size_t)token * rows + row] - expected;
            if (!isfinite(difference)) {
                fprintf(stderr, "Non-finite output at row %d token %d\n", row, token);
                exit(1);
            }
            error += difference * difference;
            signal += expected * expected;
            maximum = fmax(maximum, fabs(difference));
            ++count;
        }
    }
    const double nrmse = sqrt(error / fmax(signal, 1e-30));
    printf("checked=%u nrmse=%.7g max_error=%.7g\n", count, nrmse, maximum);
    return nrmse;
}

int main(int argc, char **argv) {
    const int rows = argc >= 6 ? dimension(argv[3], 4, 262144) : 0;
    const int columns = argc >= 6 ? dimension(argv[4], BLOCK_COLUMNS, 16384) : 0;
    const int tokens = argc >= 6 ? dimension(argv[5], 1, 256) : 0;
    const char *up_path = getenv("N20_FFN_UP_WEIGHTS");
    const char *fused_path = getenv("N20_FFN_SOURCE");
    const int ffn_rows = getenv("N20_FFN_ROWS") ? atoi(getenv("N20_FFN_ROWS")) : 2;
    const int ffn_tokens = getenv("N20_FFN_TOKENS") ? atoi(getenv("N20_FFN_TOKENS")) : 8;
    if (argc != 6 || !rows || !columns || !tokens || !up_path || !fused_path ||
        (ffn_rows != 1 && ffn_rows != 2 && ffn_rows != 4) ||
        (ffn_tokens != 4 && ffn_tokens != 8) || rows % ffn_rows) {
        fprintf(stderr,
                "Usage: %s BASE_KERNEL GATE_WEIGHTS ROWS COLUMNS TOKENS\n",
                argv[0]);
        return 2;
    }
    const int blocks = columns / BLOCK_COLUMNS;
    // The backend keeps eight-token input padding when it uses the four-token tail.
    const int padding_tile = 8;
    const int padded = (tokens + padding_tile - 1) / padding_tile * padding_tile;
    const size_t weights = (size_t)rows * blocks;
    const size_t output_size = (size_t)rows * tokens * sizeof(float);
    const size_t input_size = (size_t)columns * padded * sizeof(_Float16);
    unsigned char *raw = read_file(argv[2], weights * BLOCK_BYTES, NULL);
    unsigned char *raw_up = read_file(up_path, weights * BLOCK_BYTES, NULL);
    unsigned char *codes = alloc(weights * 16);
    unsigned char *codes_up = alloc(weights * 16);
    uint16_t *scales = alloc(weights * sizeof(*scales));
    uint16_t *scales_up = alloc(weights * sizeof(*scales_up));
    _Float16 *input = alloc(input_size);
    float *output = alloc(output_size);
    for (int row = 0; row < rows; ++row) {
        for (int b = 0; b < blocks; ++b) {
            const unsigned char *block = raw + ((size_t)row * blocks + b) * BLOCK_BYTES;
            const unsigned char *other = raw_up + ((size_t)row * blocks + b) * BLOCK_BYTES;
            memcpy(scales + b * (size_t)rows + row, block, sizeof(*scales));
            memcpy(scales_up + b * (size_t)rows + row, other, sizeof(*scales_up));
            for (int i = 0; i < 16; ++i) {
                codes[(b * 16 + i) * (size_t)rows + row] = block[2 + i];
                codes_up[(b * 16 + i) * (size_t)rows + row] = other[2 + i];
            }
        }
    }
    uint32_t rng = 621;
    for (int k = 0; k < columns; ++k) {
        for (int token = 0; token < padded; ++token) {
            rng = 1664525u * rng + 1013904223u;
            input[k * padded + token] = token < tokens
                ? (float)((int)(rng >> 16) - 32768) / 16384 : 0;
        }
    }

    cl_platform_id platform;
    cl_device_id device;
    CL(clGetPlatformIDs(1, &platform, NULL));
    CL(clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &device, NULL));
    cl_int status;
    cl_context context = clCreateContext(NULL, 1, &device, NULL, NULL, &status);
    check(status, "clCreateContext");
    const cl_queue_properties properties[] = {CL_QUEUE_PROPERTIES, CL_QUEUE_PROFILING_ENABLE, 0};
    cl_command_queue queue = clCreateCommandQueueWithProperties(context, device, properties, &status);
    check(status, "clCreateCommandQueueWithProperties");

    size_t length;
    const char *source = (char *)read_file(argv[1], 0, &length);
    cl_program program = clCreateProgramWithSource(context, 1, &source, &length, &status);
    check(status, "clCreateProgramWithSource");
    char options[512];
    const int option_length = snprintf(options, sizeof(options),
                                       "-cl-fast-relaxed-math -DN20_FFN_ROWS=%d -DN20_FFN_TOKENS=%d",
                                       ffn_rows, ffn_tokens);
    if (option_length < 0 || (size_t)option_length >= sizeof(options))
        return 2;
    status = clBuildProgram(program, 1, &device, options, NULL, NULL);
    if (status != CL_SUCCESS) {
        size_t size;
        CL(clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, 0, NULL, &size));
        char *log = alloc(size + 1);
        CL(clGetProgramBuildInfo(program, device, CL_PROGRAM_BUILD_LOG, size, log, NULL));
        log[size] = '\0';
        fprintf(stderr, "%s\n", log);
        free(log);
        check(status, "clBuildProgram");
    }
    free((void *)source);
    cl_kernel kernel = clCreateKernel(program, "kernel_gemm_noshuffle_q2_0_f32", &status);
    check(status, "clCreateKernel");
    source = (char *)read_file(fused_path, 0, &length);
    cl_program fused_program = clCreateProgramWithSource(context, 1, &source, &length, &status);
    check(status, "clCreateProgramWithSource(fused)");
    status = clBuildProgram(fused_program, 1, &device, options, NULL, NULL);
    if (status != CL_SUCCESS) {
        size_t size;
        CL(clGetProgramBuildInfo(fused_program, device, CL_PROGRAM_BUILD_LOG, 0, NULL, &size));
        char *log = alloc(size + 1);
        CL(clGetProgramBuildInfo(fused_program, device, CL_PROGRAM_BUILD_LOG, size, log, NULL));
        log[size] = 0;
        fprintf(stderr, "%s\n", log);
        check(status, "clBuildProgram(fused)");
    }
    free((void *)source);
    cl_kernel fused = clCreateKernel(fused_program, "n20_q2_ffn_texture", &status);
    check(status, "clCreateKernel(fused)");
    cl_kernel combine = clCreateKernel(fused_program, "n20_swiglu", &status);
    check(status, "clCreateKernel(combine)");
    cl_kernel combine_merged = clCreateKernel(fused_program, "n20_swiglu_merged", &status);
    check(status, "clCreateKernel(combine_merged)");
    cl_mem q = upload(context, codes, weights * 16);
    cl_mem d = upload(context, scales, weights * sizeof(*scales));
    cl_mem uq = upload(context, codes_up, weights * 16);
    cl_mem ud = upload(context, scales_up, weights * sizeof(*scales_up));
    unsigned char *codes_both = alloc(weights * 32);
    uint16_t *scales_both = alloc(weights * 2 * sizeof(*scales_both));
    for (int b = 0; b < blocks; ++b) {
        memcpy(scales_both + (size_t)b * 2 * rows, scales + (size_t)b * rows, rows * sizeof(*scales));
        memcpy(scales_both + (size_t)b * 2 * rows + rows, scales_up + (size_t)b * rows, rows * sizeof(*scales_up));
        for (int i = 0; i < 16; ++i) {
            const size_t old = (size_t)(b * 16 + i) * rows;
            const size_t next = old * 2;
            memcpy(codes_both + next, codes + old, rows);
            memcpy(codes_both + next + rows, codes_up + old, rows);
        }
    }
    cl_mem bq = upload(context, codes_both, weights * 32);
    cl_mem bd = upload(context, scales_both, weights * 2 * sizeof(*scales_both));
    cl_mem x = upload(context, input, input_size);
    const cl_image_format format = {CL_RGBA, CL_HALF_FLOAT};
    const cl_image_desc image = {.image_type = CL_MEM_OBJECT_IMAGE2D,
        .image_width = (size_t)padded / 4, .image_height = (size_t)columns};
    cl_mem x_image = clCreateImage(context, CL_MEM_READ_ONLY, &format, &image, NULL, &status);
    check(status, "clCreateImage");
    cl_mem y = clCreateBuffer(context, CL_MEM_READ_WRITE, output_size, NULL, &status);
    check(status, "clCreateBuffer(output)");
    cl_mem g = clCreateBuffer(context, CL_MEM_READ_WRITE, output_size, NULL, &status);
    check(status, "clCreateBuffer(gate)");
    cl_mem u = clCreateBuffer(context, CL_MEM_READ_WRITE, output_size, NULL, &status);
    check(status, "clCreateBuffer(up)");
    cl_mem both = clCreateBuffer(context, CL_MEM_READ_WRITE, output_size * 2, NULL, &status);
    check(status, "clCreateBuffer(both)");

    const cl_ulong offset = 0;
    #define ARG(k, index, value) CL(clSetKernelArg(k, index, sizeof(value), &(value)))
    ARG(kernel, 2, x_image); ARG(kernel, 4, offset); ARG(kernel, 5, rows);
    ARG(kernel, 6, padded); ARG(kernel, 7, columns); ARG(kernel, 8, tokens);
    ARG(fused, 0, q); ARG(fused, 1, d); ARG(fused, 2, uq); ARG(fused, 3, ud);
    ARG(fused, 4, x_image); ARG(fused, 5, y); ARG(fused, 6, rows);
    ARG(fused, 7, padded); ARG(fused, 8, columns); ARG(fused, 9, tokens);
    const cl_uint count = (cl_uint)((size_t)rows * tokens);
    const cl_uint row_count = (cl_uint)rows;
    const int merged_rows = 2 * rows;
    ARG(combine, 0, g); ARG(combine, 1, u); ARG(combine, 2, y); ARG(combine, 3, count);
    ARG(combine_merged, 0, both); ARG(combine_merged, 1, y);
    ARG(combine_merged, 2, row_count); ARG(combine_merged, 3, count);
    #undef ARG
    const size_t local[] = {1, 128};
    const size_t global[] = {(size_t)(tokens + 7) / 8, (size_t)((rows + 511) / 512) * 128};
    const size_t fg[] = {(size_t)(tokens + ffn_tokens - 1) / ffn_tokens,
        (size_t)((rows + ffn_rows * 128 - 1) / (ffn_rows * 128)) * 128};
    const size_t mg[] = {(size_t)(tokens + 7) / 8, (size_t)((merged_rows + 511) / 512) * 128};
    const size_t cg[] = {(size_t)count};
    const size_t cmg[] = {(size_t)rows, (size_t)tokens};
    const size_t origin[] = {0, 0, 0}, region[] = {(size_t)padded / 4, (size_t)columns, 1};
    float *baseline = alloc(output_size);
    double max_diff = 0, square_diff = 0, square_base = 0;
    for (int mode = 0; mode < 4; ++mode) {
        printf("mode=%s rows=%d tokens=%d rows_per_item=%d token_tile=%d copy_plus_gpu_ms=",
               mode == 1 ? "fused" : mode == 2 ? "merged" : "baseline", rows, tokens, ffn_rows, ffn_tokens);
        for (int run = 0; run < RUNS; ++run) {
            cl_event copy, end_event;
            CL(clEnqueueCopyBufferToImage(queue, x, x_image, 0, origin, region, 0, NULL, &copy));
            if (mode == 1) {
                CL(clEnqueueNDRangeKernel(queue, fused, 2, NULL, fg, local, 0, NULL, &end_event));
            } else if (mode == 2) {
                CL(clSetKernelArg(kernel, 0, sizeof(bq), &bq));
                CL(clSetKernelArg(kernel, 1, sizeof(bd), &bd));
                CL(clSetKernelArg(kernel, 3, sizeof(both), &both));
                CL(clSetKernelArg(kernel, 5, sizeof(merged_rows), &merged_rows));
                CL(clEnqueueNDRangeKernel(queue, kernel, 2, NULL, mg, local, 0, NULL, NULL));
                CL(clEnqueueNDRangeKernel(queue, combine_merged, 2, NULL, cmg, NULL, 0, NULL, &end_event));
            } else {
                CL(clSetKernelArg(kernel, 5, sizeof(rows), &rows));
                CL(clSetKernelArg(kernel, 0, sizeof(q), &q));
                CL(clSetKernelArg(kernel, 1, sizeof(d), &d));
                CL(clSetKernelArg(kernel, 3, sizeof(g), &g));
                CL(clEnqueueNDRangeKernel(queue, kernel, 2, NULL, global, local, 0, NULL, NULL));
                CL(clSetKernelArg(kernel, 0, sizeof(uq), &uq));
                CL(clSetKernelArg(kernel, 1, sizeof(ud), &ud));
                CL(clSetKernelArg(kernel, 3, sizeof(u), &u));
                CL(clEnqueueNDRangeKernel(queue, kernel, 2, NULL, global, local, 0, NULL, NULL));
                CL(clEnqueueNDRangeKernel(queue, combine, 1, NULL, cg, NULL, 0, NULL, &end_event));
            }
            CL(clWaitForEvents(1, &end_event));
            cl_ulong start, end;
            CL(clGetEventProfilingInfo(copy, CL_PROFILING_COMMAND_START, sizeof(start), &start, NULL));
            CL(clGetEventProfilingInfo(end_event, CL_PROFILING_COMMAND_END, sizeof(end), &end, NULL));
            printf("%s%.3f", run ? "," : "", (double)(end - start) / 1e6);
            CL(clReleaseEvent(copy)); CL(clReleaseEvent(end_event));
        }
        putchar('\n');
        CL(clEnqueueReadBuffer(queue, y, CL_TRUE, 0, output_size, output, 0, NULL, NULL));
        const double nrmse = verify(raw, raw_up, input, output, rows, columns, tokens, padded);
        if (nrmse > 0.004) return 1;
        if (mode == 0) memcpy(baseline, output, output_size);
        if (mode == 1 || mode == 2) {
            max_diff = 0; square_diff = 0; square_base = 0;
            for (size_t i = 0; i < (size_t)rows * tokens; ++i) {
                const double diff = (double)output[i] - baseline[i];
                max_diff = fmax(max_diff, fabs(diff)); square_diff += diff * diff;
                square_base += (double)baseline[i] * baseline[i];
            }
            printf("full_output_nrmse=%.8g max_diff=%.8g\n", sqrt(square_diff / fmax(square_base, 1e-30)), max_diff);
        }
    }

    CL(clReleaseMemObject(y));
    CL(clReleaseMemObject(both)); CL(clReleaseMemObject(bq)); CL(clReleaseMemObject(bd));
    CL(clReleaseMemObject(g)); CL(clReleaseMemObject(u));
    CL(clReleaseMemObject(x_image));
    CL(clReleaseMemObject(x));
    CL(clReleaseMemObject(uq)); CL(clReleaseMemObject(ud));
    CL(clReleaseMemObject(d));
    CL(clReleaseMemObject(q));
    CL(clReleaseKernel(kernel));
    CL(clReleaseKernel(fused)); CL(clReleaseKernel(combine)); CL(clReleaseKernel(combine_merged)); CL(clReleaseProgram(fused_program));
    CL(clReleaseProgram(program));
    CL(clReleaseCommandQueue(queue));
    CL(clReleaseContext(context));
    free(output);
    free(input);
    free(scales);
    free(scales_up); free(codes_up); free(raw_up); free(baseline); free(codes_both); free(scales_both);
    free(codes);
    free(raw);
    return 0;
}
