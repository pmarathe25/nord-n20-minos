#!/usr/bin/env python3
"""Exercise Drive replication using isolated Restic and local rclone repositories."""
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

source = Path(__file__).resolve().parents[1] / 'runtime/usr/local/sbin/n20-drive-backup'
loader = importlib.machinery.SourceFileLoader('n20_drive_backup', str(source))
module = importlib.util.module_from_spec(importlib.util.spec_from_loader(loader.name, loader))
loader.exec_module(module)
original_rclone = module.rclone
copy_destinations = []
def local_rclone(*args, **kwargs):
    # The local backend lacks server-side copy; Drive supports --copy-dest.
    args = list(args)
    if '--copy-dest' in args:
        pos = args.index('--copy-dest')
        copy_destinations.append(args[pos + 1])
        del args[pos:pos + 2]
    return original_rclone(*args, **kwargs)
module.rclone = local_rclone

with tempfile.TemporaryDirectory(prefix='n20-drive-test-') as tmp:
    root = Path(tmp)
    world = root / 'world'
    world.mkdir()
    (world / 'level.dat').write_text('first version')
    module.REPO = root / 'repo'
    module.PASS = root / 'password'
    module.PASS.write_text('isolated test password\n')
    module.CONFIG = root / 'rclone.conf'
    module.CONFIG.write_text('')
    module.META = root / 'meta'
    module.BASE = root
    module.REMOTE = str(root / 'cloud')
    module.SOURCES = {str(world)}
    module.MIN_FREE = 0
    command = [module.RESTIC, '-r', str(module.REPO), '--password-file', str(module.PASS), '--no-cache']
    subprocess.run(command + ['init'], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(command + ['backup', str(world)], check=True, stdout=subprocess.DEVNULL)
    assert module.backup_once() == '1 snapshot(s) copied and verified'
    first = json.loads((module.META / 'drive-backup-last.json').read_text())['snapshot']
    assert (root / 'cloud' / 'snapshots' / first / 'world' / 'level.dat').read_text() == 'first version'
    assert (root / 'cloud' / 'snapshots' / first / 'COMPLETE.json').is_file()
    assert module.backup_once() == 'Already uploaded'
    time.sleep(1.1)
    (world / 'level.dat').write_text('second version')
    subprocess.run(command + ['backup', str(world)], check=True, stdout=subprocess.DEVNULL)
    assert module.backup_once() == '1 snapshot(s) copied and verified'
    second = json.loads((module.META / 'drive-backup-last.json').read_text())['snapshot']
    assert first != second
    assert copy_destinations == [f'{module.REMOTE}/snapshots/{first}/world']
    assert (root / 'cloud' / 'snapshots' / first / 'world' / 'level.dat').read_text() == 'first version'
    assert (root / 'cloud' / 'snapshots' / second / 'world' / 'level.dat').read_text() == 'second version'
    time.sleep(1.1)
    (world / 'level.dat').write_text('third version')
    subprocess.run(command + ['backup', str(world)], check=True, stdout=subprocess.DEVNULL)
    time.sleep(1.1)
    (world / 'level.dat').write_text('fourth version')
    subprocess.run(command + ['backup', str(world)], check=True, stdout=subprocess.DEVNULL)
    assert module.backup_once() == '2 snapshot(s) copied and verified'
    state = json.loads((module.META / 'drive-backup-last.json').read_text())
    assert len(state['uploaded']) == 4 and first in state['uploaded'] and second in state['uploaded']
    assert module.backup_once() == 'Already uploaded'
    latest = state['snapshot']
    time.sleep(1.1)
    os.symlink('/etc/passwd', world / 'link')
    subprocess.run(command + ['backup', str(world)], check=True, stdout=subprocess.DEVNULL)
    try: module.backup_once()
    except RuntimeError as exc: assert 'could not be copied' in str(exc)
    else: raise AssertionError('Symlink was not rejected')
    assert json.loads((module.META / 'drive-backup-last.json').read_text())['snapshot'] == latest
print('Drive backup isolation tests passed')
