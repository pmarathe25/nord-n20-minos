/* Compiled without HVX so the dispatcher can acquire a context before vector code. */
#include "v66-q2-256.h"

extern void *memcpy(void *destination, const void *source, size_t bytes);
extern void *qurt_malloc(size_t size);
extern void qurt_free(void *memory);
extern int qurt_hvx_lock(int mode);
extern int qurt_hvx_unlock(void);
extern int qurt_hvx_get_mode(void);
extern int qurt_hvx_get_units(void);
extern uint64_t HAP_perf_get_time_us(void);

_Static_assert(sizeof(union rpc_arg) == 8, "Hexagon FastRPC argument layout");

struct matrix {
    struct weight_tile *weights;
    void *memory;
    uint32_t rows;
    uint32_t columns;
    uint32_t uploaded;
    uint32_t size;
};

static void *align_hvx(void *pointer) {
    return (void *)(((uintptr_t)pointer + HVX_BYTES - 1) & ~(uintptr_t)(HVX_BYTES - 1));
}

static int upload(struct matrix *matrix, union rpc_arg *args) {
    if (args[0].buffer.size != sizeof(struct upload) || args[1].buffer.size > MAX_UPLOAD)
        return -1;
    struct upload request;
    memcpy(&request, args[0].buffer.data, sizeof(request));
    if (request.rows == 0 || request.rows > MAX_ROWS || request.rows % HVX_ROWS ||
        request.columns == 0 || request.columns > MAX_COLS || request.columns % Q2_WIDTH)
        return -2;
    const uint32_t size = request.rows / HVX_ROWS * (request.columns / Q2_WIDTH) * sizeof(struct weight_tile);
    if (request.offset == 0) {
        if (matrix->memory)
            qurt_free(matrix->memory);
        matrix->memory = qurt_malloc(size + HVX_BYTES - 1);
        matrix->weights = align_hvx(matrix->memory);
        matrix->rows = request.rows;
        matrix->columns = request.columns;
        matrix->size = size;
        matrix->uploaded = 0;
    }
    if (!matrix->memory || request.rows != matrix->rows || request.columns != matrix->columns ||
        request.offset != matrix->uploaded || args[1].buffer.size > matrix->size - matrix->uploaded)
        return -3;
    memcpy((uint8_t *)matrix->weights + matrix->uploaded, args[1].buffer.data, args[1].buffer.size);
    matrix->uploaded += args[1].buffer.size;
    return 0;
}

static int project(struct matrix *matrix, union rpc_arg *args, int top1) {
    if (!matrix->memory || matrix->uploaded != matrix->size ||
        args[0].buffer.size != sizeof(struct projection_request) ||
        args[2].buffer.size != sizeof(struct projection_timing))
        return -1;
    struct projection_request request;
    memcpy(&request, args[0].buffer.data, sizeof(request));
    const uint32_t blocks = matrix->columns / Q2_WIDTH;
    const size_t out_size = request.tokens * (size_t)matrix->rows * sizeof(int32_t);
    if (!request.tokens || request.tokens > MAX_TOKENS ||
        args[1].buffer.size != request.tokens * blocks * sizeof(struct activation_block) ||
        (top1 && request.tokens != 1) ||
        args[3].buffer.size != (top1 ? sizeof(struct projection_top1) : out_size))
        return -2;

    int32_t *output = args[3].buffer.data;
    void *scratch = NULL;
    if (top1 || ((uintptr_t)output & (HVX_BYTES - 1))) {
        scratch = qurt_malloc(out_size + HVX_BYTES - 1);
        if (!scratch)
            return -3;
        output = align_hvx(scratch);
    }
    const int lock = qurt_hvx_lock(HVX_MODE_128);
    if (lock) {
        if (scratch)
            qurt_free(scratch);
        return 0x10000 | (lock & 0xffff);
    }
    struct projection_timing timing = {
        .hvx_mode = qurt_hvx_get_mode(), .hvx_units = qurt_hvx_get_units(),
        .direct_output = scratch == NULL,
    };
    if (timing.hvx_mode == HVX_MODE_128) {
        const uint64_t start = HAP_perf_get_time_us();
        /* Bound the token working set to 128 while retaining one RPC and
         * canonical [row tile][token][row] output. The input is token-major. */
        if (request.tokens > 128) {
            const struct activation_block *input = args[1].buffer.data;
            const uint32_t tiles = matrix->rows / HVX_ROWS;
            for (uint32_t tile = 0; tile < tiles; ++tile) {
                const struct weight_tile *weights = matrix->weights + (size_t)tile * blocks;
                int32_t *destination = output + (size_t)tile * request.tokens * HVX_ROWS;
                for (uint32_t first = 0; first < request.tokens; first += 128) {
                    const uint32_t remaining = request.tokens - first;
                    const uint32_t count = remaining < 128 ? remaining : 128;
                    v66_q2_project(weights, input + (size_t)first * blocks,
                                   destination + (size_t)first * HVX_ROWS, 1, blocks, count);
                }
            }
        } else {
            v66_q2_project(matrix->weights, args[1].buffer.data, output,
                           matrix->rows / HVX_ROWS, blocks, request.tokens);
        }
        if (top1) {
            struct projection_top1 best = {0, output[0]};
            for (uint32_t row = 1; row < matrix->rows; ++row) {
                if (output[row] > best.value)
                    best = (struct projection_top1){row, output[row]};
            }
            memcpy(args[3].buffer.data, &best, sizeof(best));
        }
        timing.elapsed_us = HAP_perf_get_time_us() - start;
    }
    const int unlock = qurt_hvx_unlock();
    memcpy(args[2].buffer.data, &timing, sizeof(timing));
    if (scratch) {
        if (!top1 && timing.hvx_mode == HVX_MODE_128 && !unlock)
            memcpy(args[3].buffer.data, output, out_size);
        qurt_free(scratch);
    }
    return unlock ? -4 : 0;
}

