import gzip
import importlib.machinery
import json
import os
import tempfile
import time
from pathlib import Path
m=importlib.machinery.SourceFileLoader('n20_admin',str(Path(__file__).resolve().parents[1] / 'runtime/usr/local/sbin/n20-admin')).load_module()
top=Path(tempfile.mkdtemp(prefix='n20-autosave-validation-',dir='/tmp'))
m.ROOT=top/'server'; m.WORLD=m.ROOT/'world'; m.REPO=top/'backups'/'repo'; m.PASS=top/'password'; m.RUN=top/'run'; m.META=top/'meta'
m.STAGE=top/'stage'; m.STAGE_WORLD=m.STAGE/'world'; m.LAST_SIG=m.META/'last-world-signature.json'; m.PRUNE=m.META/'last-prune'; m.SAVE_EVENT=m.RUN/'save-event.json'
m.STOP=top/'stop'; m.PID=top/'pid'; m.FIFO=top/'fifo'; m.HOLD=m.ROOT/'.n20-restore-hold'; m.LOW=m.ROOT/'.n20-low-battery-hold'; m.LOCK=m.RUN/'operation.lock'; m.OP=m.RUN/'operation'; m.ERR=m.META/'error'; m.CACHE=m.META/'snapshots.json'; m.LAST=m.META/'last-auto-backup'
m.WORLD.mkdir(parents=True); (m.WORLD/'level.dat').write_bytes(gzip.compress(b'isolated level')); (m.WORLD/'region').write_text('before')
m.directories(); m.PID.write_text(str(os.getpid())); m.pid_running=lambda:True; m.mc_status=lambda:{'players':{'online':1}}
m.ensure_repo(); m.atomic(m.LAST_SIG,json.dumps(m.world_signature())); m.atomic(m.LAST,str(int(time.time())-400)); (m.WORLD/'region').write_text('one changed region')
m.SAVE_EVENT.write_text(json.dumps({'time':time.time(),'server_pid':os.getpid()})); m.scheduler(once=True)
assert not m.CACHE.exists(), 'light activity backed up too early'
m.atomic(m.LAST,str(int(time.time())-1000)); m.scheduler(once=True)
items=m.snapshots(); assert len(items)==1 and 'auto' in items[0].get('tags',[]) and 'live' in items[0].get('tags',[])
assert not m.STOP.exists(), 'automatic live snapshot stopped the server'
baseline=len(items); m.atomic(m.LAST,str(int(time.time())-400))
for i in range(8): (m.WORLD/('change-%s'%i)).write_text(str(i))
m.SAVE_EVENT.write_text(json.dumps({'time':time.time()+1,'server_pid':os.getpid()})); m.scheduler(once=True)
assert len(m.snapshots())==baseline+1, 'heavy activity did not use shorter interval'
real_run=m.subprocess.run
def changing_copy(cmd,*args,**kwargs):
    result=real_run(cmd,*args,**kwargs)
    if any(str(part).endswith('/rsync') for part in cmd): (m.WORLD/'region').write_text('changed while copying')
    return result
m.subprocess.run=changing_copy
try:
    try: m.backup(False); raise AssertionError('expected changed-file rejection')
    except m.AdminError as exc: assert 'changed during copy' in str(exc)
finally: m.subprocess.run=real_run
assert not m.STAGE.exists() and not m.STOP.exists()
baseline=len(m.snapshots());m.atomic(m.LAST,str(int(time.time())-1000))
m.SAVE_EVENT.write_text(json.dumps({'time':time.time()-300,'server_pid':os.getpid()}));m.scheduler(once=True)
assert len(m.snapshots())==baseline, 'stale save event triggered a snapshot'
print('SAVE_EVENT_RATE_STALE_EVENT_AND_CHANGED_FILE_REJECTION_PASSED')
