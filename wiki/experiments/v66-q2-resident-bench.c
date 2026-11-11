/* Device-only resident-slot correctness probe. It does not modify the service.
 * Inputs are two distinct row-major Q2_0 tensors of the same shape. ROWS is
 * the number of leading rows used in each input (files may contain more).
 * Every DSP output is checked against a scalar calculation from the original
 * Q2 bytes and signed activations; the scalar path does not use packed weights.
 */
#define main n20_original_projection_probe_main
#include "v66-q2-bench.c"
#undef main
#include "v66-q2-resident.h"

struct client {
    uint64_t handle;
    int (*invoke)(uint64_t, uint32_t, union rpc_arg *);
};

static int select_slot(struct client *client, uint32_t slot) {
    struct n20_resident_slot request = {slot};
    union rpc_arg args[] = {{.buffer = {&request, sizeof(request)}}};
    return client->invoke(client->handle, RPC_RESIDENT_SELECT, args);
}

static int evict_slot(struct client *client, uint32_t slot) {
    struct n20_resident_slot request = {slot};
    union rpc_arg args[] = {{.buffer = {&request, sizeof(request)}}};
    return client->invoke(client->handle, RPC_RESIDENT_EVICT, args);
}

static int get_stats(struct client *client, struct n20_resident_stats *stats) {
    union rpc_arg args[] = {{.buffer = {stats, sizeof(*stats)}}};
    memset(stats, 0, sizeof(*stats));
    return client->invoke(client->handle, RPC_RESIDENT_STATS, args);
}

static int upload_chunk(struct client *client, const struct weight_tile *weights,
                        uint32_t rows, uint32_t columns, uint32_t offset, uint32_t bytes) {
    struct upload request = {rows, columns, offset};
    union rpc_arg args[] = {{.buffer = {&request, sizeof(request)}},
                            {.buffer = {(uint8_t *)weights + offset, bytes}}};
    return client->invoke(client->handle, RPC_UPLOAD, args);
}

static int finish_upload(struct client *client, const struct weight_tile *weights,
                         uint32_t rows, uint32_t columns, uint32_t offset, uint32_t total) {
    while (offset < total) {
        const uint32_t bytes = total - offset < MAX_UPLOAD ? total - offset : MAX_UPLOAD;
        const int status = upload_chunk(client, weights, rows, columns, offset, bytes);
        if (status) return status;
        offset += bytes;
    }
    return 0;
}

static int project(struct client *client, struct activation_block *input, uint32_t input_bytes,
                   int32_t *output, uint32_t output_bytes, uint32_t tokens,
                   struct projection_timing *timing) {
    struct projection_request request = {tokens};
    *timing = (struct projection_timing){0};
    union rpc_arg args[] = {{.buffer = {&request, sizeof(request)}},
                            {.buffer = {input, input_bytes}},
                            {.buffer = {timing, sizeof(*timing)}},
                            {.buffer = {output, output_bytes}}};
    return client->invoke(client->handle, RPC_PROJECT, args);
}

static int scalar_check(const uint8_t *raw, const int8_t *activations, const int32_t *output,
                        float unit, uint32_t rows, uint32_t blocks, uint32_t tokens) {
    uint64_t checked = 0;
    for (uint32_t t = 0; t < tokens; ++t) {
        for (uint32_t row = 0; row < rows; ++row) {
            int32_t expected = 0;
            for (uint32_t b = 0; b < blocks; ++b) {
                const uint8_t *source = raw + ((size_t)row * blocks + b) * Q2_BYTES;
                const int scale = (int)lroundf(weight_scale(source) / unit);
                int dot = 0;
                for (uint32_t k = 0; k < Q2_WIDTH; ++k) {
                    const int c = (source[2 + k / 4] >> (2 * (k % 4))) & 3;
                    dot += (c - 1) * activations[((size_t)t * blocks + b) * Q2_WIDTH + k];
                }
                expected += dot * scale;
            }
            const int32_t actual = output[((size_t)(row / HVX_ROWS) * tokens + t) * HVX_ROWS + row % HVX_ROWS];
            if (actual != expected) {
                fprintf(stderr, "scalar mismatch token=%u row=%u\n", t, row); return 1;
            }
            ++checked;
        }
    }
    printf("full_scalar_outputs_checked=%" PRIu64 "\n", checked);
    return 0;
}

static int stats_expect(struct client *client, uint32_t selected, uint32_t allocated,
                        uint32_t complete, uint64_t bytes) {
    struct n20_resident_stats stats;
    const int status = get_stats(client, &stats);
    if (status || stats.selected_slot != selected || stats.allocated_slots != allocated ||
        stats.complete_slots != complete || stats.allocated_bytes != bytes ||
        stats.limit_bytes != N20_RESIDENT_LIMIT_BYTES || stats.reserved) {
        fprintf(stderr, "resident stats mismatch status=0x%08x\n", (unsigned)status); return 1;
    }
    printf("resident_stats slot=%u allocated=%u complete=%u bytes=%" PRIu64 "\n",
           selected, allocated, complete, bytes);
    return 0;
}

