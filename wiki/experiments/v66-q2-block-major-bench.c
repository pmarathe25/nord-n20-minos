/* Block-major input conversion is outside timing; not a full pipeline measurement. */
#define _POSIX_C_SOURCE 200809L
#include "v66-q2.h"

#include <dlfcn.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

_Static_assert(sizeof(union rpc_arg) == 16, "ARM64 FastRPC argument layout");

static uint64_t now_us(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * 1000000 + (uint64_t)time.tv_nsec / 1000;
}

static void *allocate(size_t bytes) {
    void *memory = malloc(bytes);
    if (!memory) {
        fputs("Unable to allocate a test buffer\n", stderr);
        exit(1);
    }
    return memory;
}

static float weight_scale(const uint8_t *block) {
    _Float16 half;
    memcpy(&half, block, sizeof(half));
    return (float)half;
}

static unsigned code(const uint8_t *block, unsigned index) {
    return (block[2 + index / 4] >> (2 * (index % 4))) & 3;
}

static struct weight_tile *pack_weights(const uint8_t *raw, uint32_t rows,
                                        uint32_t blocks, float *unit) {
    float maximum = 0;
    const size_t n = (size_t)rows * blocks;
    for (size_t i = 0; i < n; ++i) {
        const float scale = weight_scale(raw + i * Q2_BYTES);
        if (!isfinite(scale) || scale < 0) {
            fputs("Expected finite nonnegative Q2_0 scales\n", stderr);
            exit(1);
        }
        maximum = fmaxf(maximum, scale);
    }
    /* At most 64 terms of magnitude 2 * 127 per block. */
    const unsigned safe_scale = INT32_MAX / (blocks * 64 * 2 * 127);
    const unsigned limit = safe_scale < UINT16_MAX ? safe_scale : UINT16_MAX;
    *unit = maximum == 0 ? 1 : maximum / limit;
    struct weight_tile *packed = allocate(n / HVX_ROWS * sizeof(*packed));
    for (uint32_t row = 0; row < rows; ++row) {
        for (uint32_t block = 0; block < blocks; ++block) {
            struct weight_tile *tile = packed + row / HVX_ROWS * blocks + block;
            const uint8_t *source = raw + ((size_t)row * blocks + block) * Q2_BYTES;
            for (unsigned group = 0; group < 4; ++group)
                memcpy(&tile->codes[group][row % HVX_ROWS], source + 2 + 4 * group, 4);
            tile->scales[row % HVX_ROWS] = (uint32_t)lroundf(weight_scale(source) / *unit);
        }
    }
    return packed;
}

static void make_activations(float *exact, int8_t *quantized, struct activation_block *packed,
                             uint32_t count) {
    uint32_t random = 1;
    for (uint32_t i = 0; i < count; ++i) {
        struct activation_block *block = packed + i;
        *block = (struct activation_block){0};
        for (unsigned j = 0; j < Q2_WIDTH; ++j) {
            random = random * 1664525u + 1013904223u;
            const float x = (float)((int32_t)(random >> 16) - 32768) / 32768.0f;
            const int8_t q = (int8_t)lroundf(x * 127);
            exact[i * Q2_WIDTH + j] = x;
            quantized[i * Q2_WIDTH + j] = q;
            block->sum += q;
            block->groups[j / 16][j % 4] |= (uint32_t)(uint8_t)q << (8 * (j % 16 / 4));
        }
    }
}

