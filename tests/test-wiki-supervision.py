"""Bound restart delay and terminate a disposable TERM-resistant child."""
import importlib.machinery
from pathlib import Path
import subprocess
import sys
import time
import tempfile
from unittest.mock import patch

root=Path(__file__).resolve().parents[1]
m=importlib.machinery.SourceFileLoader('wiki_supervisor',str(root/'runtime/usr/local/sbin/n20-wiki-supervisor')).load_module()
assert [m.backoff(n) for n in range(7)]==[30,60,120,240,480,900,900]
stop=importlib.machinery.SourceFileLoader('stop_process',str(root/'runtime/usr/local/sbin/n20-stop-process')).load_module()
child=subprocess.Popen([sys.executable,'-c','import signal,time;signal.signal(signal.SIGTERM,signal.SIG_IGN);print("ready",flush=True);time.sleep(30)'],stdout=subprocess.PIPE,text=True)
try:
    assert child.stdout.readline().strip()=='ready'
    started=time.monotonic()
    assert stop.stop(child.pid,.2,1)
    assert child.wait(timeout=2)==-9 and time.monotonic()-started<2
finally:
    if child.poll() is None:child.kill();child.wait()
with tempfile.TemporaryDirectory(prefix='n20-wiki-supervise-') as tmp:
    area=Path(tmp);enabled=area/'enabled';enabled.touch()
    clock=[0];attempts=[];events=[]
    class Child:
        def poll(self):return 7
        def wait(self):return 7
    def launch(args):attempts.append(clock[0]);return Child()
    def allowed():
        if len(attempts)<2:return True
        enabled.unlink();return False
    with patch.object(m,'ENABLED',enabled),patch.object(m,'LOCK',area/'lock'), \
         patch.object(m,'allowed',side_effect=allowed),patch.object(m.subprocess,'Popen',side_effect=launch), \
         patch.object(m,'ensure_server_pid_access'), \
         patch.object(m.time,'monotonic',side_effect=lambda:clock[0]), \
         patch.object(m.time,'sleep',side_effect=lambda seconds:clock.__setitem__(0,clock[0]+seconds)), \
         patch.object(m.signal,'signal'),patch.object(m.HEALTH,'record',side_effect=lambda *a:events.append(a)):
        m.main()
    assert attempts==[0,30] and clock[0]==91
    assert sum(e[1]=='backoff' for e in events)==2 and events[-1][1]=='paused'
print('WIKI_BOUNDED_BACKOFF_AND_FORCED_TERMINATION_PASSED')
