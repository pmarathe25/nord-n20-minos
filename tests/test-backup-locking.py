"""Real Restic contention is waited out, without removing live locks."""
import importlib.machinery
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time
from unittest.mock import patch

root=Path(__file__).resolve().parents[1]
m=importlib.machinery.SourceFileLoader('locking_admin',str(root/'runtime/usr/local/sbin/n20-admin')).load_module()
with tempfile.TemporaryDirectory(prefix='n20-backup-locking-') as tmp:
    area=Path(tmp)
    m.REPO=area/'repo';m.PASS=area/'password';m.PASS.write_text('isolated test password\n')
    m.run_restic(['init'],timeout=120)
    cmd=['restic','-r',str(m.REPO),'--password-file',str(m.PASS),'--no-cache']
    held=subprocess.Popen(cmd+['backup','--stdin','--stdin-filename','isolated'],stdin=subprocess.PIPE,
                          stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    try:
        deadline=time.monotonic()+10
        while not list((m.REPO/'locks').glob('*')):
            assert held.poll() is None and time.monotonic()<deadline
            time.sleep(.05)
        def finish():
            time.sleep(.5);held.stdin.write(b'isolated data');held.stdin.close()
        worker=threading.Thread(target=finish);worker.start()
        m.run_restic(['forget','--keep-last','1'],timeout=120)
        worker.join();assert held.wait(timeout=10)==0
        assert not list((m.REPO/'locks').glob('*'))
    finally:
        if held.poll() is None:held.kill();held.wait()
    with patch.object(m.subprocess,'run',return_value=subprocess.CompletedProcess([],0,'','')) as run:
        m.run_restic(['snapshots','--json'],timeout=120)
        assert run.call_args.args[0][-4:-2]==['--retry-lock','30s']
        m.run_restic(['prune'],timeout=3600)
        assert '5m' in run.call_args.args[0]
    m.RUN=area/'run';m.META=area/'meta';m.LOCK=m.RUN/'lock';m.ERR=m.META/'error';m.OP=m.RUN/'operation'
    m.directories();m.set_error('old failure')
    def success():assert m.read(m.ERR)=='old failure'
    m.locked(success);assert not m.ERR.exists()
    m.set_error('old failure')
    def failure():raise m.AdminError('new failure')
    try:m.locked(failure)
    except m.AdminError:pass
    else:raise AssertionError('failure ignored')
    assert m.read(m.ERR)=='old failure'
    m.locked(lambda:m.set_error('Snapshot saved; cleanup failed: test'))
    assert m.read(m.ERR)=='Snapshot saved; cleanup failed: test'
print('RESTIC_LOCK_CONTENTION_PASSED')
