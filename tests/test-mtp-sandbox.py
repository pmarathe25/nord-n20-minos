#!/usr/bin/env python3
"""Exercise the USB responder sandbox with disposable directories only."""
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import time


def read_until(proc, token, timeout=10):
    data = b''
    deadline = time.monotonic() + timeout
    while token not in data:
        remaining = deadline - time.monotonic()
        if remaining <= 0 or not select.select([proc.stdout], [], [], remaining)[0]:
            raise AssertionError(f'timed out waiting for {token!r}: {data!r}')
        part = os.read(proc.stdout.fileno(), 4096)
        if not part:
            raise AssertionError(f'sandbox exited before {token!r}: {data!r}')
        data += part
    return data


def send(proc, command, token):
    proc.stdin.write((command + '\n').encode())
    proc.stdin.flush()
    return read_until(proc, token)


def main():
    if os.geteuid() != 0 or len(sys.argv) != 2:
        raise SystemExit('run as root: test-mtp-sandbox.py /path/to/n20-mtp-sandbox')
    existing = {p.name for p in Path('/run/n20').glob('mtp-sandbox-*') if p.is_dir()}
    with tempfile.TemporaryDirectory(prefix='n20-mtp-test-', dir='/tmp') as temp:
        parent = Path(temp)
        parent.chmod(0o755)
        world = parent / 'world'
        world.mkdir()
        (world / 'level.dat').write_text('old-world\n')
        (world / 'level.dat').chmod(0o600)
        (world / 'escape').symlink_to('/etc/shadow')
        proc = subprocess.Popen([sys.argv[1], '--test', str(parent)], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
        try:
            read_until(proc, b'SANDBOX_READY\n')
            deadline = time.monotonic() + 10
            while True:
                if proc.poll() is not None:
                    raise AssertionError(f'sandbox exited early: {proc.stderr.read()!r}')
                children = []
                for candidate in Path('/proc').iterdir():
                    if not candidate.name.isdigit():
                        continue
                    try:
                        status = (candidate / 'status').read_text()
                    except (FileNotFoundError, ProcessLookupError):
                        continue
                    values = dict(line.split(':', 1) for line in status.splitlines() if ':' in line)
                    if int(values.get('PPid', '-1').strip()) == proc.pid:
                        children.append(candidate.name)
                if children:
                    child = int(children[0])
                    fields = dict(line.split(':', 1) for line in Path(f'/proc/{child}/status').read_text().splitlines() if ':' in line)
                    if fields['Uid'].split()[1] == '61124':
                        break
                if time.monotonic() >= deadline:
                    raise AssertionError('sandboxed child did not drop UID')
                time.sleep(0.05)
            assert int(fields['CapBnd'].strip(), 16) == 2
            assert int(fields['CapEff'].strip(), 16) == 2
            assert fields['NoNewPrivs'].strip() == '1'
            assert os.readlink(f'/proc/{child}/ns/net') != os.readlink('/proc/self/ns/net')
            out = send(proc, 'if [ -c /dev/null ] && echo ok > /dev/null; then echo NULL_OK; else echo NULL_BAD; fi', b'NULL_OK\n')
            assert b'NULL_BAD\n' not in out
            duplicate = subprocess.run([sys.argv[1], '--test', str(parent)], stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, timeout=10)
            assert duplicate.returncode != 0 and (b'UID is already in use' in duplicate.stderr or b'lock is unavailable or in use' in duplicate.stderr)
            out = send(proc, '/bin/busybox cat /srv/minecraft/world/level.dat; echo READ_DONE', b'READ_DONE\n')
            assert b'old-world\n' in out
            out = send(proc, 'if [ -r /srv/minecraft/world/escape ]; then echo ESCAPED; else echo CONFINED; fi', b'CONFINED\n')
            assert b'ESCAPED\n' not in out
            out = send(proc, 'if (echo bad > /srv/minecraft/world/level.dat) 2>/dev/null; then echo WRITABLE; else echo READONLY; fi', b'READONLY\n')
            assert b'WRITABLE\n' not in out
            out = send(proc, 'if (echo bad > /created-in-root) 2>/dev/null; then echo WRITABLE; else echo ROOT_READONLY; fi', b'ROOT_READONLY\n')
            assert b'WRITABLE\n' not in out

            staged = parent / 'staged'
            staged.mkdir()
            (staged / 'level.dat').write_text('new-world\n')
            world.rename(parent / 'previous')
            staged.rename(world)
            read_until(proc, b'WORLD_CHANGED\n')
            assert proc.wait(timeout=10) == 75
            proc = subprocess.Popen([sys.argv[1], '--test', str(parent)], stdin=subprocess.PIPE,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
            read_until(proc, b'SANDBOX_READY\n')
            out = send(proc, '/bin/busybox cat /srv/minecraft/world/level.dat; echo REBIND_DONE', b'REBIND_DONE\n')
            assert b'new-world\n' in out and b'old-world\n' not in out

            world.rename(parent / 'held')
            world.symlink_to('/etc')
            read_until(proc, b'WORLD_CHANGED\n')
            assert proc.wait(timeout=10) == 75
            refused = subprocess.run([sys.argv[1], '--test', str(parent)], stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, timeout=10)
            assert refused.returncode != 0 and b'world unavailable or unsafe' in refused.stderr
            world.unlink()
            (parent / 'held').rename(world)
            proc = subprocess.Popen([sys.argv[1], '--test', str(parent)], stdin=subprocess.PIPE,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
            read_until(proc, b'SANDBOX_READY\n')
            out = send(proc, '/bin/busybox cat /srv/minecraft/world/level.dat; echo RECOVERED', b'RECOVERED\n')
            assert b'new-world\n' in out
            proc.stdin.write(b'exit\n')
            proc.stdin.flush()
            assert proc.wait(timeout=10) == 0
            remaining = {p.name for p in Path('/run/n20').glob('mtp-sandbox-*') if p.is_dir()}
            assert remaining == existing, 'sandbox left a temporary jail behind'
            print('MTP_SANDBOX_RESTORE_AND_SYMLINK_GUARDS_PASSED')
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


if __name__ == '__main__':
    main()
