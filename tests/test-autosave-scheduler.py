import gzip
import fcntl
import atexit
import importlib.machinery
import json
import os
import shutil
import tempfile
import time
from pathlib import Path
m=importlib.machinery.SourceFileLoader('n20_admin',str(Path(__file__).resolve().parents[1] / 'runtime/usr/local/sbin/n20-admin')).load_module()
top=Path(tempfile.mkdtemp(prefix='n20-autosave-validation-',dir='/tmp'))
atexit.register(shutil.rmtree,top,ignore_errors=True)
top.chmod(0o755)
m.COPY_HELPER=str(Path(__file__).resolve().parents[1] / 'runtime/usr/local/sbin/n20-backup-copy')
m.ROOT=top/'server'; m.WORLD=m.ROOT/'world'; m.REPO=top/'backups'/'repo'; m.PASS=top/'password'; m.RUN=top/'run'; m.META=top/'meta'
m.STAGE=top/'stage'; m.STAGE_WORLD=m.STAGE/'world'; m.LAST_SIG=m.META/'last-world-signature.json'; m.PRUNE=m.META/'last-prune'; m.SAVE_EVENT=m.RUN/'save-event.json'
m.STOP=top/'stop'; m.PID=top/'pid'; m.FIFO=top/'fifo'; m.LEGACY_HOLD=m.ROOT/'.n20-restore-hold'; m.HOLD=m.META/'restore-hold'; m.LEGACY_LOW=m.ROOT/'.n20-low-battery-hold'; m.LOW=m.META/'low-battery-hold'; m.LOCK=m.RUN/'operation.lock'; m.OP=m.RUN/'operation'; m.ERR=m.META/'error'; m.CACHE=m.META/'snapshots.json'; m.LAST=m.META/'last-auto-backup'
event_counter=[0]
def emit(when):
    event_counter[0]+=1
    (m.META/'pending-save-event.json').write_text(json.dumps({'id':f'{event_counter[0]:032x}','time':when,'server_pid':os.getpid()}))
m.WORLD.mkdir(parents=True); (m.WORLD/'level.dat').write_bytes(gzip.compress(b'isolated level')); (m.WORLD/'region').write_text('before')
m.directories(); m.PID.write_text(str(os.getpid())); m.pid_running=lambda:True; m.mc_status=lambda:{'players':{'online':1}}
inherited=os.open(m.LOCK,os.O_CREAT|os.O_RDWR,0o600); fcntl.flock(inherited,fcntl.LOCK_EX|fcntl.LOCK_NB)
m.close_inherited_operation_lock()
fresh=os.open(m.LOCK,os.O_RDWR); fcntl.flock(fresh,fcntl.LOCK_EX|fcntl.LOCK_NB); os.close(fresh)
m.ensure_repo(); m.atomic(m.LAST_SIG,json.dumps(m.world_signature())); m.atomic(m.LAST,str(int(time.time())-400)); (m.WORLD/'region').write_text('one changed region')
emit(time.time()); m.scheduler(once=True)
assert not m.CACHE.exists(), 'light activity backed up too early'
m.atomic(m.LAST,str(int(time.time())-1000)); m.scheduler(once=True)
items=m.snapshots(); assert len(items)==1 and 'auto' in items[0].get('tags',[]) and 'live' in items[0].get('tags',[])
assert not m.STOP.exists(), 'automatic live snapshot stopped the server'
baseline=len(items); m.atomic(m.LAST,str(int(time.time())-400))
for i in range(8): (m.WORLD/('change-%s'%i)).write_text(str(i))
emit(time.time()+1); m.scheduler(once=True)
assert len(m.snapshots())==baseline+1, 'heavy activity did not use shorter interval'
real_run=m.run_backup_copy
def changing_copy(cmd,*args,**kwargs):
    result=real_run(cmd,*args,**kwargs)
    if any(str(part).endswith('/n20-backup-copy') for part in cmd): (m.WORLD/'region').write_text('changed while copying')
    return result
