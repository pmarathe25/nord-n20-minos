/* Scratch-only Android OpenCL capture. Captures can contain private prompts;
 * output is mode 0600, created exclusively, and must be deleted after use.
 * LD_PRELOAD this before cl-compat.so. It blocks the queue for one image read,
 * so it is not suitable for timing or production use. No values are logged. */
#define _GNU_SOURCE
#define CL_TARGET_OPENCL_VERSION 200
#include <CL/cl.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "n20-q2-capture-format.h"

enum { MAX_KERNELS = 64, MAX_CAPTURE_TOKENS = 256, MAX_CAPTURE_COLUMNS = 16384 };
struct gemm_args { cl_kernel kernel; cl_mem image; int rows, padded, columns, tokens; unsigned seen; };
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct gemm_args entries[MAX_KERNELS];
static _Thread_local int inside;
static unsigned seen_copies;
static int capture_done;

typedef cl_int (*set_arg_fn)(cl_kernel, cl_uint, size_t, const void *);
typedef cl_int (*copy_fn)(cl_command_queue, cl_mem, cl_mem, size_t, const size_t *, const size_t *,
                          cl_uint, const cl_event *, cl_event *);
typedef cl_int (*release_kernel_fn)(cl_kernel);
static set_arg_fn next_set_arg;
static copy_fn next_copy;
static release_kernel_fn next_release_kernel;
static pthread_once_t set_once = PTHREAD_ONCE_INIT, copy_once = PTHREAD_ONCE_INIT, release_once = PTHREAD_ONCE_INIT;

static void *symbol(const char *name) {
    void *result = dlsym(RTLD_NEXT, name);
    if (!result) { fprintf(stderr, "N20_CAPTURE_ERROR missing_symbol=%s\n", name); abort(); }
    return result;
}
static void init_set(void) { next_set_arg = (set_arg_fn)symbol("clSetKernelArg"); }
static void init_copy(void) { next_copy = (copy_fn)symbol("clEnqueueCopyBufferToImage"); }
static void init_release(void) { next_release_kernel = (release_kernel_fn)symbol("clReleaseKernel"); }

static int matching_kernel(cl_kernel kernel) {
    char name[128]; size_t length = 0;
    return clGetKernelInfo(kernel, CL_KERNEL_FUNCTION_NAME, sizeof(name), name, &length) == CL_SUCCESS &&
           length && length <= sizeof(name) && !strcmp(name, "kernel_gemm_noshuffle_q2_0_f32");
}

CL_API_ENTRY cl_int CL_API_CALL clSetKernelArg(cl_kernel kernel, cl_uint index, size_t size, const void *value) {
    pthread_once(&set_once, init_set);
    const cl_int status = next_set_arg(kernel, index, size, value);
    if (inside || status != CL_SUCCESS || !value || (index != 2 && (index < 5 || index > 8))) return status;
    if ((index == 2 && size != sizeof(cl_mem)) || (index != 2 && size != sizeof(int))) return status;
    inside = 1;
    pthread_mutex_lock(&lock);
    struct gemm_args *entry = NULL, *empty = NULL;
    for (unsigned i = 0; i < MAX_KERNELS; ++i) {
        if (entries[i].kernel == kernel) { entry = &entries[i]; break; }
        if (!entries[i].kernel && !empty) empty = &entries[i];
    }
    if (!entry && empty && matching_kernel(kernel)) { entry = empty; entry->kernel = kernel; }
    if (entry) {
        if (index == 2) memcpy(&entry->image, value, sizeof(entry->image));
        else if (index == 5) memcpy(&entry->rows, value, sizeof(entry->rows));
        else if (index == 6) memcpy(&entry->padded, value, sizeof(entry->padded));
        else if (index == 7) memcpy(&entry->columns, value, sizeof(entry->columns));
        else memcpy(&entry->tokens, value, sizeof(entry->tokens));
        entry->seen |= 1u << index;
    }
    pthread_mutex_unlock(&lock);
    inside = 0;
    return status;
}

CL_API_ENTRY cl_int CL_API_CALL clReleaseKernel(cl_kernel kernel) {
    pthread_once(&release_once, init_release);
    if (!inside) {
        pthread_mutex_lock(&lock);
        for (unsigned i = 0; i < MAX_KERNELS; ++i)
            if (entries[i].kernel == kernel) memset(&entries[i], 0, sizeof(entries[i]));
        pthread_mutex_unlock(&lock);
    }
    return next_release_kernel(kernel);
}

static int number(const char *key, unsigned fallback, unsigned limit, unsigned *out) {
    const char *text = getenv(key); char *end;
    if (!text) { *out = fallback; return 1; }
    errno = 0; const unsigned long value = strtoul(text, &end, 10);
    if (errno || !*text || *end || value > limit) return 0;
    *out = (unsigned)value; return 1;
}

