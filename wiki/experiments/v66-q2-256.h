#ifndef N20_V66_Q2_256_H
#define N20_V66_Q2_256_H

/* Scratch-only extended batch limit. All layouts and RPC IDs are unchanged;
 * use this header consistently for both client and 256-token DSP module. */

#include <stddef.h>
#include <stdint.h>

enum {
    Q2_WIDTH = 64,
    Q2_BYTES = 18,
    HVX_ROWS = 32,
    HVX_BYTES = 128,
    HVX_MODE_128 = 1,
    MAX_ROWS = 16384,
    MAX_COLS = 16384,
    MAX_TOKENS = 256,
    MAX_UPLOAD = 256 * 1024,
    RPC_OPEN = 0x00020001,
    RPC_CLOSE = 0x01000010,
    RPC_UPLOAD = 0x02020000,
    RPC_PROJECT = 0x03020200,
    RPC_TOP1 = 0x04020200,
    RPC_FWHT = 0x05010200,
    RPC_FWHT_BOUNDED = 0x06010200,
    FWHT_WIDTH = 512,
    FWHT_MAX_ROWS = 4096,
    FWHT_INPUT_OUT_OF_RANGE = 1,
};

/* Each four packed bytes hold 16 codes for one row. The low half of
   each scale word holds that row's unsigned fixed-point block scale. */
struct weight_tile {
    uint32_t codes[4][HVX_ROWS];
    uint32_t scales[HVX_ROWS];
};

/* Four input bytes for each of four shifts in each packed weight word. */
struct activation_block {
    uint32_t groups[4][4];
    int32_t sum;
};

struct upload {
    uint32_t rows;
    uint32_t columns;
    uint32_t offset;
};

struct projection_request {
    uint32_t tokens;
};

struct projection_top1 {
    uint32_t row;
    int32_t value;
};

struct projection_timing {
    uint64_t elapsed_us;
    int32_t hvx_mode;
    int32_t hvx_units;
    uint32_t direct_output;
    uint32_t flags;
};

_Static_assert(sizeof(struct projection_timing) == 24, "FastRPC timing layout");

union rpc_arg {
    struct {
        void *data;
        size_t size;
    } buffer;
    uint64_t handle;
};

void v66_q2_project(const struct weight_tile *weights, const struct activation_block *input,
                    int32_t *output, uint32_t tiles, uint32_t blocks, uint32_t tokens);
void v66_fwht512(const int32_t *input, int32_t *output, uint32_t rows);

#endif