m.run_backup_copy=changing_copy
try:
    try: m.backup(False); raise AssertionError('expected changed-file rejection')
    except m.AdminError as exc: assert 'changed during copy' in str(exc)
finally: m.run_backup_copy=real_run
assert not m.STAGE.exists() and not m.STOP.exists()
baseline=len(m.snapshots());m.atomic(m.LAST,str(int(time.time())-1000))
real_changed_files=m.changed_files;m.changed_files=lambda old,current:0
emit(time.time()-300);m.scheduler(once=True)
assert len(m.snapshots())==baseline and m.pending_save()[0], 'unresolved save event was not retained'
m.changed_files=real_changed_files
scheduled=[]
m.mc_status=lambda:None
m.world_signature=lambda:{'changed':'yes'}
m.changed_files=lambda old,current:1
m.locked=lambda action:action()
m.backup=lambda scheduled=False:globals()['scheduled'].append(scheduled)
m.atomic(m.LAST,str(int(time.time())-1000))
emit(time.time());m.scheduler(once=True)
assert scheduled==[True], 'invalid server status suppressed an eligible snapshot'
assert not m.pending_save()[0], 'completed snapshot did not acknowledge its save event'
m.mc_status=lambda:{'players':{'online':0}}
base=time.time(); now=[base]; sleeps=[0]
m.atomic(m.LAST,str(int(base-50)))
emit(base)
original_time=m.time.time;original_sleep=m.time.sleep;original_mono=m.time.monotonic;original_priority=m.lower_priority
original_waiter=m.backup_wake.waiter;waits=[]
def advance(_):
    now[0]+=20;sleeps[0]+=1
    if sleeps[0]==5: raise StopIteration
class FakeWaiter:
    def __init__(self,step): self.step=step
    def wait(self,timeout): waits.append(timeout);self.step(timeout)
    def close(self): pass
try:
    m.time.time=lambda:now[0];m.time.monotonic=lambda:now[0];m.time.sleep=advance;m.lower_priority=lambda:None
    m.backup_wake.waiter=lambda run,channel:FakeWaiter(advance)
    try:m.scheduler()
    except StopIteration:pass
finally:m.time.time=original_time;m.time.monotonic=original_mono;m.time.sleep=original_sleep;m.lower_priority=original_priority;m.backup_wake.waiter=original_waiter
assert scheduled==[True,True], f'early final save was discarded instead of retried: {scheduled}; error={m.read(m.ERR)}'
assert waits[-1]>=24*3600-60, 'idle scheduler did not wait a full day after its first snapshot'

# A changed on-disk world must be checked after an idle period even when the
# latest save notification is stale. An unchanged world must not be copied.
base=time.time();now=[base];sleeps=[0];dirty=[True];idle_backups=[];waits=[]
m.atomic(m.LAST,str(int(base-1000)))
(m.META/'pending-save-event.json').unlink(missing_ok=True)
m.changed_files=lambda old,current:1 if dirty[0] else 0
def idle_backup(scheduled=False):
    idle_backups.append(scheduled);dirty[0]=False
def idle_advance(_):
    now[0]+=20;sleeps[0]+=1
    if sleeps[0]==8: raise StopIteration
m.backup=idle_backup
try:
    m.time.time=lambda:now[0];m.time.monotonic=lambda:now[0];m.time.sleep=idle_advance;m.lower_priority=lambda:None
    m.backup_wake.waiter=lambda run,channel:FakeWaiter(idle_advance)
    try:m.scheduler()
    except StopIteration:pass
finally:m.time.time=original_time;m.time.monotonic=original_mono;m.time.sleep=original_sleep;m.lower_priority=original_priority;m.backup_wake.waiter=original_waiter
assert idle_backups==[True], 'idle scan failed to back up changed files exactly once'
assert waits[-1]>=24*3600-120, 'idle scan continued polling after the initial snapshot'
now=[base];sleeps=[0];dirty=[True];idle_backups=[]
m.mc_status=lambda:None
try:
    m.time.time=lambda:now[0];m.time.monotonic=lambda:now[0];m.time.sleep=idle_advance;m.lower_priority=lambda:None
    m.backup_wake.waiter=lambda run,channel:FakeWaiter(idle_advance)
    try:m.scheduler()
    except StopIteration:pass
