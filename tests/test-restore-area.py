"""Reject attacker-controlled restore areas without touching live data."""
import importlib.machinery
import os
from pathlib import Path
import stat
import tempfile

m = importlib.machinery.SourceFileLoader('n20_admin', str(Path(__file__).resolve().parents[1] / 'runtime/usr/local/sbin/n20-admin')).load_module()
with tempfile.TemporaryDirectory(prefix='n20-restore-area-') as tmp:
    root = Path(tmp)
    m.WORLD = root / 'world'; m.WORLD.mkdir()
    backups = root / 'backups'; backups.mkdir()
    m.REPO = backups / 'repo'
    area = m.secure_restore_area()
    assert area.parent == backups and stat.S_IMODE(area.stat().st_mode) == 0o700
    area.chmod(0o777)
    try: m.secure_restore_area(); raise AssertionError('accepted writable restore area')
    except m.AdminError: pass
    area.chmod(0o700); area.rmdir()
    area.symlink_to(m.WORLD, target_is_directory=True)
    try: m.secure_restore_area(); raise AssertionError('accepted symlink restore area')
    except m.AdminError: pass
    area.unlink()
    backups.chmod(0o777)
    try: m.secure_restore_area(); raise AssertionError('accepted writable backup parent')
    except m.AdminError: pass
    stage = root / 'stage'; stage.mkdir()
    world = stage / 'srv' / 'minecraft' / 'world'; world.mkdir(parents=True)
    marker = world / 'level.dat'; marker.write_bytes(b'world')
    source = Path('/srv/minecraft/world')
    assert m.validate_restored_world(stage, source) == world
    marker.unlink(); marker.symlink_to('/etc/passwd')
    try: m.validate_restored_world(stage, source); raise AssertionError('accepted symlink marker')
    except m.AdminError: pass
    marker.unlink(); world.rmdir()
    world.symlink_to(root, target_is_directory=True)
    try: m.validate_restored_world(stage, source); raise AssertionError('accepted symlink world')
    except m.AdminError: pass
    world.unlink(); world.parent.rmdir(); world.parent.symlink_to(root, target_is_directory=True)
    try: m.validate_restored_world(stage, source); raise AssertionError('accepted symlink ancestor')
    except m.AdminError: pass
print('RESTORE_AREA_PERMISSIONS_AND_SYMLINK_GUARDS_PASSED')
