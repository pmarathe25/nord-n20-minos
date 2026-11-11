"""Replace the calculator link's reserved DT_RUNPATH with DT_HEXAGON_VER."""

from pathlib import Path
import struct
import sys


def tag_calculator(path):
    data = bytearray(path.read_bytes())
    if (len(data) < 52 or data[:7] != b"\x7fELF\x01\x01\x01"
            or struct.unpack_from("<HH", data, 16) != (3, 164)
            or struct.unpack_from("<I", data, 36)[0] != 0x66):
        raise ValueError("expected a little-endian Hexagon v66 shared object")

    header_offset = struct.unpack_from("<I", data, 28)[0]
    header_size, header_count = struct.unpack_from("<HH", data, 42)
    if header_size != 32 or header_offset + header_count * header_size > len(data):
        raise ValueError("invalid program header table")
    for index in range(header_count):
        header = struct.unpack_from("<8I", data, header_offset + index * header_size)
        if header[0] == 2:  # PT_DYNAMIC
            start, size = header[1], header[4]
            break
    else:
        raise ValueError("missing PT_DYNAMIC")

    if size % 8 or start + size > len(data):
        raise ValueError("invalid dynamic table")
    tags = {}
    for offset in range(start, start + size, 8):
        tag = struct.unpack_from("<I", data, offset)[0]
        if tag == 0:
            break
        tags.setdefault(tag, []).append(offset)
    else:
        raise ValueError("unterminated dynamic table")

    hexagon_version = 0x70000001
    runpath = tags.get(29, [])
    if hexagon_version in tags or len(runpath) != 1:
        raise ValueError("expected one reserved DT_RUNPATH and no DT_HEXAGON_VER")
    struct.pack_into("<II", data, runpath[0], hexagon_version, 3)
    path.write_bytes(data)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: tag-v66-elf.py LIBRARY")
    try:
        tag_calculator(Path(sys.argv[1]))
    except (OSError, ValueError) as error:
        raise SystemExit(str(error))
