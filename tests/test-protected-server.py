#!/usr/bin/env python3
"""Start a disposable server using a root-controlled executable bundle."""
import argparse
import hashlib
import importlib.machinery
import os
from pathlib import Path
import pwd
import shutil
import signal
import subprocess
import tempfile
import time


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--jar',required=True,type=Path);p.add_argument('--eula',required=True,type=Path)
    a=p.parse_args()
    if os.geteuid()!=0:p.error('must run as root')
    if 'eula=true' not in a.eula.read_text().splitlines():p.error('EULA must already be accepted')
    root=Path(__file__).resolve().parents[1]
    verify=importlib.machinery.SourceFileLoader('n20_server_boundary',str(root/'tests/test-server-privileges.py')).load_module()
    digest=hashlib.sha256()
    with a.jar.open('rb') as f:
        for block in iter(lambda:f.read(1048576),b''):digest.update(block)
    user=pwd.getpwnam('minecraft')
    with tempfile.TemporaryDirectory(prefix='n20-protected-test-',dir='/tmp') as tmp:
        top=Path(tmp);top.chmod(0o755);bundle=top/'bundle';world=top/'world';world.mkdir();os.chown(world,user.pw_uid,user.pw_gid)
        prepare=['python3',str(root/'tools/prepare-protected-server.py'),'--jar',str(a.jar),'--sha256',digest.hexdigest(),'--output',str(bundle)]
        bad=list(prepare);bad[bad.index('--sha256')+1]='0'*64
        assert subprocess.run(bad,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode!=0 and not bundle.exists()
        subprocess.run(prepare,check=True,stdout=subprocess.DEVNULL)
        # The preparation command must not overwrite a pre-existing bundle.
        assert subprocess.run(prepare,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode!=0
        for path in (bundle,bundle/'server.jar',bundle/'repo',bundle/'repo/versions',bundle/'repo/libraries'):
            assert subprocess.run(['setpriv','--reuid=minecraft','--regid=minecraft','--clear-groups','test','-w',str(path)]).returncode!=0
        shutil.copyfile(a.eula,world/'eula.txt');os.chown(world/'eula.txt',user.pw_uid,user.pw_gid)
        props=world/'server.properties'
        props.write_text('server-ip=127.0.0.1\nserver-port=0\nonline-mode=true\nwhite-list=true\nenforce-whitelist=true\nmanagement-server-enabled=false\nenable-rcon=false\nview-distance=2\nsimulation-distance=2\n')
        os.chown(props,user.pw_uid,user.pw_gid)
        log=top/'server.log'
        with log.open('wb') as output:
            proc=subprocess.Popen(['setpriv','--no-new-privs','--bounding-set=-all','--reuid=minecraft','--regid=minecraft','--clear-groups','/opt/java/bin/java','-Xms128M','-Xmx768M','-DbundlerRepoDir='+str(bundle/'repo'),'-jar',str(bundle/'server.jar'),'nogui'],cwd=world,stdin=subprocess.PIPE,stdout=output,stderr=subprocess.STDOUT,start_new_session=True)
            try:
                deadline=time.monotonic()+150
                while time.monotonic()<deadline:
                    if proc.poll() is not None:raise RuntimeError('isolated server exited before readiness')
                    if b'Done (' in log.read_bytes():break
                    time.sleep(.3)
                else:raise RuntimeError('isolated server did not become ready')
                verify.assert_loopback_listeners(proc.pid)
                proc.stdin.write(b'stop\n');proc.stdin.flush()
                if proc.wait(timeout=60)!=0:raise RuntimeError('isolated server shutdown failed')
            finally:
                if proc.poll() is None:os.killpg(proc.pid,signal.SIGKILL);proc.wait()
    print('PROTECTED_SERVER_STARTUP_AND_PERMISSIONS_PASSED')


if __name__=='__main__':main()
