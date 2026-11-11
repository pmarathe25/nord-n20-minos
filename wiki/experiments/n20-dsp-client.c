/* Scratch-only Android client for the opt-in resident-slot FastRPC module. */
#include "n20-dsp-client.h"
#include "v66-q2-resident.h"

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(union rpc_arg) == 16, "FastRPC ARM64 host layout");
_Static_assert(sizeof(struct n20_dsp_timing) == sizeof(struct projection_timing), "timing ABI");

struct slot_info { uint32_t rows, columns; };
struct n20_dsp_client {
    void *library;
    uint64_t handle;
    int (*invoke)(uint64_t, uint32_t, union rpc_arg *);
    int (*close_rpc)(uint64_t);
    void *(*rpc_alloc)(int, uint32_t, int);
    int (*rpc_fd)(void *);
    void (*rpc_free)(void *);
    pthread_mutex_t mutex;
    unsigned buffers;
    struct slot_info slots[N20_DSP_CLIENT_SLOTS];
};

static int select_slot(n20_dsp_client *c, uint32_t slot) {
    struct n20_resident_slot request = {slot};
    union rpc_arg args[] = {{.buffer = {&request, sizeof(request)}}};
    return c->invoke(c->handle, RPC_RESIDENT_SELECT, args);
}

static int evict_slot(n20_dsp_client *c, uint32_t slot) {
    struct n20_resident_slot request = {slot};
    union rpc_arg args[] = {{.buffer = {&request, sizeof(request)}}};
    return c->invoke(c->handle, RPC_RESIDENT_EVICT, args);
}

int n20_dsp_open(n20_dsp_client **out) {
    if (!out) return -EINVAL;
    *out = NULL;
    n20_dsp_client *c = calloc(1, sizeof(*c));
    if (!c) return -ENOMEM;
    if (pthread_mutex_init(&c->mutex, NULL)) { free(c); return -ENOMEM; }
    c->library = dlopen("libcdsprpc.so", RTLD_NOW | RTLD_LOCAL);
    if (!c->library) { pthread_mutex_destroy(&c->mutex); free(c); return -ENOENT; }
    int (*control)(uint32_t, void *, uint32_t) = dlsym(c->library, "remote_session_control");
    int (*open_rpc)(const char *, uint64_t *) = dlsym(c->library, "remote_handle64_open");
    c->invoke = dlsym(c->library, "remote_handle64_invoke");
    c->close_rpc = dlsym(c->library, "remote_handle64_close");
    c->rpc_alloc = dlsym(c->library, "rpcmem_alloc");
    c->rpc_fd = dlsym(c->library, "rpcmem_to_fd");
    c->rpc_free = dlsym(c->library, "rpcmem_free");
    if (!control || !open_rpc || !c->invoke || !c->close_rpc || !c->rpc_alloc || !c->rpc_fd || !c->rpc_free) {
        dlclose(c->library); pthread_mutex_destroy(&c->mutex); free(c); return -ENOSYS;
    }
    int unsigned_module[] = {3, 1};
    int status = control(2, unsigned_module, sizeof(unsigned_module));
    if (!status) status = open_rpc("file:///libn20_projection_skel.so?"
                                   "n20_projection_skel_handle_invoke&_modver=1.0&_dom=cdsp", &c->handle);
    if (status) { dlclose(c->library); pthread_mutex_destroy(&c->mutex); free(c); return status; }
    *out = c;
    return 0;
}

int n20_dsp_close(n20_dsp_client **client) {
    if (!client || !*client) return -EINVAL;
    n20_dsp_client *c = *client;
    pthread_mutex_lock(&c->mutex);
    if (c->buffers) { pthread_mutex_unlock(&c->mutex); return -EBUSY; }
    const int status = c->close_rpc(c->handle);
    pthread_mutex_unlock(&c->mutex);
    if (status) return status;
    dlclose(c->library);
    pthread_mutex_destroy(&c->mutex);
    free(c);
    *client = NULL;
    return 0;
}

static float q2_scale(const uint8_t *block) {
    _Float16 half;
    memcpy(&half, block, sizeof(half));
    return (float)half;
}