static uint32_t argument(const char *text, uint32_t max) {
    char *end;
    const unsigned long value = strtoul(text, &end, 10);
    return *text && !*end && value && value <= max ? (uint32_t)value : 0;
}

static int read_tensor(const char *path, uint8_t *bytes, size_t size) {
    FILE *file = fopen(path, "rb");
    if (!file) { perror(path); return 1; }
    const int failed = fread(bytes, 1, size, file) != size || ferror(file);
    const int close_failed = fclose(file);
    if (failed || close_failed) { fputs("Cannot read leading tensor rows\n", stderr); return 1; }
    return 0;
}

static struct weight_tile *try_pack_weights(const uint8_t *raw, uint32_t rows,
                                             uint32_t blocks, float *unit) {
    float maximum = 0;
    const size_t count = (size_t)rows * blocks;
    for (size_t i = 0; i < count; ++i) {
        const float scale = weight_scale(raw + i * Q2_BYTES);
        if (!isfinite(scale) || scale < 0) return NULL;
        maximum = fmaxf(maximum, scale);
    }
    const unsigned safe = INT32_MAX / (blocks * 64 * 2 * 127);
    const unsigned limit = safe < UINT16_MAX ? safe : UINT16_MAX;
    *unit = maximum == 0 ? 1 : maximum / limit;
    struct weight_tile *packed = malloc(count / HVX_ROWS * sizeof(*packed));
    if (!packed) return NULL;
    for (uint32_t row = 0; row < rows; ++row) for (uint32_t block = 0; block < blocks; ++block) {
        struct weight_tile *tile = packed + row / HVX_ROWS * blocks + block;
        const uint8_t *source = raw + ((size_t)row * blocks + block) * Q2_BYTES;
        for (unsigned group = 0; group < 4; ++group)
            memcpy(&tile->codes[group][row % HVX_ROWS], source + 2 + 4 * group, 4);
        tile->scales[row % HVX_ROWS] = (uint32_t)lroundf(weight_scale(source) / *unit);
    }
    return packed;
}

/* Separate optional stress pass: each slot is packed and uploaded afresh,
 * although the source alternates between two real matrices. Capture reports
 * contain counts and timings only, never model weights or activations. */
