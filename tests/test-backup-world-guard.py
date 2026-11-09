"""Exercise world-root replacement using only disposable directories."""
import importlib.machinery
import os
import subprocess
import time
from pathlib import Path
import tempfile

m = importlib.machinery.SourceFileLoader('n20_backup_guard', str(Path(__file__).resolve().parents[1] / 'runtime/usr/local/sbin/n20-admin')).load_module()
with tempfile.TemporaryDirectory(prefix='n20-world-guard-') as tmp:
    root = Path(tmp); root.chmod(0o755)
    m.RUN = root / 'run'; m.RUN.mkdir()
    m.ROOT = root / 'server'; m.ROOT.mkdir()
    m.WORLD = m.ROOT / 'world'; m.WORLD.mkdir()
    m.STAGE = root / 'stage'; m.STAGE_WORLD = m.STAGE / 'world'
    m.COPY_HELPER = str(Path(__file__).resolve().parents[1] / 'runtime/usr/local/sbin/n20-backup-copy')
    m.MIN_FREE = 0
    m.set_operation = lambda value: None
    m.time.sleep = lambda value: None
    (m.WORLD / 'level.dat').write_text('world')
    secret = root / 'private'; secret.mkdir(); (secret / 'secret').write_text('private')
    marker = m.WORLD / 'level.dat'
    for bad_marker in ('empty', 'symlink'):
        marker.unlink()
        if bad_marker == 'empty': marker.touch()
        else: marker.symlink_to(secret / 'secret')
        try: m.create_snapshot('test'); raise AssertionError('invalid marker accepted for snapshot')
        except m.AdminError: pass
        try: m.copy_quiescent_world(); raise AssertionError('invalid marker accepted in copy')
        except m.AdminError: pass
        m.clear_stage()
    marker.unlink(); marker.write_text('world')
    # Swap a directory after fwalk has statted it but before it opens it.
    # The root-run scan must not descend into the replacement symlink.
    trap = m.WORLD / 'trap'; trap.mkdir()
    saved_open = os.open
    def swap_before_open(path, flags, *args, **kwargs):
        if path == 'trap' and kwargs.get('dir_fd') is not None:
            trap.rename(m.WORLD / 'trap-old')
            trap.symlink_to(secret, target_is_directory=True)
        return saved_open(path, flags, *args, **kwargs)
    os.open = swap_before_open
    try:
        signature = m.world_signature()
        assert 'trap/secret' not in signature, 'Scan followed swapped directory'
    finally:
        os.open = saved_open
    assert trap.is_symlink(), 'Race was not exercised'
    trap.unlink(); (m.WORLD / 'trap-old').rmdir()
    (m.WORLD / 'external').symlink_to(secret, target_is_directory=True)
    before = m.world_signature()
    empty = m.WORLD / 'empty'; empty.mkdir()
    assert m.world_signature() != before, 'Empty directory change was missed'
    before = m.world_signature(); empty.chmod(0o700)
    assert m.world_signature() != before, 'Directory permission change was missed'
    before = m.world_signature(); (m.WORLD / 'external').unlink()
    (m.WORLD / 'external').symlink_to(m.WORLD / 'missing', target_is_directory=True)
    assert m.world_signature() != before, 'Directory symlink replacement was missed'
    (m.WORLD / 'external').unlink(); (m.WORLD / 'external').symlink_to(secret, target_is_directory=True)
    empty.chmod(0o755)
    restricted = m.WORLD / 'root-only'
    restricted.write_text('must not be read by rsync'); restricted.chmod(0o600)
    try: m.copy_quiescent_world(); raise AssertionError('Privileged file was copied')
    except m.AdminError: pass
    assert not (m.STAGE_WORLD / 'root-only').exists()
    m.clear_stage(); restricted.unlink()
    # Service startup may inherit a root-only working directory. The
    # unprivileged copier must not depend on being able to traverse it.
    private_cwd = root / 'root-only-cwd'; private_cwd.mkdir(mode=0o700)
    original_cwd = os.getcwd()
    try:
        os.chdir(private_cwd)
        m.copy_quiescent_world()
    finally:
        os.chdir(original_cwd)
    assert (m.STAGE_WORLD / 'external').is_symlink()
    assert not (m.STAGE_WORLD / 'secret').exists()
    m.clear_stage()
    real_run = m.run_backup_copy
    def change_directory_during_copy(cmd, *args, **kwargs):
        result = real_run(cmd, *args, **kwargs)
        (m.WORLD / 'new-empty-dir').mkdir()
        return result
    m.run_backup_copy = change_directory_during_copy
    try:
        try: m.copy_quiescent_world(); raise AssertionError('Directory change accepted')
        except m.AdminError as exc: assert 'changed during copy' in str(exc)
    finally: m.run_backup_copy = real_run
    m.clear_stage()
    old = m.ROOT / 'old-world'
    def swap_during_copy(cmd, *args, **kwargs):
        if any(str(x).endswith('/n20-backup-copy') for x in cmd):
            os.rename(m.WORLD, old)
            m.WORLD.symlink_to(secret, target_is_directory=True)
        return real_run(cmd, *args, **kwargs)
    m.run_backup_copy = swap_during_copy
    try:
        try: m.copy_quiescent_world(); raise AssertionError('World replacement accepted')
        except m.AdminError as exc: assert 'changed' in str(exc)
    finally: m.run_backup_copy = real_run
    assert not (m.STAGE_WORLD / 'secret').exists()
    try: m.world_signature(); raise AssertionError('Symlink world accepted')
    except m.AdminError: pass
    pidfile = root / 'child-pid'
    try:
        m.run_backup_copy(['/bin/sh','-c','sleep 30 & echo $! > "$1"; wait','test',str(pidfile)], timeout=0.5)
        raise AssertionError('Expected copy timeout')
    except subprocess.TimeoutExpired:
        pass
    child = int(pidfile.read_text())
    time.sleep(.1)
    try:
        state = Path('/proc/%d/stat' % child).read_text().split(') ', 1)[1][0]
        assert state == 'Z', 'Timeout left copier child running'
    except FileNotFoundError: pass
print('PINNED_WORLD_COPY_REJECTS_REPLACEMENT_PASSED')