int n20_dsp_upload_q2(n20_dsp_client *c, uint32_t slot, const void *raw_data,
                      size_t raw_bytes, uint32_t rows, uint32_t columns, float *unit_out) {
    if (!c || !raw_data || !unit_out || slot >= N20_DSP_CLIENT_SLOTS ||
        !rows || rows > MAX_ROWS || rows % HVX_ROWS ||
        !columns || columns > MAX_COLS || columns % Q2_WIDTH) return -EINVAL;
    const size_t blocks = columns / Q2_WIDTH, count = (size_t)rows * blocks;
    if (raw_bytes != count * Q2_BYTES) return -EINVAL;
    const uint8_t *raw = raw_data;
    float maximum = 0;
    for (size_t i = 0; i < count; ++i) {
        const float scale = q2_scale(raw + i * Q2_BYTES);
        if (!isfinite(scale) || scale < 0) return -ERANGE;
        maximum = fmaxf(maximum, scale);
    }
    const unsigned safe = INT32_MAX / (blocks * Q2_WIDTH * 2 * 127);
    const unsigned limit = safe < UINT16_MAX ? safe : UINT16_MAX;
    if (!limit) return -ERANGE;
    const float unit = maximum == 0 ? 1.0f : maximum / limit;
    if (!isfinite(unit) || unit <= 0) return -ERANGE;
    const size_t packed_bytes = count / HVX_ROWS * sizeof(struct weight_tile);
    if (packed_bytes > UINT32_MAX || packed_bytes + HVX_BYTES - 1 > N20_RESIDENT_LIMIT_BYTES) return -EOVERFLOW;
    struct weight_tile *packed = malloc(packed_bytes);
    if (!packed) return -ENOMEM;
    for (uint32_t row = 0; row < rows; ++row) {
        for (uint32_t block = 0; block < blocks; ++block) {
            const uint8_t *src = raw + ((size_t)row * blocks + block) * Q2_BYTES;
            struct weight_tile *tile = packed + (size_t)(row / HVX_ROWS) * blocks + block;
            for (unsigned group = 0; group < 4; ++group)
                memcpy(&tile->codes[group][row % HVX_ROWS], src + 2 + 4 * group, 4);
            const long scale = lroundf(q2_scale(src) / unit);
            if (scale < 0 || (unsigned long)scale > limit) { free(packed); return -ERANGE; }
            tile->scales[row % HVX_ROWS] = (uint32_t)scale;
        }
    }
    pthread_mutex_lock(&c->mutex);
    c->slots[slot] = (struct slot_info){0};
    int status = select_slot(c, slot);
    for (size_t offset = 0; !status && offset < packed_bytes;) {
        const size_t remaining = packed_bytes - offset;
        const size_t bytes = remaining < MAX_UPLOAD ? remaining : MAX_UPLOAD;
        struct upload request = {rows, columns, (uint32_t)offset};
        union rpc_arg args[] = {{.buffer = {&request, sizeof(request)}},
                                {.buffer = {(uint8_t *)packed + offset, bytes}}};
        status = c->invoke(c->handle, RPC_UPLOAD, args);
        offset += bytes;
    }
    if (!status) {
        c->slots[slot] = (struct slot_info){rows, columns};
        *unit_out = unit;
    } else {
        /* Even if eviction fails the local slot stays invalid. */
        (void)evict_slot(c, slot);
    }
    pthread_mutex_unlock(&c->mutex);
    free(packed);
    return status;
}

int n20_dsp_evict(n20_dsp_client *c, uint32_t slot) {
    if (!c || slot >= N20_DSP_CLIENT_SLOTS) return -EINVAL;
    pthread_mutex_lock(&c->mutex);
    c->slots[slot] = (struct slot_info){0};
    const int status = evict_slot(c, slot);
    pthread_mutex_unlock(&c->mutex);
    return status;
}

int n20_dsp_ion_alloc(n20_dsp_client *c, size_t payload, size_t extra,
                      size_t padding, size_t page, struct n20_dsp_buffer *out) {
    if (!c || !out || out->_owner || !payload || !page || (page & (page - 1)) ||
        payload > SIZE_MAX - extra || payload + extra > SIZE_MAX - padding ||
        payload + extra + padding > SIZE_MAX - (page - 1)) return -EINVAL;
    const size_t size = (payload + extra + padding + page - 1) & ~(page - 1);
    if (!size || size > INT_MAX) return -EOVERFLOW;
    pthread_mutex_lock(&c->mutex);
    void *addr = c->rpc_alloc(25, 0, (int)size);
    const int fd = addr ? c->rpc_fd(addr) : -1;
    if (!addr || fd < 0 || (uintptr_t)addr % page || (uintptr_t)addr % HVX_BYTES) {
        if (addr) c->rpc_free(addr);
        pthread_mutex_unlock(&c->mutex);
        return -ENOMEM;
    }
    *out = (struct n20_dsp_buffer){addr, size, payload, fd, c};
    ++c->buffers;
    pthread_mutex_unlock(&c->mutex);
    return 0;
}

int n20_dsp_ion_free(n20_dsp_client *c, struct n20_dsp_buffer *buffer) {
    if (!c || !buffer || buffer->_owner != c || !buffer->addr) return -EINVAL;
    pthread_mutex_lock(&c->mutex);
    c->rpc_free(buffer->addr);
    --c->buffers;
    *buffer = (struct n20_dsp_buffer){0};
    pthread_mutex_unlock(&c->mutex);
    return 0;
}

int n20_dsp_project(n20_dsp_client *c, uint32_t slot, uint32_t tokens,
                    const struct n20_dsp_buffer *input, struct n20_dsp_buffer *output,
                    struct n20_dsp_timing *timing) {
    if (!c || !input || !output || !timing || input == output || slot >= N20_DSP_CLIENT_SLOTS ||
        !tokens || tokens > N20_DSP_CLIENT_MAX_TOKENS || input->_owner != c || output->_owner != c ||
        !input->addr || !output->addr || (uintptr_t)output->addr % HVX_BYTES) return -EINVAL;
    pthread_mutex_lock(&c->mutex);
    const struct slot_info item = c->slots[slot];
    const size_t input_bytes = (size_t)tokens * (item.columns / Q2_WIDTH) * sizeof(struct activation_block);
    const size_t output_bytes = (size_t)tokens * item.rows * sizeof(int32_t);
    if (!item.rows || !item.columns || input->payload < input_bytes || output->payload < output_bytes) {
        pthread_mutex_unlock(&c->mutex); return -EINVAL;
    }
    struct projection_request request = {tokens};
    struct projection_timing result = {0};
    union rpc_arg args[] = {{.buffer = {&request, sizeof(request)}},
                            {.buffer = {input->addr, input_bytes}},
                            {.buffer = {&result, sizeof(result)}},
                            {.buffer = {output->addr, output_bytes}}};
    int status = select_slot(c, slot);
    if (!status) status = c->invoke(c->handle, RPC_PROJECT, args);
    if (!status && (result.hvx_mode != HVX_MODE_128 || !result.direct_output || !result.elapsed_us)) status = -EIO;
    if (!status) memcpy(timing, &result, sizeof(*timing));
    pthread_mutex_unlock(&c->mutex);
    return status;
}
