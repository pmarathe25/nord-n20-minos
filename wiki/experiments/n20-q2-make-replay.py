"""Create a scratch real-activation replay variant without editing the live benchmark.

Usage: python n20-q2-make-replay.py DYNAMIC_SOURCE OUTPUT_C
Compile OUTPUT_C with -I pointing to this experiments directory.
Captures are private scratch data and are not benchmark artifacts for Git.
"""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("source", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
text = args.source.read_text(encoding="utf-8")
include = '#define CL_TARGET_OPENCL_VERSION 200\n'
anchor = '    if (!inject_nan && !inject_inf) reference_blocks(exact, maxima, quantized, expected_input, tokens, blocks);\n'
if text.count(include) != 1 or text.count(anchor) != 1:
    raise SystemExit("Dynamic benchmark changed; review the capture hook before regenerating")
text = text.replace(include, include + '#include "n20-q2-capture-input.h"\n', 1)
hook = '''    const char *capture_path = getenv("N20_CAPTURE_INPUT");
    if (!capture_path || !*capture_path || guard_test || edge || extreme || alternate) {
        fputs("Replay requires a capture and no synthetic input flags\\n", stderr); return 2;
    }
    if (n20_load_capture(capture_path, exact, rows, columns, tokens)) return 1;
    /* The dynamic prototype's current safety guard rejects magnitudes >16.
       A future fallback-aware replay can remove this explicit restriction. */
    for (size_t i = 0; i < (size_t)tokens * columns; ++i)
        if (fabsf(exact[i]) > 16.0f) {
            fputs("Capture exceeds prototype magnitude limit\\n", stderr); return 2;
        }
    puts("private_capture_loaded=1");
'''
text = text.replace(anchor, hook + anchor, 1)
strict_128 = '''        if (memcmp(expected_input, observed_input, input_bytes) || memcmp(expected_input, qin, input_bytes)) {
            fputs("GPU-packed input differs from CPU reference or host-visible ION\\n", stderr); return 1;
        }
'''
strict_256 = '''        if (memcmp(expected_input, observed_input, input_bytes) ||
            memcmp(expected_input, qin[0], half_input_bytes) ||
            memcmp((const char *)expected_input + half_input_bytes, qin[1], half_input_bytes)) {
            fputs("GPU-packed input differs from CPU reference or host-visible ION\\n", stderr); return 1;
        }
'''
checked = '''        {
            uint64_t different = 0, bad_sums = 0;
            unsigned max_delta = 0;
            double max_ideal_error = 0;
            for (size_t b = 0; b < (size_t)tokens * blocks; ++b) {
                int sum = 0;
                for (unsigned j = 0; j < Q2_WIDTH; ++j) {
                    const size_t i = b * Q2_WIDTH + j;
                    const int cpu = quantized[i];
                    const int q = (int8_t)(observed_input[b].groups[j/16][j%4] >> (8 * ((j%16)/4)));
                    const unsigned delta = (unsigned)abs(cpu - q);
                    const unsigned token = (unsigned)(b / blocks);
                    const double ideal = maxima[token] ? 127.0 * exact[i] / maxima[token] : 0.0;
                    const double error = fabs(q - ideal);
                    different += delta != 0;
                    if (delta > max_delta) max_delta = delta;
                    if (error > max_ideal_error) max_ideal_error = error;
                    sum += q;
                    quantized[i] = (int8_t)q;
                }
                bad_sums += sum != observed_input[b].sum;
            }
            printf("packing_diagnostic different=%" PRIu64 " max_q_delta=%u max_ideal_q_error=%.9g bad_block_sums=%" PRIu64 " host_ion_match=%d\\n",
                   different, max_delta, max_ideal_error, bad_sums, memcmp(observed_input, qin, input_bytes) == 0);
            if (bad_sums || max_delta > 1 || max_ideal_error > 0.501 || memcmp(observed_input, qin, input_bytes)) return 1;
        }
'''
counts = (text.count(strict_128), text.count(strict_256))
if counts not in ((1, 0), (0, 1)):
    raise SystemExit("Dynamic benchmark validation changed; review replay quantization checks")
if counts == (0, 1):
    # observed_input is the concatenation of both RPC input halves. Both host
    # ION mappings must match their corresponding GPU-read half byte for byte.
    match = '(memcmp(observed_input, qin[0], half_input_bytes) == 0 && ' \
            'memcmp((const char *)observed_input + half_input_bytes, qin[1], half_input_bytes) == 0)'
    checked = checked.replace('memcmp(observed_input, qin, input_bytes) == 0', match)
    checked = checked.replace('memcmp(observed_input, qin, input_bytes)', '!' + match)
    strict = strict_256
else:
    strict = strict_128
verify = 'if (verify_dynamic(raw, weights, exact, quantized, observed_dsp, maxima, dsp_rows, blocks, tokens, unit)) return 1;'
old_marker = 'gpu_pack_full_byte_match=1 ion_visibility_match=1 gpu_shard_full_bitwise_match=1 dsp_scatter_match=1'
if text.count(verify) != 1 or text.count(old_marker) != 1:
    raise SystemExit("Dynamic benchmark reference changed; review replay quantization checks")
text = text.replace(strict, checked, 1)
text = text.replace(old_marker, 'gpu_quant_reference_used=1 ion_visibility_match=1 gpu_shard_full_bitwise_match=1 dsp_scatter_match=1', 1)
if args.output.exists():
    raise SystemExit("Output exists; choose a fresh scratch path")
args.output.write_text(text, encoding="utf-8", newline="\n")
