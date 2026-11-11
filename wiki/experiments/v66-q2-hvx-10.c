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

// Experimental 16-token tile. The eight-dot half tiles reuse weight codes;
// sixteen sums remain live across blocks. This can spill on v66 and must be
// compared with the original eight-token tile on the actual device.
static __attribute__((noinline))
void project_tile16(const struct weight_tile *weights, const struct activation_block *input,
                    HVX_Vector *output, uint32_t blocks) {
    const HVX_Vector mask = Q6_V_vsplat_R(0x03030303);
    HVX_Vector sums[16];
    #pragma clang loop unroll(full)
    for (unsigned i = 0; i < 16; ++i) sums[i] = Q6_V_vsplat_R(0);
    for (uint32_t b = 0; b < blocks; ++b) {
        const HVX_Vector *w = (const HVX_Vector *)(weights + b);
        const HVX_Vector c0 = w[0], c1 = w[1], c2 = w[2], c3 = w[3], scale = w[4];
        #pragma clang loop unroll(full)
        for (unsigned half = 0; half < 2; ++half) {
            HVX_Vector dots[8];
            #pragma clang loop unroll(full)
            for (unsigned i = 0; i < 8; ++i)
                dots[i] = Q6_V_vsplat_R(-input[(half * 8 + i) * blocks + b].sum);
            #pragma clang loop unroll(full)
            for (unsigned group = 0; group < 4; ++group) {
                const HVX_Vector codes = group == 0 ? c0 : group == 1 ? c1 : group == 2 ? c2 : c3;
                #pragma clang loop unroll(full)
                for (unsigned shift = 0; shift < 4; ++shift) {
                    const HVX_Vector plane = Q6_V_vand_VV(Q6_Vuw_vlsr_VuwR(codes, 2 * shift), mask);
                    #pragma clang loop unroll(full)
                    for (unsigned i = 0; i < 8; ++i) {
                        const uint32_t value = input[(half * 8 + i) * blocks + b].groups[group][shift];
                        dots[i] = Q6_Vw_vrmpyacc_VwVubRb(dots[i], plane, value);
                    }
                }
            }
            #pragma clang loop unroll(full)
            for (unsigned i = 0; i < 8; ++i)
                sums[half * 8 + i] = Q6_Vw_vmpyieacc_VwVwVuh(sums[half * 8 + i], dots[i], scale);
        }
    }
    #pragma clang loop unroll(full)
    for (unsigned i = 0; i < 16; ++i) output[i] = sums[i];
}

/* Test ten-token reuse for longer batches, using two five-dot groups to
 * limit the number of simultaneously live dot vectors. */
static __attribute__((noinline))
void project_tile10(const struct weight_tile *weights, const struct activation_block *input,
                    HVX_Vector *output, uint32_t blocks) {
    const HVX_Vector mask = Q6_V_vsplat_R(0x03030303);
    HVX_Vector sums[10];
    #pragma clang loop unroll(full)
    for (unsigned i = 0; i < 10; ++i) sums[i] = Q6_V_vsplat_R(0);
    for (uint32_t b = 0; b < blocks; ++b) {
        const HVX_Vector *w = (const HVX_Vector *)(weights + b);
        const HVX_Vector c0 = w[0], c1 = w[1], c2 = w[2], c3 = w[3], scale = w[4];
        #pragma clang loop unroll(full)
        for (unsigned half = 0; half < 2; ++half) {
            HVX_Vector dots[5];
            #pragma clang loop unroll(full)
            for (unsigned i = 0; i < 5; ++i)
                dots[i] = Q6_V_vsplat_R(-input[(half * 5 + i) * blocks + b].sum);
            #pragma clang loop unroll(full)
            for (unsigned group = 0; group < 4; ++group) {
                const HVX_Vector codes = group == 0 ? c0 : group == 1 ? c1 : group == 2 ? c2 : c3;
                #pragma clang loop unroll(full)
                for (unsigned shift = 0; shift < 4; ++shift) {
                    const HVX_Vector plane = Q6_V_vand_VV(Q6_Vuw_vlsr_VuwR(codes, 2 * shift), mask);
                    #pragma clang loop unroll(full)
                    for (unsigned i = 0; i < 5; ++i) {
                        const uint32_t value = input[(half * 5 + i) * blocks + b].groups[group][shift];
                        dots[i] = Q6_Vw_vrmpyacc_VwVubRb(dots[i], plane, value);
                    }
                }
            }
            #pragma clang loop unroll(full)
            for (unsigned i = 0; i < 5; ++i)
                sums[half * 5 + i] = Q6_Vw_vmpyieacc_VwVwVuh(sums[half * 5 + i], dots[i], scale);
        }
    }
    #pragma clang loop unroll(full)
    for (unsigned i = 0; i < 10; ++i) output[i] = sums[i];
}

void v66_q2_project(const struct weight_tile *weights, const struct activation_block *input,
                    int32_t *output, uint32_t tiles, uint32_t blocks, uint32_t tokens) {
    for (uint32_t tile = 0; tile < tiles; ++tile) {
        const struct weight_tile *matrix_tile = weights + tile * blocks;
        HVX_Vector *destination = (HVX_Vector *)output + tile * tokens;
        uint32_t token = 0;
        if (tokens >= 48) {
            for (; token + 10 <= tokens; token += 10)
                project_tile10(matrix_tile, input + token * blocks, destination + token, blocks);
        }
        // On the tested device the sixteen-token tile helped short batches
        // but lost on long batches. Keep the established path beyond 32.
        if (tokens >= 16 && tokens <= 32) {
            for (; token + 16 <= tokens; token += 16)
                project_tile16(matrix_tile, input + token * blocks, destination + token, blocks);
        }
        for (; token + 8 <= tokens; token += 8)
            project_tile(matrix_tile, input + token * blocks, destination + token, blocks, 8);
        for (; token + 4 <= tokens; token += 4)
            project_tile(matrix_tile, input + token * blocks, destination + token, blocks, 4);
        for (; token < tokens; ++token)
            project_tile(matrix_tile, input + token * blocks, destination + token, blocks, 1);
    }
}
