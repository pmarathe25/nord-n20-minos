#!/usr/bin/env python3
"""Start an isolated loopback-only server and verify its privilege boundary.

Run as root with explicit paths to the existing JAR and accepted EULA. This
creates and removes a separate temporary world; it does not open the live world
or copy its server configuration.
"""
import argparse
import ipaddress
import os
from pathlib import Path
import pwd
import shutil
import signal
import subprocess
import tempfile
import time


def assert_loopback_listeners(pid):
    sockets = set()
    for fd in Path(f'/proc/{pid}/fd').iterdir():
        try:
            target = os.readlink(fd)
        except FileNotFoundError:
            continue
        if target.startswith('socket:[') and target.endswith(']'):
            sockets.add(target[8:-1])
    listeners = 0
    for version in ('tcp', 'tcp6'):
        for line in Path('/proc/net', version).read_text().splitlines()[1:]:
            fields = line.split()
            if fields[3] != '0A' or fields[9] not in sockets:
                continue
            encoded = fields[1].split(':', 1)[0]
            if version == 'tcp':
                address = ipaddress.IPv4Address(bytes.fromhex(encoded)[::-1])
            else:
                address = ipaddress.IPv6Address(b''.join(
                    bytes.fromhex(encoded[i:i+8])[::-1] for i in range(0, 32, 8)))
            assert address.is_loopback or (getattr(address, 'ipv4_mapped', None) and address.ipv4_mapped.is_loopback)
            listeners += 1
    assert listeners, 'isolated server did not expose a TCP listener'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--jar', type=Path, required=True)
    parser.add_argument('--eula', type=Path, required=True)
    parser.add_argument('--java', type=Path, default=Path('/opt/java/bin/java'))
    parser.add_argument('--timeout', type=int, default=120)
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error('must run as root to drop privileges')
    jar = args.jar.resolve(strict=True)
    if 'eula=true' not in args.eula.read_text().splitlines():
        parser.error('the supplied EULA file must already be accepted')
    user = pwd.getpwnam('minecraft')
    with tempfile.TemporaryDirectory(prefix='n20-isolated-server-', dir='/tmp') as temp:
        root = Path(temp)
        os.chown(root, user.pw_uid, user.pw_gid)
        eula = root / 'eula.txt'
        shutil.copyfile(args.eula, eula)
        os.chown(eula, user.pw_uid, user.pw_gid)
        properties = root / 'server.properties'
        properties.write_text(
            'server-ip=127.0.0.1\nserver-port=0\nonline-mode=true\n'
            'white-list=true\nenforce-whitelist=true\nmanagement-server-enabled=false\n'
            'enable-rcon=false\nview-distance=2\nsimulation-distance=2\n'
        )
        os.chown(properties, user.pw_uid, user.pw_gid)
        os.chmod(properties, 0o600)
        log = root / 'isolated-server.log'
        with log.open('wb') as output:
            proc = subprocess.Popen(
                ['/usr/bin/setpriv', '--no-new-privs', '--bounding-set=-all',
                 '--reuid=minecraft', '--regid=minecraft', '--clear-groups',
                 str(args.java), '-Xms128M', '-Xmx768M', '-jar', str(jar), 'nogui'],
                cwd=root, stdin=subprocess.PIPE, stdout=output, stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            try:
                deadline = time.monotonic() + args.timeout
                while time.monotonic() < deadline:
                    if proc.poll() is not None:
                        raise RuntimeError('isolated server exited before becoming ready')
                    if b'Done (' in log.read_bytes():
                        break
                    time.sleep(0.25)
                else:
                    raise RuntimeError('isolated server did not become ready in time')
                status = Path(f'/proc/{proc.pid}/status').read_text()
                fields = dict(line.split(':', 1) for line in status.splitlines() if ':' in line)
                assert fields['NoNewPrivs'].strip() == '1'
                assert int(fields['CapBnd'].strip(), 16) == 0
                assert int(fields['Uid'].split()[1]) == user.pw_uid
                assert_loopback_listeners(proc.pid)
                proc.stdin.write(b'stop\n')
                proc.stdin.flush()
                if proc.wait(timeout=60) != 0:
                    raise RuntimeError('isolated server did not stop cleanly')
                print('ISOLATED_SERVER_PRIVILEGE_BOUNDARY_PASSED')
            finally:
                if proc.poll() is None:
                    os.killpg(proc.pid, signal.SIGTERM)
                    try:
                        proc.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        os.killpg(proc.pid, signal.SIGKILL)
                        proc.wait()


if __name__ == '__main__':
    main()
