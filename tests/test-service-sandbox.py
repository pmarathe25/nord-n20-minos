"""Run a small child through the real cgroup/seccomp launcher, never a server."""
from pathlib import Path
import subprocess

helper=Path(__file__).resolve().parents[1]/'runtime/usr/local/sbin/n20-service-sandbox'
code='''import ctypes,errno,os
from pathlib import Path
s=Path('/proc/self/status').read_text()
assert 'Seccomp:\\t2' in s and 'NoNewPrivs:\\t1' in s
assert 'n20-wiki-bridge' in Path('/proc/self/cgroup').read_text()
group=Path('/run/n20-cgroups/memory/n20-optional/n20-wiki-bridge')
assert int((group/'memory.limit_in_bytes').read_text())==256*1024**2
assert int((group/'memory.memsw.limit_in_bytes').read_text())==384*1024**2
assert Path('/proc/self/oom_score_adj').read_text().strip()=='600'
libc=ctypes.CDLL(None,use_errno=True)
assert libc.ptrace(0,0,0,0)==-1 and ctypes.get_errno()==errno.EPERM
assert os.getpid()>1
print('SERVICE_MEMORY_AND_SECCOMP_ACTIVE')
'''
subprocess.run(['/usr/bin/python3',str(helper),'wiki-bridge','--','/usr/bin/python3','-c',code],check=True)
