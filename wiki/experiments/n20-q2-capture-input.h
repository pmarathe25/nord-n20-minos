/* Optional input for scratch dynamic projection tests; never prints capture data. */
#ifndef N20_Q2_CAPTURE_INPUT_H
#define N20_Q2_CAPTURE_INPUT_H
#include "n20-q2-capture-format.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* exact is token-major float[tokens][columns]; capture is column-major FP16,
 * with padded_tokens per column. Caller computes its usual quantized reference. */
static int n20_load_capture(const char *path, float *exact, unsigned rows,
                            unsigned columns, unsigned tokens) {
    FILE *file = fopen(path, "rb");
    if (!file) { fputs("Cannot open activation capture\n", stderr); return -1; }
    struct n20_capture_header h;
    if (fread(&h, sizeof(h), 1, file) != 1 || h.magic != N20_CAPTURE_MAGIC ||
        h.version != N20_CAPTURE_VERSION || h.rows != rows || h.columns != columns ||
        h.tokens != tokens || h.padded_tokens != (tokens + 3u) / 4u * 4u ||
        h.start_token > h.source_tokens || tokens > h.source_tokens - h.start_token) {
        fputs("Activation capture header mismatch\n", stderr); fclose(file); return -1;
    }
    _Float16 *column = malloc((size_t)h.padded_tokens * sizeof(*column));
    if (!column) { fclose(file); return -1; }
    int result = 0;
    for (unsigned c = 0; c < columns && !result; ++c) {
        if (fread(column, sizeof(*column), h.padded_tokens, file) != h.padded_tokens) {
            result = -1; break;
        }
        for (unsigned t = 0; t < tokens; ++t) {
            const float value = (float)column[t];
            if (!isfinite(value)) { result = -1; break; }
            exact[(size_t)t * columns + c] = value;
        }
    }
    if (result || fgetc(file) != EOF || ferror(file)) result = -1;
    if (fclose(file)) result = -1;
    free(column);
    if (result) fputs("Activation capture is incomplete or has nonfinite values\n", stderr);
    return result;
}
#endif
