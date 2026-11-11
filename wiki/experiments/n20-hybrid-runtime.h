#ifndef N20_HYBRID_RUNTIME_H
#define N20_HYBRID_RUNTIME_H

/* Scratch-only 128-token GPU/Hexagon projection bridge. A false result leaves
 * the caller responsible for queuing the complete GPU fallback. Calls and
 * teardown must be serialized on the queue-owning backend thread. */
#include <CL/cl.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef struct n20_hybrid n20_hybrid;
int n20_hybrid_open(n20_hybrid **out, cl_context context, cl_device_id device,
                    cl_command_queue queue, const char *row_kernel_path,
                    const char *aux_kernel_path);
void n20_hybrid_close(n20_hybrid **state);
int n20_hybrid_upload(n20_hybrid *state, const void *raw, size_t raw_size,
                      uint32_t *slot, float *unit);
int n20_hybrid_project(n20_hybrid *state, uint32_t slot, float unit, unsigned tokens,
                       unsigned padded, cl_mem transposed, cl_mem texture,
                       cl_mem codes, cl_mem scales, cl_mem output, cl_ulong offset);
#ifdef __cplusplus
}
#endif
#endif
