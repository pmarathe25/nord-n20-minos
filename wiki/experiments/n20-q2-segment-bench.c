/* Compare segment-reduction and FP16-texture Q2_0 projections. */
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

static double verify(const unsigned char *raw, const _Float16 *input, const float *output,
                     int rows, int columns, int tokens, int padded) {
    const int blocks = columns / BLOCK_COLUMNS;
    const int stride = rows <= 1024 ? 1 : 97;
    double error = 0, signal = 0, maximum = 0;
    unsigned count = 0;
    for (int token = 0; token < tokens; token += tokens > 16 ? 7 : 1) {
        for (int row = 0; row < rows; row += stride) {
            double expected = 0;
            for (int b = 0; b < blocks; ++b) {
                const unsigned char *block = raw + ((size_t)row * blocks + b) * BLOCK_BYTES;
                const float scale = weight_scale(block);
                for (int k = 0; k < BLOCK_COLUMNS; ++k) {
                    const int weight = ((block[2 + k / 4] >> (2 * (k % 4))) & 3) - 1;
                    expected += (double)scale * weight * input[(b * BLOCK_COLUMNS + k) * padded + token];
                }
            }
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
    const int decode = argc == 10;
    const int segment = getenv("N20_SEGMENT") != NULL;
    const int prefill_tile = argc == 9;
    const int row_tile = decode ? dimension(argv[7], 4, 16) :
        prefill_tile ? dimension(argv[7], 2, 4) : 4;
    const int token_tile = prefill_tile ? dimension(argv[8], 4, 16) : 8;
    const int workgroup = decode ? dimension(argv[8], 32, 512) : 128;
    const int splits = decode ? dimension(argv[9], 1, columns / BLOCK_COLUMNS) : 1;
    if ((argc != 6 && argc != 7 && !decode && !prefill_tile) || (segment && (decode || prefill_tile)) || !rows || !columns || !tokens ||
        !row_tile || (row_tile & (row_tile - 1)) || rows % row_tile ||
        !token_tile || (token_tile & (token_tile - 1)) ||
        !workgroup || !splits || workgroup % splits) {
        fprintf(stderr,
                "Usage: %s KERNEL WEIGHTS ROWS COLUMNS TOKENS [FLAGS [ROW_TILE TOKEN_TILE|ROW_TILE GROUP SPLITS]]\n",
                argv[0]);
        return 2;
    }
    const int blocks = columns / BLOCK_COLUMNS;
    // The backend keeps eight-token input padding when it uses the four-token tail.
    const int padding_tile = token_tile < 8 ? 8 : token_tile;
    const int padded = (tokens + padding_tile - 1) / padding_tile * padding_tile;
    const size_t weights = (size_t)rows * blocks;
    const size_t output_size = (size_t)rows * tokens * sizeof(float);
    const size_t input_size = (size_t)columns * padded * sizeof(_Float16);
    unsigned char *raw = read_file(argv[2], weights * BLOCK_BYTES, NULL);
    unsigned char *codes = alloc(weights * 16);
    uint16_t *scales = alloc(weights * sizeof(*scales));
    _Float16 *input = alloc(input_size);
    float *output = alloc(output_size);
    for (int row = 0; row < rows; ++row) {
        for (int b = 0; b < blocks; ++b) {
            const unsigned char *block = raw + ((size_t)row * blocks + b) * BLOCK_BYTES;
            memcpy(scales + b * (size_t)rows + row, block, sizeof(*scales));
            for (int i = 0; i < 16; ++i)
                codes[(b * 16 + i) * (size_t)rows + row] = block[2 + i];
        }
    }
    unsigned char *indices = NULL, *offsets = NULL;
    if (segment) {
        const size_t tiles = (size_t)(rows / 2) * blocks;
        indices = calloc(tiles, 64);
        offsets = calloc(tiles, 10);
        if (!indices || !offsets) { fputs("Unable to allocate segments\n", stderr); return 1; }
        for (int b = 0; b < blocks; ++b) {
            for (int row = 0; row < rows; row += 2) {
                const size_t tile = (size_t)b * (rows / 2) + row / 2;
                unsigned used = 0;
                for (int pattern = 0; pattern < 9; ++pattern) {
                    offsets[tile * 10 + pattern] = (unsigned char)used;
                    if (pattern == 4) continue;
                    for (int k = 0; k < 64; ++k) {
                        const unsigned char c0 = codes[(b * 16 + k / 4) * (size_t)rows + row];
                        const unsigned char c1 = codes[(b * 16 + k / 4) * (size_t)rows + row + 1];
                        const unsigned a = (c0 >> (2 * (k % 4))) & 3;
                        const unsigned c = (c1 >> (2 * (k % 4))) & 3;
                        if (a == 3 || c == 3) { fputs("Non-ternary code\n", stderr); return 1; }
                        if (a + 3 * c == (unsigned)pattern) indices[tile * 64 + used++] = (unsigned char)k;
                    }
                }
                offsets[tile * 10 + 9] = (unsigned char)used;
            }
        }
        printf("segment_index_bytes=%zu segment_offset_bytes=%zu\n", tiles * 64, tiles * 10);
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
                                       "-cl-fast-relaxed-math -DQ2_0_MV_WIDE=%d -DN20_DECODE_ROWS=%d %s",
                                       row_tile == 8, row_tile, argc >= 7 ? argv[6] : "");
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
    const char *name = segment ? "kernel_q2_segment_prefill" : decode ? "kernel_gemv_noshuffle_q2_0_f32" : "kernel_gemm_noshuffle_q2_0_f32";
    cl_kernel kernel = clCreateKernel(program, name, &status);
    check(status, "clCreateKernel");
    cl_mem q = segment ? upload(context, indices, (size_t)(rows / 2) * blocks * 64)
                       : upload(context, codes, weights * 16);
    cl_mem seg_offsets = segment ? upload(context, offsets, (size_t)(rows / 2) * blocks * 10) : NULL;
    cl_mem d = upload(context, scales, weights * sizeof(*scales));
    cl_mem x, x_image = NULL;
    if (decode) {
        const size_t bytes = (size_t)columns * tokens * sizeof(float);
        float *contiguous = alloc(bytes);
        for (int token = 0; token < tokens; ++token) {
            for (int k = 0; k < columns; ++k)
                contiguous[token * columns + k] = input[k * padded + token];
        }
        x = upload(context, contiguous, bytes);
        free(contiguous);
    } else {
        x = upload(context, input, input_size);
        const cl_image_format format = {CL_RGBA, CL_HALF_FLOAT};
        const cl_image_desc image = {.image_type = CL_MEM_OBJECT_IMAGE2D,
            .image_width = (size_t)padded / 4, .image_height = (size_t)columns};
        x_image = clCreateImage(context, CL_MEM_READ_ONLY, &format, &image, NULL, &status);
        check(status, "clCreateImage");
    }
    cl_mem y = clCreateBuffer(context, CL_MEM_WRITE_ONLY, output_size, NULL, &status);
    check(status, "clCreateBuffer(output)");

    const cl_ulong offset = 0;
    #define ARG(index, value) CL(clSetKernelArg(kernel, index, sizeof(value), &(value)))
    ARG(0, q);
    ARG(1, d);
    size_t global[2], local[2];
    if (decode) {
        ARG(2, x);
        ARG(3, offset);
        ARG(4, y);
        ARG(5, offset);
        ARG(6, columns);
        ARG(7, rows);
        ARG(8, columns);
        ARG(9, rows);
        const int lanes = workgroup / splits;
        const int rows_per_group = lanes * row_tile;
        global[0] = (size_t)(rows + rows_per_group - 1) / rows_per_group * lanes;
        global[1] = (size_t)tokens * splits;
        local[0] = (size_t)lanes;
        local[1] = (size_t)splits;
    } else {
        ARG(2, x_image);
        ARG(3, y);
        ARG(4, offset);
        ARG(5, rows);
        ARG(6, padded);
        ARG(7, columns);
        ARG(8, tokens);
        if (segment) { ARG(9, seg_offsets); }
        const int rows_per_group = (segment ? 2 : row_tile) * workgroup;
        global[0] = ((size_t)tokens + token_tile - 1) / token_tile;
        global[1] = (size_t)(rows + rows_per_group - 1) / rows_per_group * workgroup;
        local[0] = 1;
        local[1] = workgroup;
    }
    #undef ARG

    printf("rows=%d columns=%d tokens=%d copy_plus_gpu_ms=", rows, columns, tokens);
    for (int run = 0; run < RUNS; ++run) {
        cl_event event, copy = NULL;
        if (!decode) {
            const size_t origin[] = {0, 0, 0}, region[] = {(size_t)padded / 4, (size_t)columns, 1};
            CL(clEnqueueCopyBufferToImage(queue, x, x_image, 0, origin, region, 0, NULL, &copy));
        }
        CL(clEnqueueNDRangeKernel(queue, kernel, 2, NULL, global, local, 0, NULL, &event));
        CL(clWaitForEvents(1, &event));
        cl_ulong start, end;
        CL(clGetEventProfilingInfo(event, CL_PROFILING_COMMAND_START, sizeof(start), &start, NULL));
        CL(clGetEventProfilingInfo(event, CL_PROFILING_COMMAND_END, sizeof(end), &end, NULL));
        if (copy) {
            CL(clGetEventProfilingInfo(copy, CL_PROFILING_COMMAND_START, sizeof(start), &start, NULL));
            CL(clReleaseEvent(copy));
        }
        printf("%s%.3f", run ? "," : "", (double)(end - start) / 1e6);
        CL(clReleaseEvent(event));
    }
    putchar('\n');
    CL(clEnqueueReadBuffer(queue, y, CL_TRUE, 0, output_size, output, 0, NULL, NULL));
    const double nrmse = verify(raw, input, output, rows, columns, tokens, padded);

    CL(clReleaseMemObject(y));
    if (x_image)
        CL(clReleaseMemObject(x_image));
    CL(clReleaseMemObject(x));
    CL(clReleaseMemObject(d));
    CL(clReleaseMemObject(q));
    if (seg_offsets) CL(clReleaseMemObject(seg_offsets));
    CL(clReleaseKernel(kernel));
    CL(clReleaseProgram(program));
    CL(clReleaseCommandQueue(queue));
    CL(clReleaseContext(context));
    free(output);
    free(input);
    free(scales);
    free(codes);
    free(raw);
    free(indices); free(offsets);
    return nrmse > 0.003 ? 1 : 0;
}
