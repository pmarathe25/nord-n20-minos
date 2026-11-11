// Compile-only Hexagon v66 probe. Buffers must be aligned to the HVX vector size.
#include <hexagon_types.h>
#include <hvx_hexagon_protos.h>

// Each result word sums four unsigned code bytes times four signed input bytes.
void n20_hvx_dot4(HVX_Vector *restrict sums, const HVX_Vector *restrict codes,
                  int packed_input) {
    *sums = Q6_Vw_vrmpy_VubRb(*codes, packed_input);
}