static int verify(const uint8_t *raw, const struct weight_tile *weights, const float *exact,
                  const int8_t *quantized, const int32_t *output, uint32_t rows,
                  uint32_t blocks, uint32_t tokens, float unit) {
    double error = 0, signal = 0;
    uint32_t checked = 0;
    for (uint32_t token = 0; token < tokens; ++token) {
        const uint32_t stride = tokens == 1 ? 1 : rows / 64 + 1;
        for (uint32_t row = 0; row < rows; row += stride) {
            int32_t expected = 0;
            double reference = 0;
            for (uint32_t b = 0; b < blocks; ++b) {
                const uint8_t *source = raw + ((size_t)row * blocks + b) * Q2_BYTES;
                const unsigned scale = weights[row / HVX_ROWS * blocks + b].scales[row % HVX_ROWS];
                const uint32_t start = (token * blocks + b) * Q2_WIDTH;
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
            const int32_t got = output[(row / HVX_ROWS * tokens + token) * HVX_ROWS + row % HVX_ROWS];
            if (got != expected) {
                fprintf(stderr, "Mismatch at token %u row %u: %" PRId32 " vs %" PRId32 "\n",
                        token, row, got, expected);
                return 1;
            }
            const double difference = got * (double)unit / 127 - reference;
            error += difference * difference;
            signal += reference * reference;
            ++checked;
        }
    }
    printf("checked=%u normalized_rms_error=%.6f\n", checked,
           signal ? sqrt(error / signal) : sqrt(error / checked));
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fputs("usage: n20-q2-bench Q2_FILE ROWS COLUMNS\n", stderr);
        return 2;
    }
    const unsigned long rows_arg = strtoul(argv[2], NULL, 10);
    const unsigned long cols_arg = strtoul(argv[3], NULL, 10);
    if (!rows_arg || rows_arg > MAX_ROWS || rows_arg % HVX_ROWS ||
        !cols_arg || cols_arg > MAX_COLS || cols_arg % Q2_WIDTH)
        return 2;
    const uint32_t rows = (uint32_t)rows_arg;
    const uint32_t blocks = (uint32_t)cols_arg / Q2_WIDTH;
    const size_t raw_size = (size_t)rows * blocks * Q2_BYTES;
    uint8_t *raw = allocate(raw_size);
    FILE *file = fopen(argv[1], "rb");
    if (!file || fread(raw, 1, raw_size, file) != raw_size) {
        fputs("Cannot read Q2_0 tensor\n", stderr);
        return 1;
    }
    fclose(file);
    float unit;
    const uint64_t pack_start = now_us();
    struct weight_tile *weights = pack_weights(raw, rows, blocks, &unit);
    const uint32_t weight_size = rows / HVX_ROWS * blocks * sizeof(*weights);
    printf("rows=%u cols=%lu weight_bytes=%u pack_us=%" PRIu64 "\n",
           rows, cols_arg, weight_size, now_us() - pack_start);
    fflush(stdout);

    void *library = dlopen("libcdsprpc.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    int (*control)(uint32_t, void *, uint32_t) = dlsym(library, "remote_session_control");
    int (*open)(const char *, uint64_t *) = dlsym(library, "remote_handle64_open");
    int (*invoke)(uint64_t, uint32_t, union rpc_arg *) = dlsym(library, "remote_handle64_invoke");
    int (*close)(uint64_t) = dlsym(library, "remote_handle64_close");
    if (!control || !open || !invoke || !close)
        return 1;
    int unsigned_module[] = {3, 1};
    int status = control(2, unsigned_module, sizeof(unsigned_module));
    uint64_t handle = 0;
    if (!status)
        status = open("file:///libn20_projection_skel.so?"
                      "n20_projection_skel_handle_invoke&_modver=1.0&_dom=cdsp", &handle);
    if (status) {
        fprintf(stderr, "FastRPC setup failed: 0x%08x\n", (unsigned)status);
        return 1;
    }
    const uint64_t upload_start = now_us();
    for (uint32_t offset = 0; offset < weight_size; offset += MAX_UPLOAD) {
        struct upload request = {rows, (uint32_t)cols_arg, offset};
        const uint32_t remaining = weight_size - offset;
        union rpc_arg args[] = {
            {.buffer = {&request, sizeof(request)}},
            {.buffer = {(uint8_t *)weights + offset, remaining < MAX_UPLOAD ? remaining : MAX_UPLOAD}},
        };
        status = invoke(handle, RPC_UPLOAD, args);
        if (status) {
            fprintf(stderr, "Upload failed at %u: 0x%08x\n", offset, (unsigned)status);
            close(handle);
            return 1;
        }
    }
    printf("upload_us=%" PRIu64 "\n", now_us() - upload_start);
    fflush(stdout);

    float *exact = allocate(MAX_TOKENS * cols_arg * sizeof(*exact));
    int8_t *quantized = allocate(MAX_TOKENS * cols_arg);
    /* Opt-in transport comparison; neither path includes GPU conversion. */
    const int shared = getenv("N20_SHARED") != NULL;
    void *(*rpc_alloc)(int, uint32_t, int) = dlsym(library, "rpcmem_alloc");
    void (*rpc_free)(void *) = dlsym(library, "rpcmem_free");
    if (shared && (!rpc_alloc || !rpc_free)) {
        close(handle);
        return 1;
    }
    const size_t input_bytes = MAX_TOKENS * blocks * sizeof(struct activation_block);
    struct activation_block *input = shared ? rpc_alloc(25, 0, (int)input_bytes) : allocate(input_bytes);
    void *aligned_output;
    if (posix_memalign(&aligned_output, HVX_BYTES, MAX_TOKENS * rows * sizeof(int32_t))) {
        close(handle);
        return 1;
    }
    int32_t *output = aligned_output;
    if (shared) {
        free(output);
        output = rpc_alloc(25, 0, (int)(MAX_TOKENS * rows * sizeof(*output)));
    }
    if (!input || !output) {
        fputs("Unable to allocate shared projection buffers\n", stderr);
        close(handle);
        return 1;
    }
    printf("shared=%d\n", shared);
    struct activation_block *token_major = allocate(input_bytes);
    make_activations(exact, quantized, token_major, MAX_TOKENS * blocks);
    const uint32_t batches[] = {1, 4, 16, 128, 15, 17, 31, 32, 33, 63, 127};
    const unsigned count = getenv("N20_EXTRA_BATCHES") ? (unsigned)(sizeof(batches) / sizeof(batches[0])) : 4;
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t tokens = batches[i];
        for (uint32_t b = 0; b < blocks; ++b)
            for (uint32_t t = 0; t < tokens; ++t)
                input[b * tokens + t] = token_major[t * blocks + b];
        for (unsigned sample = 0; sample < 4; ++sample) {
            struct projection_request request = {tokens};
            struct projection_timing timing = {0};
            union rpc_arg args[] = {
                {.buffer = {&request, sizeof(request)}},
                {.buffer = {input, tokens * blocks * sizeof(*input)}},
                {.buffer = {&timing, sizeof(timing)}},
                {.buffer = {output, tokens * rows * sizeof(*output)}},
            };
            const uint64_t start = now_us();
            status = invoke(handle, RPC_PROJECT, args);
            const uint64_t elapsed = now_us() - start;
            if (status || !timing.elapsed_us || timing.hvx_mode != HVX_MODE_128) {
                fprintf(stderr, "Projection failed: 0x%08x hvx_mode=%d units=%d\n",
                        (unsigned)status, timing.hvx_mode, timing.hvx_units);
                close(handle);
                return 1;
            }
            printf("tokens=%u sample=%u kernel_us=%" PRIu64 " rpc_us=%" PRIu64
                   " hvx_units=%d direct_output=%u\n",
                   tokens, sample, timing.elapsed_us, elapsed, timing.hvx_units, timing.direct_output);
            fflush(stdout);
        }
        if (getenv("N20_OUTPUT_HASH")) {
            uint64_t hash = UINT64_C(14695981039346656037);
            const uint8_t *bytes = (const uint8_t *)output;
            for (size_t n = 0; n < (size_t)tokens * rows * sizeof(*output); ++n)
                hash = (hash ^ bytes[n]) * UINT64_C(1099511628211);
            printf("output_fnv64=%016" PRIx64 "\n", hash);
        }
        if (verify(raw, weights, exact, quantized, output, rows, blocks, tokens, unit)) {
            close(handle);
            return 1;
        }
        if (tokens == 1) {
            struct projection_top1 expected = {0, output[0]};
            for (uint32_t row = 1; row < rows; ++row) {
                if (output[row] > expected.value)
                    expected = (struct projection_top1){row, output[row]};
            }
            for (unsigned sample = 0; sample < 7; ++sample) {
                struct projection_request request = {1};
                struct projection_timing timing = {0};
                struct projection_top1 best = {0};
                union rpc_arg args[] = {
                    {.buffer = {&request, sizeof(request)}},
                    {.buffer = {input, blocks * sizeof(*input)}},
                    {.buffer = {&timing, sizeof(timing)}},
                    {.buffer = {&best, sizeof(best)}},
                };
                const uint64_t start = now_us();
                status = invoke(handle, RPC_TOP1, args);
                const uint64_t elapsed = now_us() - start;
                if (status || !timing.elapsed_us || best.row != expected.row || best.value != expected.value) {
                    fprintf(stderr, "FastRPC top-1 failed or disagreed with the verified projection\n");
                    close(handle);
                    return 1;
                }
                printf("top1 sample=%u kernel_us=%" PRIu64 " rpc_us=%" PRIu64 " row=%u\n",
                       sample, timing.elapsed_us, elapsed, best.row);
            }
            fflush(stdout);
        }
    }
    if (shared) {
        rpc_free(output);
        rpc_free(input);
    } else {
        free(output);
        free(input);
    }
    free(token_major);
    free(quantized);
    free(exact);
    free(weights);
    free(raw);
    return close(handle) != 0;
}