finally:m.time.time=original_time;m.time.monotonic=original_mono;m.time.sleep=original_sleep;m.lower_priority=original_priority;m.backup_wake.waiter=original_waiter
assert not idle_backups, 'unknown player status was treated as idle'
# Advance an entire day after the first idle snapshot and verify that changed
# on-disk data is checked again, without intermediate timer polls.
base=time.time();now=[base];dirty=[True];daily=[];long_waits=[0]
m.mc_status=lambda:{'players':{'online':0}}
m.atomic(m.LAST,str(int(base-1000)))
def daily_backup(scheduled=False):
    daily.append(scheduled);dirty[0]=False;m.atomic(m.LAST,str(int(now[0])))
def daily_wait(timeout):
    if timeout>=24*3600-1:
        long_waits[0]+=1
        if long_waits[0]==2: raise StopIteration
        dirty[0]=True
    now[0]+=timeout
m.backup=daily_backup
try:
    m.time.time=lambda:now[0];m.time.monotonic=lambda:now[0];m.lower_priority=lambda:None
    m.backup_wake.waiter=lambda run,channel:FakeWaiter(daily_wait)
    try:m.scheduler()
    except StopIteration:pass
finally:m.time.time=original_time;m.time.monotonic=original_mono;m.lower_priority=original_priority;m.backup_wake.waiter=original_waiter
assert daily==[True,True], 'daily idle fallback did not check later changes'
# A join and leave can occur before the next player-count read. The activity
# marker must interrupt the daily wait and schedule a prompt idle check.
base=time.time();now=[base];dirty=[False];quick=[];long_waits=[0]
m.atomic(m.LAST,str(int(base-1000)));(m.RUN/'player-activity').write_text('initial\n')
def quick_backup(scheduled=False):
    quick.append(scheduled);dirty[0]=False;m.atomic(m.LAST,str(int(now[0])))
def quick_wait(timeout):
    if timeout>=24*3600-1:
        long_waits[0]+=1
        if long_waits[0]==2: raise StopIteration
        (m.RUN/'player-activity').write_text('brief-visit\n');dirty[0]=True;now[0]+=1
    else: now[0]+=timeout
m.backup=quick_backup
try:
    m.time.time=lambda:now[0];m.time.monotonic=lambda:now[0];m.lower_priority=lambda:None
    m.backup_wake.waiter=lambda run,channel:FakeWaiter(quick_wait)
    try:m.scheduler()
    except StopIteration:pass
finally:m.time.time=original_time;m.time.monotonic=original_mono;m.lower_priority=original_priority;m.backup_wake.waiter=original_waiter
assert quick==[True] and now[0]-base<300, 'brief visit did not restart the idle check'
# Failed backups retain the event across scheduler invocations. A new event
# arriving during a successful copy must remain pending for the next snapshot.
m.changed_files=lambda old,current:1
m.mc_status=lambda:{'players':{'online':1}}
m.atomic(m.LAST,str(int(time.time())-1000));emit(time.time()-300)
expected=m.pending_save()[0]
def fail_locked(action): raise m.AdminError('test failure')
m.locked=fail_locked;m.scheduler(once=True)
assert m.pending_save()[0]==expected, 'failed backup cleared the pending save'
m.locked=lambda action:action()
def new_event_during_backup(scheduled=False): emit(time.time())
m.backup=new_event_during_backup;m.scheduler(once=True)
assert m.pending_save()[0] and m.pending_save()[0]!=expected, 'newer event was lost during backup'
m.backup=lambda scheduled=False:None;m.scheduler(once=True)
assert not m.pending_save()[0], 'successful backup did not clear the pending save'
print('SAVE_EVENT_PERSISTENCE_IDLE_RATE_AND_CHANGED_FILE_REJECTION_PASSED')
