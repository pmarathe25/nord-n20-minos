// Read one 18-byte GGUF block per row and transpose its codes and scale
// directly into the two arrays consumed by the Adreno Q2_0 kernels.
kernel void kernel_n20_q2_pack(
    global const uchar * raw,
    global uchar * codes,
    global ushort * scales,
    uint rows,
    uint blocks
) {
    const uint x = get_local_id(0);
    const uint y = get_local_id(1);
    const uint block = get_group_id(0);
    const uint row_base = get_group_id(1) * 16;
    const uint source_row = row_base + y;
    local uchar tile[16][17];

    if (source_row < rows) {
        const size_t offset = ((size_t)source_row * blocks + block) * 18;
        tile[y][x] = raw[offset + 2 + x];
    }
    barrier(CLK_LOCAL_MEM_FENCE);

    const uint row = row_base + x;
    if (row < rows) {
        codes[((size_t)block * 16 + y) * rows + row] = tile[x][y];
        if (y == 0) {
            const size_t offset = ((size_t)row * blocks + block) * 18;
            scales[(size_t)block * rows + row] = ((global const ushort *)raw)[offset / 2];
        }
    }
}
