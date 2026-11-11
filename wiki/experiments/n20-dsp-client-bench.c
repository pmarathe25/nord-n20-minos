/* Scratch-only smoke test of the public FastRPC client with two Q2_0 tensors. */
#define main n20_original_projection_probe_main
#include "v66-q2-bench.c"
#undef main
#include "n20-dsp-client.h"
#include <errno.h>

int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: %s MATRIX0 MATRIX1 ROWS COLUMNS\n", argv[0]); return 2; }
    const uint32_t rows = (uint32_t)strtoul(argv[3], NULL, 10), cols = (uint32_t)strtoul(argv[4], NULL, 10);
    if (!rows || rows > MAX_ROWS || rows % HVX_ROWS || !cols || cols > MAX_COLS || cols % Q2_WIDTH) return 2;
    const uint32_t blocks = cols / Q2_WIDTH;
    const size_t bytes = (size_t)rows * blocks * Q2_BYTES;
    uint8_t *raw[2] = {allocate(bytes), allocate(bytes)};
    struct weight_tile *weights[2];
    float unit[2], actual_unit[2];
    for (unsigned i = 0; i < 2; ++i) {
        FILE *file = fopen(argv[i + 1], "rb");
        if (!file || fread(raw[i], 1, bytes, file) != bytes || fclose(file)) {
            fputs("Cannot read matrix prefix\n", stderr); return 1;
        }
        weights[i] = pack_weights(raw[i], rows, blocks, &unit[i]);
    }
    if (!memcmp(raw[0], raw[1], bytes)) { fputs("Matrices must differ\n", stderr); return 1; }
    n20_dsp_client *client = NULL;
    struct n20_dsp_buffer input = {0}, output = {0};
    struct n20_dsp_timing timing;
    const size_t input_bytes = (size_t)16 * blocks * sizeof(struct activation_block);
    const size_t output_bytes = (size_t)16 * rows * sizeof(int32_t);
    int status = n20_dsp_open(&client);
    if (status) { fprintf(stderr, "open status=0x%08x\n", (unsigned)status); return 1; }
    if (n20_dsp_ion_alloc(client, input_bytes, HVX_BYTES, 0, 4096, &input) ||
        n20_dsp_ion_alloc(client, output_bytes, 0, 0, 4096, &output)) return 1;
    if (n20_dsp_close(&client) != -EBUSY || !client) { fputs("close busy check failed\n", stderr); return 1; }
    const uint32_t tokens[] = {1, 16};
    for (unsigned i = 0; i < 2; ++i) {
        if (n20_dsp_upload_q2(client, i, raw[i], bytes, rows, cols, &actual_unit[i]) || actual_unit[i] != unit[i]) {
            fputs("resident upload or unit mismatch\n", stderr); return 1;
        }
    }
    for (unsigned t = 0; t < sizeof(tokens)/sizeof(tokens[0]); ++t) {
        const uint32_t count = tokens[t];
        float *exact = allocate((size_t)count * cols * sizeof(*exact));
        int8_t *quantized = allocate((size_t)count * cols);
        make_activations(exact, quantized, input.addr, count * blocks);
        for (unsigned i = 0; i < 2; ++i) {
            status = n20_dsp_project(client, i, count, &input, &output, &timing);
            if (status || verify(raw[i], weights[i], exact, quantized, output.addr, rows, blocks, count, unit[i])) {
                fprintf(stderr, "slot=%u tokens=%u status=0x%08x\n", i, count, (unsigned)status); return 1;
            }
            printf("slot=%u tokens=%u dsp_us=%" PRIu64 " pass=1\n", i, count, timing.elapsed_us);
        }
        free(quantized); free(exact);
    }
    if (n20_dsp_evict(client, 0) || n20_dsp_project(client, 0, 1, &input, &output, &timing) != -EINVAL ||
        n20_dsp_ion_free(client, &output) || n20_dsp_ion_free(client, &input) ||
        n20_dsp_close(&client) || client) {
        fputs("eviction or cleanup failed\n", stderr); return 1;
    }
    for (unsigned i = 0; i < 2; ++i) { free(weights[i]); free(raw[i]); }
    puts("client_smoke_pass=1");
    return 0;
}
