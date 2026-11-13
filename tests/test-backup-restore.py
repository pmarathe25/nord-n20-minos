import gzip
import atexit
import importlib.machinery
import os
import shutil
import tempfile
from pathlib import Path
m=importlib.machinery.SourceFileLoader('n20_admin',str(Path(__file__).resolve().parents[1] / 'runtime/usr/local/sbin/n20-admin')).load_module()
top=Path(tempfile.mkdtemp(prefix='n20-restore-validation-',dir='/tmp'))
atexit.register(shutil.rmtree,top,ignore_errors=True)
top.chmod(0o755)
m.COPY_HELPER=str(Path(__file__).resolve().parents[1] / 'runtime/usr/local/sbin/n20-backup-copy')
m.ROOT=top/'server'; m.WORLD=m.ROOT/'world'; m.REPO=top/'backups'/'repo'; m.PASS=top/'password'; m.RUN=top/'run'; m.META=top/'meta'
m.STAGE=top/'stage'; m.STAGE_WORLD=m.STAGE/'world'; m.LAST_SIG=m.META/'last-world-signature.json'; m.PRUNE=m.META/'last-prune'; m.SAVE_EVENT=m.RUN/'save-event.json'
m.STOP=top/'stop'; m.PID=top/'pid'; m.FIFO=top/'fifo'; m.LEGACY_HOLD=m.ROOT/'.n20-restore-hold'; m.HOLD=m.META/'restore-hold'; m.LEGACY_LOW=m.ROOT/'.n20-low-battery-hold'; m.LOW=m.META/'low-battery-hold'; m.LOCK=m.RUN/'operation.lock'; m.OP=m.RUN/'operation'; m.ERR=m.META/'error'; m.CACHE=m.META/'snapshots.json'; m.LAST=m.META/'last-auto-backup'
m.WORLD.mkdir(parents=True); (m.WORLD/'level.dat').write_bytes(gzip.compress(b'valid isolated test level')); (m.WORLD/'marker').write_text('original')
m.directories(); saved=m.create_snapshot('manual')[0]['id']; (m.WORLD/'marker').write_text('later changes')
m.make_state(); assert f'backup_id_0={saved}' in (m.RUN/'state').read_text()
assert m.snapshot_time('2026-11-08T12:34:56-08:00')=='2026-11-08 20:34:56 UTC'
assert m.snapshot_time('2026-11-08T12:34:56')=='Time unavailable'
m.restore(saved[:12]); assert (m.WORLD/'marker').read_text()=='original'
old=list((m.REPO.parent/'.n20-restore').glob('.world-before-*')); assert len(old)==1 and (old[0]/'marker').read_text()=='later changes'
assert not m.HOLD.exists() and not m.STOP.exists(); assert any('pre-restore' in s.get('tags',[]) for s in m.snapshots())
(m.WORLD/'marker').write_text('staged checkpoint')
(m.META/'pending-save-event.json').write_text(__import__('json').dumps({'id':'a'*32,'time':__import__('time').time()}))
m.backup(False)
assert not m.pending_save()[0], 'manual snapshot did not acknowledge its saved notification'
staged=next(x['id'] for x in m.snapshots() if str(m.STAGE_WORLD) in x['paths'])
(m.WORLD/'marker').write_text('changed after staged checkpoint')
m.restore(staged[:12]); assert (m.WORLD/'marker').read_text()=='staged checkpoint'
(m.WORLD/'marker').write_text('keep after failure'); real_rename=os.rename
def fault(src,dst):
    if '/.n20-restore-' in str(src) and Path(dst)==m.WORLD: raise OSError('injected swap failure')
    return real_rename(src,dst)
m.os.rename=fault
try:
    try: m.restore(saved[:12]); raise AssertionError('expected swap failure')
    except OSError as e: assert 'injected swap failure' in str(e)
finally: m.os.rename=real_rename
assert (m.WORLD/'marker').read_text()=='keep after failure'; assert not m.HOLD.exists() and not m.STOP.exists()
# Simulate a crash just after moving the original world, before replacement.
def interrupted(src, dst):
    if Path(src) == m.WORLD:
        real_rename(src, dst)
        raise OSError('injected interruption after world move')
    return real_rename(src, dst)
m.os.rename = interrupted
try:
    try: m.restore(saved[:12]); raise AssertionError('expected interruption')
    except OSError as e: assert 'injected interruption' in str(e)
finally: m.os.rename = real_rename
assert m.HOLD.exists() and not m.WORLD.exists()
assert m.HOLD.parent == m.META and m.HOLD.stat().st_mode & 0o077 == 0
hold = __import__('json').loads(m.HOLD.read_text())
assert hold['phase'] == 'swapping' and Path(hold['old']).is_dir()
real_rename(hold['old'], m.WORLD)
m.HOLD.unlink(); m.STOP.unlink(missing_ok=True)
assert (m.WORLD/'marker').read_text() == 'keep after failure'
print('ISOLATED_LIVE_AND_OFFLINE_RESTORE_AND_FAILED_SWAP_PASSED')
