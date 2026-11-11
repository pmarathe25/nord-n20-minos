/* Compare packed-integer and FP16-texture Q2_0 projections with real weights. */
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
    const int radix = getenv("N20_RADIX") != NULL;
    const int prefill_tile = argc == 9;
    const int row_tile = decode ? dimension(argv[7], 4, 16) :
        prefill_tile ? dimension(argv[7], 2, 4) : 4;
    const int token_tile = prefill_tile ? dimension(argv[8], 4, 16) : 8;
    const int workgroup = decode ? dimension(argv[8], 32, 512) : 128;
    const int splits = decode ? dimension(argv[9], 1, columns / BLOCK_COLUMNS) : 1;
    if ((argc != 6 && argc != 7 && !decode && !prefill_tile) || (radix && (decode || prefill_tile)) || !rows || !columns || !tokens ||
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
    uint32_t rng = 621;
    for (int k = 0; k < columns; ++k) {
        for (int token = 0; token < padded; ++token) {
            rng = 1664525u * rng + 1013904223u;
            input[k * padded + token] = token < tokens
                ? (float)((int)(rng >> 16) - 32768) / 16384 : 0;
        }
    }

    // The experimental radix path is intentionally prepacked on the host.
    // Its GPU time is an optimistic bound that excludes activation preparation.
    uint32_t *packed = NULL;
    float *xscale = NULL;
    uint16_t *xsum = NULL;
    unsigned char *csum = NULL;
    if (radix) {
        packed = alloc((size_t)columns / 4 * padded * sizeof(*packed));
        xscale = alloc((size_t)blocks * padded * sizeof(*xscale));
        xsum = alloc((size_t)blocks * padded * sizeof(*xsum));
        csum = alloc(weights);
        for (int b = 0; b < blocks; ++b) {
            for (int row = 0; row < rows; ++row) {
                unsigned sum = 0;
                for (int i = 0; i < 16; ++i) {
                    const unsigned char c = codes[(b * 16 + i) * (size_t)rows + row];
                    for (int j = 0; j < 4; ++j) sum += (c >> (2 * j)) & 3;
                }
                csum[(size_t)b * rows + row] = (unsigned char)sum;
            }
            for (int t = 0; t < padded; ++t) {
                float peak = 0;
                for (int k = 0; k < 64; ++k)
                    peak = fmaxf(peak, fabsf(input[(b * 64 + k) * padded + t]));
                const float scale = peak ? peak / 127.0f : 1.0f;
                xscale[(size_t)b * padded + t] = scale;
                unsigned sum = 0;
                for (int i = 0; i < 16; ++i) {
                    uint32_t p = 0;
                    for (int j = 0; j < 4; ++j) {
                        int q = (int)roundf((float)input[(b * 64 + i * 4 + j) * padded + t] / scale);
                        if (q < -127) q = -127;
                        if (q > 127) q = 127;
                        const unsigned a = (unsigned)(q + 127);
                        p |= a << (8 * j);
                        sum += a;
                    }
                    packed[(size_t)(b * 16 + i) * padded + t] = p;
                }
                xsum[(size_t)b * padded + t] = (uint16_t)sum;
            }
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
                                       "-cl-fast-relaxed-math -DQ2_0_MV_WIDE=%d -DN20_DECODE_ROWS=%d %s %s",
                                       row_tile == 8, row_tile, getenv("N20_RADIX8") ? "-DN20_RADIX_TOKENS=8" : "",
                                       argc >= 7 ? argv[6] : "");
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
    const char *name = radix ? "kernel_q2_radix_prefill" : decode ? "kernel_gemv_noshuffle_q2_0_f32" : "kernel_gemm_noshuffle_q2_0_f32";
    cl_kernel kernel = clCreateKernel(program, name, &status);
    check(status, "clCreateKernel");
    cl_mem q = upload(context, codes, weights * 16);
    cl_mem d = upload(context, scales, weights * sizeof(*scales));
    cl_mem x, x_image = NULL, xs = NULL, xa = NULL, cs = NULL;
    if (radix) {
        const size_t bytes = (size_t)columns / 4 * padded * sizeof(*packed);
        x = upload(context, packed, bytes);
        const cl_image_format format = {CL_RGBA, CL_UNSIGNED_INT32};
        const cl_image_desc image = {.image_type = CL_MEM_OBJECT_IMAGE2D,
            .image_width = (size_t)padded / 4, .image_height = (size_t)columns / 4};
        x_image = clCreateImage(context, CL_MEM_READ_ONLY, &format, &image, NULL, &status);
        check(status, "clCreateImage(radix)");
        const size_t origin[] = {0, 0, 0}, region[] = {(size_t)padded / 4, (size_t)columns / 4, 1};
        CL(clEnqueueCopyBufferToImage(queue, x, x_image, 0, origin, region, 0, NULL, NULL));
        xs = upload(context, xscale, (size_t)blocks * padded * sizeof(*xscale));
        xa = upload(context, xsum, (size_t)blocks * padded * sizeof(*xsum));
        cs = upload(context, csum, weights);
    } else if (decode) {
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
        const cl_image_desc image = {
            .image_type = CL_MEM_OBJECT_IMAGE2D, .image_width = (size_t)padded / 4,
            .image_height = (size_t)columns,
        };
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
    if (radix) {
        ARG(2, x_image); ARG(3, xs); ARG(4, xa); ARG(5, cs); ARG(6, y);
        ARG(7, rows); ARG(8, padded); ARG(9, columns); ARG(10, tokens);
        const int rows_per_group = 4 * workgroup;
        const int radix_tokens = getenv("N20_RADIX8") ? 8 : 4;
        global[0] = ((size_t)tokens + radix_tokens - 1) / radix_tokens;
        global[1] = (size_t)(rows + rows_per_group - 1) / rows_per_group * workgroup;
        local[0] = 1; local[1] = workgroup;
    } else if (decode) {
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
        const int rows_per_group = row_tile * workgroup;
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
            const size_t origin[] = {0, 0, 0};
            const size_t region[] = {(size_t)padded / 4, (size_t)columns / (radix ? 4 : 1), 1};
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

    if (radix) {
        double max_diff = 0, max_expected = 0;
        unsigned count = 0;
        for (int t = 0; t < tokens; t += tokens > 16 ? 7 : 1) {
            for (int row = 0; row < rows; row += rows <= 1024 ? 1 : 97) {
                double expected = 0;
                for (int b = 0; b < blocks; ++b) {
                    int sum = 0;
                    for (int i = 0; i < 16; ++i) {
                        const uint32_t p = packed[(size_t)(b * 16 + i) * padded + t];
                        const unsigned char c = codes[(b * 16 + i) * (size_t)rows + row];
                        for (int j = 0; j < 4; ++j)
                            sum += ((int)((p >> (8 * j)) & 255) - 127) * ((int)((c >> (2 * j)) & 3) - 1);
                    }
                    const unsigned char *block = raw + ((size_t)row * blocks + b) * BLOCK_BYTES;
                    expected += (double)sum * xscale[(size_t)b * padded + t] * weight_scale(block);
                }
                max_diff = fmax(max_diff, fabs(output[(size_t)t * rows + row] - expected));
                max_expected = fmax(max_expected, fabs(expected));
                ++count;
            }
        }
        printf("quantized_reference_checked=%u max_error=%.7g max_expected=%.7g\n", count, max_diff, max_expected);
        if (max_diff > 0.0001 * fmax(1.0, max_expected)) return 1;
    }

    CL(clReleaseMemObject(y));
    if (x_image)
        CL(clReleaseMemObject(x_image));
    CL(clReleaseMemObject(x));
    if (xs) CL(clReleaseMemObject(xs));
    if (xa) CL(clReleaseMemObject(xa));
    if (cs) CL(clReleaseMemObject(cs));
    CL(clReleaseMemObject(d));
    CL(clReleaseMemObject(q));
    CL(clReleaseKernel(kernel));
    CL(clReleaseProgram(program));
    CL(clReleaseCommandQueue(queue));
    CL(clReleaseContext(context));
    free(output);
    free(input);
    free(scales);
    free(codes);
    free(raw);
    free(packed); free(xscale); free(xsum); free(csum);
    return nrmse > (radix ? 0.01 : 0.003) ? 1 : 0;
}
