/* Private scratch capture: little-endian uint32 header, then FP16 texture rows.
 * Do not commit or log captures: they contain model activations from user prompts. */
#ifndef N20_Q2_CAPTURE_FORMAT_H
#define N20_Q2_CAPTURE_FORMAT_H
#include <stdint.h>

#define N20_CAPTURE_MAGIC UINT32_C(0x4330324e) /* "N20C" */
#define N20_CAPTURE_VERSION UINT32_C(1)
struct n20_capture_header {
    uint32_t magic, version, rows, columns, source_tokens, start_token, tokens, padded_tokens;
};
_Static_assert(sizeof(struct n20_capture_header) == 32, "capture header layout");
#endif
