#include "v66-q2.h"
#include <hexagon_types.h>
#include <hvx_hexagon_protos.h>

_Static_assert(sizeof(HVX_Vector) == HVX_BYTES, "requires 128-byte HVX vectors");
_Static_assert(sizeof(struct weight_tile) == 5 * HVX_BYTES, "packed weight layout");

/* The caller specializes count to 1, 4, or 8 so accumulators stay in registers. */
static inline __attribute__((always_inline))
void project_tile(const struct weight_tile *weights, const struct activation_block *input,
                  HVX_Vector *output, uint32_t blocks, unsigned count) {
    const HVX_Vector mask = Q6_V_vsplat_R(0x03030303);
    HVX_Vector sums[8];
#pragma clang loop unroll(full)
    for (unsigned i = 0; i < 8; ++i)
        if (i < count)
            sums[i] = Q6_V_vsplat_R(0);

    for (uint32_t block = 0; block < blocks; ++block) {
        const HVX_Vector *w = (const HVX_Vector *)(weights + block);
        HVX_Vector dots[8];
#pragma clang loop unroll(full)
        for (unsigned i = 0; i < 8; ++i)
            if (i < count)
                dots[i] = Q6_V_vsplat_R(-input[i * blocks + block].sum);

        for (unsigned group = 0; group < 4; ++group) {
            const HVX_Vector codes = w[group];
#pragma clang loop unroll(full)
            for (unsigned shift = 0; shift < 4; ++shift) {
                const HVX_Vector plane = Q6_V_vand_VV(Q6_Vuw_vlsr_VuwR(codes, 2 * shift), mask);
#pragma clang loop unroll(full)
                for (unsigned i = 0; i < 8; ++i) {
                    if (i < count) {
                        const uint32_t value = input[i * blocks + block].groups[group][shift];
                        dots[i] = Q6_Vw_vrmpyacc_VwVubRb(dots[i], plane, value);
                    }
                }
            }
        }
#pragma clang loop unroll(full)
        for (unsigned i = 0; i < 8; ++i)
            if (i < count)
                sums[i] = Q6_Vw_vmpyieacc_VwVwVuh(sums[i], dots[i], w[4]);
    }
#pragma clang loop unroll(full)
    for (unsigned i = 0; i < 8; ++i)
        if (i < count)
            output[i] = sums[i];
}

void v66_q2_project(const struct weight_tile *weights, const struct activation_block *input,
                    int32_t *output, uint32_t tiles, uint32_t blocks, uint32_t tokens) {
    for (uint32_t tile = 0; tile < tiles; ++tile) {
        const struct weight_tile *matrix_tile = weights + tile * blocks;
        HVX_Vector *destination = (HVX_Vector *)output + tile * tokens;
        uint32_t token = 0;
        for (; token + 8 <= tokens; token += 8)
            project_tile(matrix_tile, input + token * blocks, destination + token, blocks, 8);
        for (; token + 4 <= tokens; token += 4)
            project_tile(matrix_tile, input + token * blocks, destination + token, blocks, 4);
        for (; token < tokens; ++token)
            project_tile(matrix_tile, input + token * blocks, destination + token, blocks, 1);
    }
}
