/* Scratch resident-module dispatcher; both implementations share the ABI. */
#include "v66-q2.h"

extern void v66_q2_project_up(const struct weight_tile *, const struct activation_block *,
                               int32_t *, uint32_t, uint32_t, uint32_t);
extern void v66_q2_project_down(const struct weight_tile *, const struct activation_block *,
                                 int32_t *, uint32_t, uint32_t, uint32_t);

void v66_q2_project(const struct weight_tile *weights, const struct activation_block *input,
                    int32_t *output, uint32_t tiles, uint32_t blocks, uint32_t tokens) {
    if (blocks == 144 && tiles == 16)
        v66_q2_project_down(weights, input, output, tiles, blocks, tokens);
    else
        v66_q2_project_up(weights, input, output, tiles, blocks, tokens);
}