static int many_slots(struct client *client, uint32_t requested, uint8_t *const raw[2],
                      const float unit[2], int32_t *const snapshot[2],
                      struct activation_block *input, uint32_t input_bytes,
                      int32_t *output, uint32_t output_bytes, uint32_t rows,
                      uint32_t columns, uint32_t tokens, uint32_t weight_bytes) {
    const uint32_t blocks = columns / Q2_WIDTH;
    const uint64_t one_bytes = (uint64_t)weight_bytes + HVX_BYTES - 1;
    uint32_t completed = 0, touched = 0;
    uint64_t pack_us = 0, upload_us = 0;
    int status = 0, cleanup_failed = 0;
    if (evict_slot(client, 0) || evict_slot(client, 1) || stats_expect(client, 0, 0, 0, 0)) {
        fputs("Cannot clear original slots before residency stress\n", stderr); return 1;
    }
    const uint64_t begin = now_us();
    for (uint32_t slot = 0; slot < requested; ++slot) {
        float packed_unit;
        const uint64_t pack_start = now_us();
        struct weight_tile *packed = try_pack_weights(raw[slot % 2], rows, blocks, &packed_unit);
        pack_us += now_us() - pack_start;
        if (!packed) { fputs("Matrix packing allocation or data validation failed\n", stderr); status = -1; break; }
        if (memcmp(&packed_unit, &unit[slot % 2], sizeof(packed_unit))) {
            free(packed); fputs("Matrix repacking scale changed\n", stderr); status = -1; break;
        }
        const uint64_t upload_start = now_us();
        status = select_slot(client, slot);
        if (!status) {
            touched = slot + 1;
            status = finish_upload(client, packed, rows, columns, 0, weight_bytes);
        }
        upload_us += now_us() - upload_start;
        free(packed);
        if (status) break;
        ++completed;
    }
    const uint64_t elapsed = now_us() - begin;
    struct n20_resident_stats observed;
    const int stats_status = get_stats(client, &observed);
    printf("resident_many requested=%u completed=%u upload_status=0x%08x stats_status=0x%08x "
           "allocated=%u complete=%u resident_bytes=%" PRIu64 " pack_ms=%.3f upload_ms=%.3f total_ms=%.3f\n",
           requested, completed, (unsigned)status, (unsigned)stats_status,
           observed.allocated_slots, observed.complete_slots, observed.allocated_bytes,
           pack_us / 1000.0, upload_us / 1000.0, elapsed / 1000.0);
    if (status || stats_status || completed != requested || observed.allocated_slots != requested ||
        observed.complete_slots != requested || observed.allocated_bytes != requested * one_bytes ||
        observed.limit_bytes != N20_RESIDENT_LIMIT_BYTES || observed.reserved) {
        fputs("Resident stress incomplete; no success claimed\n", stderr); status = -1;
    }
    if (!status) {
        const uint32_t checks[] = {0, requested / 2, requested - 1};
        for (unsigned i = 0; i < sizeof(checks) / sizeof(checks[0]); ++i) {
            const uint32_t slot = checks[i];
            struct projection_timing timing;
            const uint64_t start = now_us();
            if (select_slot(client, slot) || project(client, input, input_bytes, output, output_bytes, tokens, &timing) ||
                timing.hvx_mode != HVX_MODE_128 || !timing.direct_output || !timing.elapsed_us ||
                memcmp(snapshot[slot % 2], output, output_bytes)) {
                fprintf(stderr, "Resident stress projection mismatch at slot %u\n", slot); status = -1; break;
            }
            printf("resident_many_check slot=%u projection_wall_ms=%.3f dsp_ms=%.3f match=1\n",
                   slot, (now_us() - start) / 1000.0, timing.elapsed_us / 1000.0);
        }
    }
    for (uint32_t slot = 0; slot < touched; ++slot) {
        const int result = evict_slot(client, slot);
        if (result) { fprintf(stderr, "Resident cleanup failed at slot %u: 0x%08x\n", slot, (unsigned)result); cleanup_failed = 1; }
    }
    if (get_stats(client, &observed) || observed.allocated_slots || observed.complete_slots || observed.allocated_bytes) {
        fputs("Resident stress cleanup could not be verified\n", stderr); cleanup_failed = 1;
    }
    printf("resident_many_cleanup attempted=%u passed=%d\n", touched, !cleanup_failed);
    if (!status && !cleanup_failed) puts("resident_many_pass=1");
    return status || cleanup_failed;
}

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr, "usage: %s MATRIX0 MATRIX1 ROWS COLUMNS TOKENS\n", argv[0]); return 2;
    }
    const uint32_t rows = argument(argv[3], MAX_ROWS), columns = argument(argv[4], MAX_COLS);
    const uint32_t tokens = argument(argv[5], MAX_TOKENS);
    const char *many_setting = getenv("N20_RESIDENT_MANY");
    const uint32_t many = many_setting ? argument(many_setting, 62) : 0;
    if (!rows || rows % HVX_ROWS || !columns || columns % Q2_WIDTH || !tokens) {
        fputs("Invalid dimensions\n", stderr); return 2;
    }
    if (many_setting && !many) { fputs("N20_RESIDENT_MANY must be an integer in 1..62\n", stderr); return 2; }
    const uint32_t blocks = columns / Q2_WIDTH;
    const size_t raw_bytes = (size_t)rows * blocks * Q2_BYTES;
    const uint32_t weight_bytes = rows / HVX_ROWS * blocks * sizeof(struct weight_tile);
    const uint32_t input_bytes = tokens * blocks * sizeof(struct activation_block);
    const uint32_t output_bytes = tokens * rows * sizeof(int32_t);
    uint8_t *raw[2] = {allocate(raw_bytes), allocate(raw_bytes)};
    if (read_tensor(argv[1], raw[0], raw_bytes) || read_tensor(argv[2], raw[1], raw_bytes)) return 1;
    if (!memcmp(raw[0], raw[1], raw_bytes)) { fputs("Matrices must be different\n", stderr); return 1; }
    float unit[2];
    struct weight_tile *weights[2] = {pack_weights(raw[0], rows, blocks, &unit[0]),
                                      pack_weights(raw[1], rows, blocks, &unit[1])};
    float *exact = allocate((size_t)tokens * columns * sizeof(*exact));
    int8_t *quantized = allocate((size_t)tokens * columns);
    struct activation_block *host_input = allocate(input_bytes);
    make_activations(exact, quantized, host_input, tokens * blocks);
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
    struct client client = {.handle = 0, .invoke = invoke};
    if (!status) status = open_rpc("file:///libn20_projection_skel.so?"
                                   "n20_projection_skel_handle_invoke&_modver=1.0&_dom=cdsp", &client.handle);
    if (status) { fprintf(stderr, "FastRPC setup: 0x%08x\n", (unsigned)status); return 1; }
    struct activation_block *input = rpc_alloc(25, 0, (int)input_bytes);
    int32_t *output = rpc_alloc(25, 0, (int)output_bytes);
    int32_t *snapshot[2] = {allocate(output_bytes), allocate(output_bytes)};
    if (!input || !output || rpc_fd(input) < 0 || rpc_fd(output) < 0 || ((uintptr_t)output % HVX_BYTES)) {
        fputs("ION allocation or alignment failed\n", stderr); return 1;
    }
    memcpy(input, host_input, input_bytes);
    struct projection_timing timing;
    const uint32_t first = weight_bytes < MAX_UPLOAD ? weight_bytes : MAX_UPLOAD;
    const uint64_t one_bytes = (uint64_t)weight_bytes + HVX_BYTES - 1;
    if (select_slot(&client, 0) || upload_chunk(&client, weights[0], rows, columns, 0, first)) {
        fputs("Slot zero initial upload failed\n", stderr); return 1;
    }
    /* Use a genuinely incomplete upload even for the smallest weight tensor. */
    if (first < weight_bytes) {
        if (!project(&client, input, input_bytes, output, output_bytes, tokens, &timing) ||
            stats_expect(&client, 0, 1, 0, one_bytes)) {
            fputs("Incomplete slot accepted\n", stderr); return 1;
        }
    }
    if (finish_upload(&client, weights[0], rows, columns, first, weight_bytes) ||
        select_slot(&client, 1) || !project(&client, input, input_bytes, output, output_bytes, tokens, &timing) ||
        upload_chunk(&client, weights[1], rows, columns, 0, first) ||
        finish_upload(&client, weights[1], rows, columns, first, weight_bytes) ||
        stats_expect(&client, 1, 2, 2, 2 * one_bytes)) {
        fputs("Resident setup or empty-slot test failed\n", stderr); return 1;
    }
    for (uint32_t slot = 0; slot < 2; ++slot) {
        const uint64_t start = now_us();
        if (select_slot(&client, slot) || project(&client, input, input_bytes, output, output_bytes, tokens, &timing) ||
            timing.hvx_mode != HVX_MODE_128 || !timing.direct_output || !timing.elapsed_us) {
            fprintf(stderr, "Projection failed at slot %u\n", slot); return 1;
        }
        printf("slot=%u projection_wall_ms=%.3f dsp_ms=%.3f\n", slot,
               (now_us() - start) / 1000.0, timing.elapsed_us / 1000.0);
        if (scalar_check(raw[slot], quantized, output, unit[slot], rows, blocks, tokens)) return 1;
        memcpy(snapshot[slot], output, output_bytes);
    }
    if (!memcmp(snapshot[0], snapshot[1], output_bytes)) {
        fputs("Different matrices produced identical test output\n", stderr); return 1;
    }
    for (uint32_t slot = 0; slot < 2; ++slot) {
        if (select_slot(&client, slot) || project(&client, input, input_bytes, output, output_bytes, tokens, &timing) ||
            memcmp(snapshot[slot], output, output_bytes)) {
            fprintf(stderr, "Slot-switch mismatch at slot %u\n", slot); return 1;
        }
    }
    if (evict_slot(&client, 0) || stats_expect(&client, 1, 1, 1, one_bytes) ||
        select_slot(&client, 0) || !project(&client, input, input_bytes, output, output_bytes, tokens, &timing) ||
        upload_chunk(&client, weights[0], rows, columns, 0, first)) {
        fputs("Eviction or reselect test failed\n", stderr); return 1;
    }
    if (first < weight_bytes && (!project(&client, input, input_bytes, output, output_bytes, tokens, &timing) ||
                                 stats_expect(&client, 0, 2, 1, 2 * one_bytes))) {
        fputs("Incomplete reupload accepted\n", stderr); return 1;
    }
    if (finish_upload(&client, weights[0], rows, columns, first, weight_bytes) ||
        stats_expect(&client, 0, 2, 2, 2 * one_bytes) ||
        project(&client, input, input_bytes, output, output_bytes, tokens, &timing) ||
        memcmp(snapshot[0], output, output_bytes) || !select_slot(&client, N20_RESIDENT_SLOTS)) {
        fputs("Reupload or invalid-slot test failed\n", stderr); return 1;
    }
    puts("resident_slot_switch_evict_reupload_invalid_pass=1");
    const int many_failed = many && many_slots(&client, many, raw, unit, snapshot, input, input_bytes,
                                                output, output_bytes, rows, columns, tokens, weight_bytes);
    const int close_failed = close_rpc(client.handle);
    if (close_failed) fputs("FastRPC close failed\n", stderr);
    rpc_free(output); rpc_free(input); dlclose(library);
    free(snapshot[0]); free(snapshot[1]); free(host_input); free(quantized); free(exact);
    free(weights[0]); free(weights[1]); free(raw[0]); free(raw[1]);
    return many_failed || close_failed;
}
