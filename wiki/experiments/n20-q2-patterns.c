/* Count reusable ternary patterns within Q2_0's independently scaled groups. */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { BLOCK = 64, BYTES = 18, MAX_GROUP = 5, PATTERNS = 1024 };

static unsigned weight(const unsigned char *data, size_t blocks,
                       unsigned row, unsigned block, unsigned column) {
    const unsigned char *q = data + ((size_t)row * blocks + block) * BYTES + 2;
    return (q[column / 4] >> (2 * (column % 4))) & 3;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s Q2_FILE ROWS COLUMNS\n", argv[0]);
        return 2;
    }
    char *end;
    const unsigned long rows = strtoul(argv[2], &end, 10);
    if (*end || !rows || rows > 262144) return 2;
    const unsigned long columns = strtoul(argv[3], &end, 10);
    if (*end || !columns || columns > 16384 || columns % BLOCK) return 2;
    const size_t blocks = columns / BLOCK;
    const size_t bytes = (size_t)rows * blocks * BYTES;
    unsigned char *data = malloc(bytes);
    FILE *file = fopen(argv[1], "rb");
    if (!data || !file || fread(data, 1, bytes, file) != bytes || fgetc(file) != EOF) {
        fputs("Cannot read tensor or unexpected length\n", stderr);
        return 1;
    }
    fclose(file);
    uint64_t codes[4] = {0};
    for (unsigned r = 0; r < rows; ++r)
        for (unsigned b = 0; b < blocks; ++b)
            for (unsigned k = 0; k < BLOCK; ++k) ++codes[weight(data, blocks, r, b, k)];
    printf("rows=%lu columns=%lu codes=%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
           rows, columns, codes[0], codes[1], codes[2], codes[3]);
    for (unsigned group = 2; group <= MAX_GROUP; ++group) {
        uint64_t tiles = 0, positions = 0, all_zero = 0, unique = 0;
        uint64_t segment_adds = 0, distribute_adds = 0, direct_adds = 0;
        for (unsigned r = 0; r + group <= rows; r += group) {
            for (unsigned b = 0; b < blocks; ++b) {
                unsigned counts[PATTERNS] = {0};
                for (unsigned k = 0; k < BLOCK; ++k) {
                    unsigned key = 0;
                    for (unsigned j = 0; j < group; ++j) {
                        const unsigned c = weight(data, blocks, r + j, b, k);
                        key |= c << (2 * j);
                        direct_adds += c != 1;
                    }
                    ++counts[key];
                }
                for (unsigned key = 0; key < (1u << (2 * group)); ++key) {
                    if (!counts[key]) continue;
                    unsigned nonzero = 0;
                    for (unsigned j = 0; j < group; ++j)
                        nonzero += ((key >> (2 * j)) & 3) != 1;
                    if (!nonzero) {
                        all_zero += counts[key];
                        continue;
                    }
                    ++unique;
                    positions += counts[key];
                    segment_adds += counts[key] - 1;
                    distribute_adds += nonzero;
                }
                ++tiles;
            }
        }
        printf("group=%u tiles=%" PRIu64 " positions=%" PRIu64 " zero_positions=%" PRIu64
               " patterns=%" PRIu64 " segment_adds=%" PRIu64 " distribute_adds=%" PRIu64
               " direct_nonzero=%" PRIu64 " dense_terms=%" PRIu64 "\n",
               group, tiles, positions, all_zero, unique, segment_adds, distribute_adds,
               direct_adds, tiles * BLOCK * group);
    }
    free(data);
    return 0;
}
