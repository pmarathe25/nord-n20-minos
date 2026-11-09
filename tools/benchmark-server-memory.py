#!/usr/bin/env python3
"""Compare idle heaps with disposable loopback-only Minecraft servers.

Run as root on the phone, with an existing server JAR and accepted EULA. This
is a low-load comparison, not a multiplayer, gameplay or battery benchmark.
"""
import argparse
import os
from pathlib import Path
import pwd
import shutil
import signal
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('--jar', required=True, type=Path)
parser.add_argument('--eula', required=True, type=Path)
parser.add_argument('--timeout', type=int, default=180)
args = parser.parse_args()
if os.geteuid() != 0: parser.error('must run as root')
jar = args.jar.resolve(strict=True)
if 'eula=true' not in args.eula.read_text().splitlines(): parser.error('EULA must already be accepted')
user = pwd.getpwnam('minecraft')

def ticks(pid):
    text = Path('/proc/%d/stat' % pid).read_text()
    fields = text[text.rfind(')')+2:].split()
    return int(fields[11])+int(fields[12])

def rss(pid):
    for line in Path('/proc/%d/status' % pid).read_text().splitlines():
        if line.startswith('VmRSS:'): return int(line.split()[1])
    raise RuntimeError('RSS unavailable')

with tempfile.TemporaryDirectory(prefix='n20-heap-bench-',dir='/tmp') as tmp:
    top = Path(tmp); top.chmod(0o755)
    for heap in (512, 128):
        root = top / str(heap); root.mkdir()
        os.chown(root, user.pw_uid, user.pw_gid)
        eula = root / 'eula.txt'; shutil.copyfile(args.eula, eula); os.chown(eula,user.pw_uid,user.pw_gid)
        props = root / 'server.properties'
        props.write_text('server-ip=127.0.0.1\nserver-port=0\nonline-mode=true\nwhite-list=true\n'
                         'enforce-whitelist=true\nmanagement-server-enabled=false\nenable-rcon=false\n'
                         'view-distance=6\nsimulation-distance=4\npause-when-empty-seconds=1\nlevel-seed=73111\n')
        os.chown(props,user.pw_uid,user.pw_gid); props.chmod(0o600)
        log = root / 'server.log'
        with log.open('wb') as output:
            start=time.monotonic()
            proc=subprocess.Popen(['/usr/bin/setpriv','--no-new-privs','--bounding-set=-all',
                  '--reuid=minecraft','--regid=minecraft','--clear-groups',
                  '/opt/java/bin/java','-Xms%dM'%heap,'-Xmx2G','-jar',str(jar),'nogui'],
                  cwd=root,stdin=subprocess.PIPE,stdout=output,stderr=subprocess.STDOUT,start_new_session=True)
            try:
                deadline=start+args.timeout; ready=None; paused=False
                while time.monotonic()<deadline:
                    if proc.poll() is not None: raise RuntimeError('benchmark server exited before pause')
                    content=log.read_bytes()
                    if ready is None and b'Done (' in content: ready=time.monotonic()-start
                    if ready is not None and b'pausing' in content: paused=True; break
                    time.sleep(.25)
                if not paused: raise RuntimeError('benchmark server did not reach paused state')
                time.sleep(3)
                first=ticks(proc.pid); memory=rss(proc.pid);time.sleep(10)
                cpu=(ticks(proc.pid)-first)/os.sysconf('SC_CLK_TCK')
                proc.stdin.write(b'stop\n');proc.stdin.flush()
                if proc.wait(timeout=60)!=0: raise RuntimeError('benchmark server did not stop cleanly')
                print('xms_mib=%d startup_s=%.2f paused_rss_kib=%d paused_cpu_s_per_10s=%.3f'%(heap,ready,memory,cpu),flush=True)
            finally:
                if proc.poll() is None:
                    os.killpg(proc.pid,signal.SIGTERM)
                    try:proc.wait(timeout=10)
                    except subprocess.TimeoutExpired:os.killpg(proc.pid,signal.SIGKILL);proc.wait()
