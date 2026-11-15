"""Separate socket and GPU groups with real dropped identities/fake devices."""
import os
from pathlib import Path
import socket
import stat
import subprocess
import tempfile

with tempfile.TemporaryDirectory(prefix='n20-wiki-groups-') as tmp:
    root=Path(tmp);root.chmod(0o755)
    model=root/'model';model.mkdir();os.chown(model,61124,61125);model.chmod(0o2750)
    gpu=root/'gpu';os.mknod(gpu,stat.S_IFCHR|0o660,os.makedev(1,3));os.chown(gpu,0,61124);gpu.chmod(0o660)
    prefix=['setpriv','--no-new-privs','--bounding-set=-all']
    serve='''import os,socket,sys
os.umask(0o007)
fd=os.open(sys.argv[2],os.O_RDWR);os.close(fd)
s=socket.socket(socket.AF_UNIX);s.bind(sys.argv[1]);s.listen(1);print('ready',flush=True)
c,_=s.accept();assert c.recv(4)==b'ping';c.sendall(b'pong');c.close();s.close()
'''
    child=subprocess.Popen(prefix+['--reuid=61124','--regid=61124','--clear-groups','python3','-c',serve,str(model/'test.sock'),str(gpu)],stdout=subprocess.PIPE,text=True,cwd='/')
    try:
        assert child.stdout.readline().strip()=='ready'
        assert (model/'test.sock').stat().st_gid==61125
        client='''import os,socket,sys
try:os.open(sys.argv[2],os.O_RDWR)
except PermissionError:pass
else:raise AssertionError('bridge opened GPU node')
s=socket.socket(socket.AF_UNIX);s.connect(sys.argv[1]);s.sendall(b'ping');assert s.recv(4)==b'pong';s.close()
'''
        subprocess.run(prefix+['--reuid=999','--regid=999','--groups=61125','python3','-c',client,str(model/'test.sock'),str(gpu)],check=True,cwd='/')
        assert child.wait(timeout=5)==0
    finally:
        if child.poll() is None:child.kill();child.wait()
print('SOCKET_GROUP_WITHOUT_GPU_ACCESS_PASSED')
