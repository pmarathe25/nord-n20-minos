/* Experimental Android/Bionic FastRPC client for the resident v66 module.
 * This API does not start CDSP, create device nodes, or install firmware. */
#ifndef N20_DSP_CLIENT_H
#define N20_DSP_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { N20_DSP_CLIENT_SLOTS = 96, N20_DSP_CLIENT_MAX_TOKENS = 128 };
typedef struct n20_dsp_client n20_dsp_client;

/* Noncopyable by convention: initialize with {0}, keep the client alive, and
 * release exactly once after all OpenCL objects and queued work are finished. */
struct n20_dsp_buffer {
    void *addr;
    size_t size;      /* allocated bytes, including alignment and padding */
    size_t payload;   /* logical bytes available to the caller */
    int fd;
    void *_owner;
};

struct n20_dsp_timing {
    uint64_t elapsed_us;
    int32_t hvx_mode;
    int32_t hvx_units;
    uint32_t direct_output;
    uint32_t flags;
};

/* Return 0 on success, negative errno for local failures, otherwise the raw
 * nonzero FastRPC status. open returns NULL on failure via *out. Functions on
 * a live client are serialized, but caller must exclude close/free from other
 * uses of the same client or buffer. close refuses outstanding ION buffers. */
int n20_dsp_open(n20_dsp_client **out);
int n20_dsp_close(n20_dsp_client **client);

/* raw contains exactly the leading rows of a row-major Q2_0 matrix, 18 bytes
 * per 64 columns. Slots are owned by this client. Invalid input or a packing
 * failure leaves the old slot intact; once an upload starts the slot is marked
 * invalid, and remains invalid if that upload fails. Always check the return
 * value before using the returned scale or projecting the new matrix. */
int n20_dsp_upload_q2(n20_dsp_client *client, uint32_t slot,
                      const void *raw, size_t raw_bytes,
                      uint32_t rows, uint32_t columns, float *unit);
int n20_dsp_evict(n20_dsp_client *client, uint32_t slot);

/* Allocate an rpcmem/ION buffer suitable for CL_MEM_ION_HOST_PTR_QCOM.
 * page and padding must be the OpenCL device's reported values. extra gives
 * bytes beyond the logical payload, e.g. 128 for the input guard. No OpenCL
 * object is created here. GPU writes must complete before invoking FastRPC;
 * DSP writes must complete before GPU use. CPU access to a CL-imported buffer
 * requires the OpenCL extension's map/unmap protocol. */
int n20_dsp_ion_alloc(n20_dsp_client *client, size_t payload, size_t extra,
                      size_t padding, size_t page, struct n20_dsp_buffer *out);
int n20_dsp_ion_free(n20_dsp_client *client, struct n20_dsp_buffer *buffer);

/* Input is token-major activation_block layout, output is [row tile][token][32]
 * signed int32, with byte sizes determined by the uploaded matrix. Both must
 * be buffers allocated by this client; output must be HVX-aligned. This call
 * is synchronous and includes select+projection; nonzero means callers must
 * not consume the DSP result and should use the complete GPU fallback. */
int n20_dsp_project(n20_dsp_client *client, uint32_t slot, uint32_t tokens,
                    const struct n20_dsp_buffer *input,
                    struct n20_dsp_buffer *output, struct n20_dsp_timing *timing);

#ifdef __cplusplus
}
#endif
#endif