static int write_all(int fd, const void *data, size_t size) {
    const unsigned char *p = data;
    while (size) {
        const ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        p += n; size -= (size_t)n;
    }
    return 1;
}

static int capture(cl_command_queue queue, cl_mem image, const struct gemm_args *e,
                   const char *path, unsigned start, unsigned tokens) {
    cl_command_queue_properties properties = 0;
    cl_image_format format;
    if (clGetCommandQueueInfo(queue, CL_QUEUE_PROPERTIES, sizeof(properties), &properties, NULL) != CL_SUCCESS ||
        (properties & CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE) ||
        clGetImageInfo(image, CL_IMAGE_FORMAT, sizeof(format), &format, NULL) != CL_SUCCESS ||
        format.image_channel_order != CL_RGBA || format.image_channel_data_type != CL_HALF_FLOAT) {
        fputs("N20_CAPTURE_ERROR unsupported_queue_or_image\n", stderr); return 0;
    }
    const size_t padded = (tokens + 3u) / 4u * 4u;
    const size_t bytes = (size_t)e->columns * padded * sizeof(uint16_t);
    uint16_t *data = malloc(bytes);
    if (!data) { fputs("N20_CAPTURE_ERROR allocation\n", stderr); return 0; }
    const size_t origin[] = {start / 4u, 0, 0}, region[] = {padded / 4u, (size_t)e->columns, 1};
    const cl_int status = clEnqueueReadImage(queue, image, CL_TRUE, origin, region, 0, 0, data, 0, NULL, NULL);
    if (status != CL_SUCCESS) {
        fprintf(stderr, "N20_CAPTURE_ERROR read_image=%d\n", status); free(data); return 0;
    }
    const struct n20_capture_header header = {N20_CAPTURE_MAGIC, N20_CAPTURE_VERSION,
        (uint32_t)e->rows, (uint32_t)e->columns, (uint32_t)e->tokens, start, tokens, (uint32_t)padded};
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { fputs("N20_CAPTURE_ERROR create\n", stderr); free(data); return 0; }
    const int wrote = write_all(fd, &header, sizeof(header)) && write_all(fd, data, bytes);
    const int closed = close(fd) == 0;
    free(data);
    if (!wrote || !closed) {
        unlink(path); fputs("N20_CAPTURE_ERROR write\n", stderr); return 0;
    }
    fprintf(stderr, "N20_CAPTURE_OK rows=%d columns=%d source_tokens=%d start=%u tokens=%u\n",
            e->rows, e->columns, e->tokens, start, tokens);
    return 1;
}

CL_API_ENTRY cl_int CL_API_CALL clEnqueueCopyBufferToImage(
    cl_command_queue queue, cl_mem source, cl_mem image, size_t offset, const size_t *origin,
    const size_t *region, cl_uint nwait, const cl_event *waits, cl_event *event) {
    pthread_once(&copy_once, init_copy);
    const cl_int status = next_copy(queue, source, image, offset, origin, region, nwait, waits, event);
    const char *path = getenv("N20_CAPTURE_PATH");
    if (inside || status != CL_SUCCESS || !path || !*path || !origin || !region) return status;
    unsigned rows, columns, start, tokens, index;
    if (!number("N20_CAPTURE_ROWS", 9216, 16384, &rows) || !number("N20_CAPTURE_COLUMNS", 2560, MAX_CAPTURE_COLUMNS, &columns) ||
        !number("N20_CAPTURE_START", 0, MAX_CAPTURE_TOKENS, &start) || !number("N20_CAPTURE_TOKENS", 128, MAX_CAPTURE_TOKENS, &tokens) ||
        !number("N20_CAPTURE_INDEX", 0, UINT32_MAX, &index) || !tokens || start % 4) {
        fputs("N20_CAPTURE_ERROR bad_configuration\n", stderr); return status;
    }
    const unsigned needed = (1u << 2) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8);
    inside = 1;
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; !capture_done && i < MAX_KERNELS; ++i) {
        const struct gemm_args *e = &entries[i];
        if (e->kernel && e->image == image && (e->seen & needed) == needed &&
            e->rows == (int)rows && e->columns == (int)columns && e->tokens >= 0 &&
            (unsigned)e->tokens >= start && tokens <= (unsigned)e->tokens - start &&
            e->padded >= e->tokens && e->padded % 4 == 0 &&
            region[0] == (size_t)e->padded / 4 && region[1] == columns && region[2] == 1 &&
            origin[0] == 0 && origin[1] == 0 && origin[2] == 0 && offset == 0) {
            if (seen_copies++ == index) {
                capture_done = 1;
                capture(queue, image, e, path, start, tokens);
            }
            break;
        }
    }
    pthread_mutex_unlock(&lock);
    inside = 0;
    return status;
}
