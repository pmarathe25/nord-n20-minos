#!/usr/bin/python3
"""Validate a bridge-owned model demand without following untrusted links."""
import os
from pathlib import Path
import pwd
import stat
import sys


def requested(path, bridge_pid):
    try:
        pid = int(bridge_pid)
        if pid <= 1:
            return False
        start = Path(f'/proc/{pid}/stat').read_text().rpartition(')')[2].split()[19]
        expected = f'{pid} {start}'.encode('ascii')
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
        try:
            info = os.fstat(fd)
            if (not stat.S_ISREG(info.st_mode) or info.st_uid != pwd.getpwnam('minecraft').pw_uid or
                    info.st_nlink != 1 or stat.S_IMODE(info.st_mode) != 0o600 or info.st_size > 64):
                return False
            return os.read(fd, 65).strip() == expected
        finally:
            os.close(fd)
    except (OSError, ValueError, KeyError, TypeError, IndexError):
        return False


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit(2)
    raise SystemExit(0 if requested(Path(sys.argv[1]), sys.argv[2]) else 1)
