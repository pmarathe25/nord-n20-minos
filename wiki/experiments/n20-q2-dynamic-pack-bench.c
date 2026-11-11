/* Screen the existing dynamic-Q8 reduction/packing choices in isolation.
 * The included row-view harness supplies the kernels and CPU reference. */
#define N20_PACK_ONLY
#include "n20-q2-overlap-rowview-bench.c"

int main(int argc, char **argv) {
    if (argc < 3 || argc > 5 || (argc == 5 && strcmp(argv[4], "dual"))) {
        fprintf(stderr, "usage: %s COLUMNS TOKENS [PARALLEL_PACK_SOURCE [dual]]\n", argv[0]);
        return 2;
    }
    const unsigned columns = parse_number(argv[1], MAX_COLS);
    const unsigned tokens = parse_number(argv[2], 256);
    const int dual = argc == 5;
    if (!columns || columns % Q2_WIDTH || !tokens) return 2;
    const unsigned blocks = columns / Q2_WIDTH;
    const unsigned padded = (tokens + 7u) & ~7u;
    const size_t values = (size_t)columns * tokens;
    float *exact = allocate(values * sizeof(*exact));
    float *maxima = allocate(tokens * sizeof(*maxima));
    int8_t *quantized = allocate(values);
    struct activation_block *expected = allocate((size_t)tokens * blocks * sizeof(*expected));
    struct activation_block *actual = allocate((size_t)tokens * blocks * sizeof(*actual));
    _Float16 *input = calloc((size_t)columns * padded, sizeof(*input));
    if (!input) return 1;
    make_activations(exact, quantized, expected, tokens * blocks);
    for (unsigned t = 0; t < tokens; ++t)
        for (unsigned k = 0; k < columns; ++k) {
            size_t i = (size_t)t * columns + k;
            float factor = t % 3 == 0 ? 0.0f : t % 3 == 1 ? 1.0f : 8.0f;
            exact[i] = (float)(_Float16)(exact[i] * factor);
            input[(size_t)k * padded + t] = (_Float16)exact[i];
        }
    reference_blocks(exact, maxima, quantized, expected, tokens, blocks);
    cl_platform_id platform;
    cl_device_id device;
    CL(clGetPlatformIDs(1, &platform, NULL));
    CL(clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &device, NULL));
    cl_int err;
    cl_context context = clCreateContext(NULL, 1, &device, NULL, NULL, &err);
    cl_check(err, "context");
    const cl_queue_properties props[] = {CL_QUEUE_PROPERTIES, CL_QUEUE_PROFILING_ENABLE, 0};
    cl_command_queue queue = clCreateCommandQueueWithProperties(context, device, props, &err);
    cl_check(err, "queue");
    cl_program program = build_auxiliary(context, device);
    cl_kernel reductions[2] = {clCreateKernel(program, "reduce_max", &err), NULL};
    cl_check(err, "reduce_max");
    reductions[1] = clCreateKernel(program, "reduce_max_token8", &err);
    cl_check(err, "reduce_max_token8");
    cl_kernel pack = clCreateKernel(program, "pack", &err);
    cl_check(err, "pack");
    cl_program parallel_program = NULL;
    cl_kernel parallel_pack = NULL;
    if (argc >= 4) {
        size_t length;
        char *source = read_source(argv[3], &length);
        parallel_program = clCreateProgramWithSource(context, 1, (const char **)&source, &length, &err);
        cl_check(err, "parallel program");
        err = clBuildProgram(parallel_program, 1, &device, "-cl-std=CL2.0", NULL, NULL);
        if (err != CL_SUCCESS) {
            size_t n = 0;
            CL(clGetProgramBuildInfo(parallel_program, device, CL_PROGRAM_BUILD_LOG, 0, NULL, &n));
            char *log = allocate(n + 1);
            CL(clGetProgramBuildInfo(parallel_program, device, CL_PROGRAM_BUILD_LOG, n, log, NULL));
            log[n] = 0; fprintf(stderr, "%s\n", log); free(log);
            cl_check(err, "parallel build");
        }
        free(source);
        parallel_pack = clCreateKernel(parallel_program, dual ? "parallel_pack_q8" : "parallel_pack", &err);
        cl_check(err, "parallel kernel");
    }
    cl_mem x = copy_buffer(context, input, (size_t)columns * padded * sizeof(*input));
    cl_mem out = clCreateBuffer(context, CL_MEM_READ_WRITE, (size_t)tokens * blocks * sizeof(*actual), NULL, &err);
    cl_check(err, "out");
    cl_mem out2 = NULL;
    if (dual) {
        out2 = clCreateBuffer(context, CL_MEM_READ_WRITE, (size_t)tokens * blocks * sizeof(*actual), NULL, &err);
        cl_check(err, "out2");
    }
    cl_mem maxbuf = clCreateBuffer(context, CL_MEM_READ_WRITE, tokens * sizeof(float), NULL, &err);
    cl_check(err, "maxima");
    cl_mem inverse = clCreateBuffer(context, CL_MEM_READ_WRITE, tokens * sizeof(float), NULL, &err);
    cl_check(err, "inverse");
    cl_mem guard = clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(cl_uint), NULL, &err);
    cl_check(err, "guard");
    const cl_uint count = tokens * blocks, z = 0;
    for (int r = 0; r < 2; ++r) {
        cl_kernel k = reductions[r];
        CL(clSetKernelArg(k, 0, sizeof(x), &x)); CL(clSetKernelArg(k, 1, sizeof(maxbuf), &maxbuf));
        CL(clSetKernelArg(k, 2, sizeof(inverse), &inverse)); CL(clSetKernelArg(k, 3, sizeof(guard), &guard));
        CL(clSetKernelArg(k, 4, sizeof(columns), &columns)); CL(clSetKernelArg(k, 5, sizeof(padded), &padded));
        if (r) CL(clSetKernelArg(k, 6, sizeof(tokens), &tokens));
    }
    CL(clSetKernelArg(pack, 0, sizeof(x), &x)); CL(clSetKernelArg(pack, 1, sizeof(out), &out));
    CL(clSetKernelArg(pack, 2, sizeof(inverse), &inverse)); CL(clSetKernelArg(pack, 3, sizeof(blocks), &blocks));
    CL(clSetKernelArg(pack, 4, sizeof(padded), &padded)); CL(clSetKernelArg(pack, 5, sizeof(count), &count));
    CL(clSetKernelArg(pack, 6, sizeof(tokens), &tokens));
    if (parallel_pack) {
        CL(clSetKernelArg(parallel_pack, 0, sizeof(x), &x));
        CL(clSetKernelArg(parallel_pack, 1, sizeof(out), &out));
        if (dual) {
            CL(clSetKernelArg(parallel_pack, 2, sizeof(out2), &out2));
            CL(clSetKernelArg(parallel_pack, 3, sizeof(inverse), &inverse));
            CL(clSetKernelArg(parallel_pack, 4, sizeof(blocks), &blocks));
            CL(clSetKernelArg(parallel_pack, 5, sizeof(padded), &padded));
            CL(clSetKernelArg(parallel_pack, 6, sizeof(count), &count));
            CL(clSetKernelArg(parallel_pack, 7, sizeof(tokens), &tokens));
        } else {
            CL(clSetKernelArg(parallel_pack, 2, sizeof(inverse), &inverse));
            CL(clSetKernelArg(parallel_pack, 3, sizeof(blocks), &blocks));
            CL(clSetKernelArg(parallel_pack, 4, sizeof(padded), &padded));
            CL(clSetKernelArg(parallel_pack, 5, sizeof(tokens), &tokens));
        }
    }
    /* Baseline brackets the three options; warm each variant before timing. */
    const unsigned variants[][2] = {{0,0},{0,1},{1,0},{1,1},{1,2},{1,1},{0,0}};
    for (size_t v = 0; v < sizeof(variants)/sizeof(variants[0]); ++v) {
        const cl_uint reduce8 = variants[v][0], fast = variants[v][1];
        if (fast == 2 && !parallel_pack) continue;
        if (fast != 2) CL(clSetKernelArg(pack, 7, sizeof(fast), &fast));
        const size_t local = 64, rg = (size_t)(reduce8 ? (tokens + 7) / 8 : tokens) * 64, pg = count;
        const size_t parallel_local = 128, parallel_global = (size_t)((tokens + 7) / 8) * blocks * parallel_local;
        printf("reduce8=%u token_fast=%u", reduce8, fast);
        for (int sample = -1; sample < 5; ++sample) {
            cl_event a, b;
            if (dual && fast == 2) {
                const cl_uint sentinel = 0xa5a5a5a5u;
                const size_t bytes = (size_t)count * sizeof(*actual);
                CL(clEnqueueFillBuffer(queue, out, &sentinel, sizeof(sentinel), 0, bytes, 0, NULL, NULL));
                CL(clEnqueueFillBuffer(queue, out2, &sentinel, sizeof(sentinel), 0, bytes, 0, NULL, NULL));
            }
            CL(clEnqueueFillBuffer(queue, guard, &z, sizeof(z), 0, sizeof(z), 0, NULL, NULL));
            CL(clEnqueueNDRangeKernel(queue, reductions[reduce8], 1, NULL, &rg, &local, 0, NULL, &a));
            if (fast == 2)
                CL(clEnqueueNDRangeKernel(queue, parallel_pack, 1, NULL, &parallel_global, &parallel_local, 0, NULL, &b));
            else
                CL(clEnqueueNDRangeKernel(queue, pack, 1, NULL, &pg, NULL, 0, NULL, &b));
            CL(clWaitForEvents(1, &b));
            if (sample >= 0) printf(" %.4f", event_ms(a, b));
            CL(clReleaseEvent(a)); CL(clReleaseEvent(b));
        }
        cl_uint observed_guard = 1;
        float *observed_maxima = allocate(tokens * sizeof(*observed_maxima));
        const unsigned first = dual && fast == 2 && tokens > 128 ? 128 : tokens;
        CL(clEnqueueReadBuffer(queue, out, CL_TRUE, 0, (size_t)first * blocks * sizeof(*actual), actual, 0, NULL, NULL));
        if (first < tokens)
            CL(clEnqueueReadBuffer(queue, out2, CL_TRUE, 0, (size_t)(tokens-first) * blocks * sizeof(*actual),
                                   actual + (size_t)first * blocks, 0, NULL, NULL));
        if (dual && fast == 2) {
            const size_t bytes = (size_t)count * sizeof(*actual);
            unsigned char *canary = allocate(bytes);
            const size_t used[] = {(size_t)first * blocks * sizeof(*actual),
                                   (size_t)(tokens - first) * blocks * sizeof(*actual)};
            cl_mem outputs[] = {out, out2};
            for (unsigned i = 0; i < 2; ++i) {
                CL(clEnqueueReadBuffer(queue, outputs[i], CL_TRUE, 0, bytes, canary, 0, NULL, NULL));
                for (size_t j = used[i]; j < bytes; ++j)
                    if (canary[j] != 0xa5) { fputs("\noutput canary modified\n", stderr); return 1; }
            }
            free(canary);
        }
        CL(clEnqueueReadBuffer(queue, maxbuf, CL_TRUE, 0, tokens * sizeof(float), observed_maxima, 0, NULL, NULL));
        CL(clEnqueueReadBuffer(queue, guard, CL_TRUE, 0, sizeof(observed_guard), &observed_guard, 0, NULL, NULL));
        if (observed_guard || memcmp(expected, actual, (size_t)count * sizeof(*actual)) ||
            memcmp(maxima, observed_maxima, tokens * sizeof(float))) {
            fputs("\nreduction or packed bytes mismatch\n", stderr); return 1;
        }
        free(observed_maxima);
        puts(" exact=1");
    }
    CL(clReleaseMemObject(guard)); CL(clReleaseMemObject(inverse)); CL(clReleaseMemObject(maxbuf));
    if (out2) CL(clReleaseMemObject(out2));
    CL(clReleaseMemObject(out)); CL(clReleaseMemObject(x));
    if (parallel_pack) { CL(clReleaseKernel(parallel_pack)); CL(clReleaseProgram(parallel_program)); }
    CL(clReleaseKernel(pack)); CL(clReleaseKernel(reductions[0])); CL(clReleaseKernel(reductions[1]));
    CL(clReleaseProgram(program)); CL(clReleaseCommandQueue(queue)); CL(clReleaseContext(context));
    free(input); free(actual); free(expected); free(quantized); free(maxima); free(exact);
    return 0;
}
