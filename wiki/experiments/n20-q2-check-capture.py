"""Check a private capture against the shared benchmark's default synthetic input.

This checks exact FP16 bytes and prints only pass/fail and counts, not values.
Run only on the shared benchmark's normal input (no N20_EDGE/GUARD flags).
"""
import argparse
import struct
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("capture", type=Path)
args = parser.parse_args()
with args.capture.open("rb") as file:
    header = file.read(32)
    if len(header) != 32:
        raise SystemExit("Capture header is incomplete")
    magic, version, rows, columns, source_tokens, start, tokens, padded = struct.unpack("<8I", header)
    if (magic != 0x4330324E or version != 1 or not rows or not columns or
        not tokens or source_tokens > 256 or start + tokens > source_tokens or
        padded != (tokens + 3) // 4 * 4):
        raise SystemExit("Capture header is invalid")
    data = file.read()
    if len(data) != columns * padded * 2:
        raise SystemExit("Capture length mismatch")
random = 1
checked = 0
for token in range(source_tokens):
    for column in range(columns):
        random = (random * 1664525 + 1013904223) & 0xffffffff
        value = ((random >> 16) - 32768) / 32768.0
        if start <= token < start + tokens:
            offset = (column * padded + token - start) * 2
            if data[offset:offset + 2] != struct.pack("<e", value):
                raise SystemExit("Capture does not match expected synthetic FP16 input")
            checked += 1
print(f"capture_exact_fp16_match=1 tokens={tokens} columns={columns} elements={checked}")
