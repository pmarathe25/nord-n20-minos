#include "v66-q2.h"
#include <hexagon_types.h>
#include <hvx_hexagon_protos.h>

_Static_assert(sizeof(HVX_Vector) == HVX_BYTES, "requires 128-byte HVX vectors");

static const union {
    HVX_Vector vector;
    uint32_t words[32];
} lane_indices = {
    .words = {
         0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
        16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    },
};

void v66_fwht512(const int32_t *input, int32_t *output, uint32_t rows) {
    const HVX_Vector *src = (const HVX_Vector *)input;
    HVX_Vector *dst = (HVX_Vector *)output;
    const HVX_Vector index = lane_indices.vector;
    const HVX_Vector zero = Q6_V_vsplat_R(0);
    for (uint32_t row = 0; row < rows; ++row) {
        HVX_Vector x[16];
#pragma clang loop unroll(full)
        for (unsigned i = 0; i < 16; ++i)
            x[i] = src[row * 16 + i];

        for (unsigned step = 1; step < 32; step <<= 1) {
            const HVX_Vector controls = Q6_V_vsplat_R(0x04040404 * step);
            const HVX_Vector bit = Q6_V_vand_VV(index, Q6_V_vsplat_R(step));
            const HVX_VectorPred low = Q6_Q_vcmp_eq_VwVw(bit, zero);
#pragma clang loop unroll(full)
            for (unsigned i = 0; i < 16; ++i) {
                const HVX_Vector other = Q6_V_vdelta_VV(x[i], controls);
                const HVX_Vector sum = Q6_Vw_vadd_VwVw(x[i], other);
                const HVX_Vector diff = Q6_Vw_vsub_VwVw(other, x[i]);
                x[i] = Q6_V_vmux_QVV(low, sum, diff);
            }
        }

        #define STAGE(step) \
            _Pragma("clang loop unroll(full)") \
            for (unsigned i = 0; i < 8; ++i) { \
                const unsigned lo = (i / (step)) * (2 * (step)) + i % (step); \
                const unsigned hi = lo + (step); \
                const HVX_Vector a = x[lo], b = x[hi]; \
                x[lo] = Q6_Vw_vadd_VwVw(a, b); \
                x[hi] = Q6_Vw_vsub_VwVw(a, b); \
            }
        STAGE(1);
        STAGE(2);
        STAGE(4);
        STAGE(8);
        #undef STAGE

#pragma clang loop unroll(full)
        for (unsigned i = 0; i < 16; ++i)
            dst[row * 16 + i] = x[i];
    }
}