static int fwht(union rpc_arg *args, int bounded) {
    const size_t bytes = args[2].buffer.size;
    const size_t stride = FWHT_WIDTH * sizeof(int32_t);
    if (!bytes || bytes % stride || bytes / stride > FWHT_MAX_ROWS ||
        args[1].buffer.size != sizeof(struct projection_timing) ||
        args[0].buffer.size != bytes + (bounded ? HVX_BYTES : 0))
        return -1;
    if (bounded) {
        uint32_t overflow;
        memcpy(&overflow, (const uint8_t *)args[0].buffer.data + bytes, sizeof(overflow));
        if (overflow) {
            const struct projection_timing timing = {.flags = FWHT_INPUT_OUT_OF_RANGE};
            memcpy(args[1].buffer.data, &timing, sizeof(timing));
            return 0;
        }
    }

    const int32_t *input = args[0].buffer.data;
    int32_t *output = args[2].buffer.data;
    void *input_copy = NULL, *output_copy = NULL;
    int status = -2;
    if ((uintptr_t)input & (HVX_BYTES - 1)) {
        input_copy = qurt_malloc(bytes + HVX_BYTES - 1);
        if (!input_copy)
            goto finish;
        input = memcpy(align_hvx(input_copy), input, bytes);
    }
    if ((uintptr_t)output & (HVX_BYTES - 1)) {
        output_copy = qurt_malloc(bytes + HVX_BYTES - 1);
        if (!output_copy)
            goto finish;
        output = align_hvx(output_copy);
    }

    const int lock = qurt_hvx_lock(HVX_MODE_128);
    if (lock) {
        status = 0x10000 | (lock & 0xffff);
        goto finish;
    }
    struct projection_timing timing = {
        .hvx_mode = qurt_hvx_get_mode(), .hvx_units = qurt_hvx_get_units(),
        .direct_output = output_copy == NULL,
    };
    if (timing.hvx_mode == HVX_MODE_128) {
        const uint64_t start = HAP_perf_get_time_us();
        v66_fwht512(input, output, bytes / stride);
        timing.elapsed_us = HAP_perf_get_time_us() - start;
    }
    status = timing.hvx_mode == HVX_MODE_128 ? 0 : -4;
    if (qurt_hvx_unlock())
        status = -3;
    if (output_copy && !status)
        memcpy(args[2].buffer.data, output, bytes);
    memcpy(args[1].buffer.data, &timing, sizeof(timing));
finish:
    if (input_copy)
        qurt_free(input_copy);
    if (output_copy)
        qurt_free(output_copy);
    return status;
}

int n20_projection_skel_handle_invoke(uint64_t handle, uint32_t call, union rpc_arg *args) {
    struct matrix *matrix = (struct matrix *)(uintptr_t)handle;
    switch (call) {
    case RPC_OPEN:
        matrix = qurt_malloc(sizeof(*matrix));
        if (!matrix)
            return -1;
        *matrix = (struct matrix){0};
        args[2].handle = (uintptr_t)matrix;
        return 0;
    case RPC_CLOSE:
        matrix = (struct matrix *)(uintptr_t)args[0].handle;
        if (matrix->memory)
            qurt_free(matrix->memory);
        qurt_free(matrix);
        return 0;
    case RPC_UPLOAD:
        return upload(matrix, args);
    case RPC_PROJECT:
        return project(matrix, args, 0);
    case RPC_TOP1:
        return project(matrix, args, 1);
    case RPC_FWHT:
        return fwht(args, 0);
    case RPC_FWHT_BOUNDED:
        return fwht(args, 1);
    default:
        return -1;
    }
}
