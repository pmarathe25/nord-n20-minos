#!/usr/bin/env python3
"""Compare heap settings using disposable, loopback-only active worlds.

Run as root with explicit existing JAR and accepted EULA paths. This produces a
synthetic chunk workload, not a gameplay, latency or battery-life benchmark.
"""
import argparse
import ipaddress
import os
from pathlib import Path
import pwd
import shutil
import signal
import subprocess
import tempfile
import time

parser=argparse.ArgumentParser()
parser.add_argument('--jar',required=True,type=Path)
parser.add_argument('--eula',required=True,type=Path)
parser.add_argument('--timeout',type=int,default=180)
parser.add_argument('--reverse',action='store_true',help='run 128 MiB before 512 MiB')
args=parser.parse_args()
if os.geteuid()!=0:parser.error('must run as root')
jar=args.jar.resolve(strict=True)
if 'eula=true' not in args.eula.read_text().splitlines():parser.error('EULA must already be accepted')
user=pwd.getpwnam('minecraft')

def ticks(pid):
    text=Path('/proc/%d/stat'%pid).read_text();fields=text[text.rfind(')')+2:].split()
    return int(fields[11])+int(fields[12])

def rss(pid):
    for line in Path('/proc/%d/status'%pid).read_text().splitlines():
        if line.startswith('VmRSS:'):return int(line.split()[1])
    raise RuntimeError('RSS unavailable')

def assert_loopback_listeners(pid):
    inodes=set()
    for fd in Path('/proc/%d/fd'%pid).iterdir():
        try: target=os.readlink(fd)
        except FileNotFoundError: continue
        if target.startswith('socket:[') and target.endswith(']'):inodes.add(target[8:-1])
    listeners=0
    for version in ('tcp','tcp6'):
        for line in Path('/proc/net',version).read_text().splitlines()[1:]:
            fields=line.split()
            if fields[3]!='0A' or fields[9] not in inodes:continue
            encoded=fields[1].split(':',1)[0]
            if version=='tcp':address=ipaddress.IPv4Address(bytes.fromhex(encoded)[::-1])
            else:address=ipaddress.IPv6Address(b''.join(bytes.fromhex(encoded[i:i+8])[::-1] for i in range(0,32,8)))
            if not (address.is_loopback or (getattr(address,'ipv4_mapped',None) and address.ipv4_mapped.is_loopback)):
                raise RuntimeError('benchmark server exposed a non-loopback listener')
            listeners+=1
    if not listeners:raise RuntimeError('benchmark server has no verified TCP listener')

with tempfile.TemporaryDirectory(prefix='n20-active-heap-',dir='/tmp') as temp:
    top=Path(temp);top.chmod(0o755)
    for heap in ((128,512) if args.reverse else (512,128)):
        root=top/str(heap);root.mkdir();os.chown(root,user.pw_uid,user.pw_gid)
        eula=root/'eula.txt';shutil.copyfile(args.eula,eula);os.chown(eula,user.pw_uid,user.pw_gid)
        props=root/'server.properties'
        props.write_text('server-ip=127.0.0.1\nserver-port=0\nonline-mode=true\nwhite-list=true\n'
                         'enforce-whitelist=true\nmanagement-server-enabled=false\nenable-rcon=false\n'
                         'view-distance=6\nsimulation-distance=4\npause-when-empty-seconds=0\nlevel-seed=73111\n')
        os.chown(props,user.pw_uid,user.pw_gid);props.chmod(0o600)
        log=root/'server.log'
        with log.open('wb') as output:
            start=time.monotonic()
            proc=subprocess.Popen(['/usr/bin/setpriv','--no-new-privs','--bounding-set=-all',
                    '--reuid=minecraft','--regid=minecraft','--clear-groups','/opt/java/bin/java',
                    '-Xms%dM'%heap,'-Xmx2G','-jar',str(jar),'nogui'],cwd=root,stdin=subprocess.PIPE,
                    stdout=output,stderr=subprocess.STDOUT,start_new_session=True)
            try:
                deadline=start+args.timeout
                while time.monotonic()<deadline:
                    if proc.poll() is not None:raise RuntimeError('benchmark server exited before ready')
                    if b'Done (' in log.read_bytes():break
                    time.sleep(.25)
                else:raise RuntimeError('benchmark server did not become ready')
                ready=time.monotonic()-start
                assert_loopback_listeners(proc.pid)
                proc.stdin.write(b'execute in minecraft:overworld run forceload add -64 -64 63 63\n');proc.stdin.flush()
                start_tick=ticks(proc.pid);work_start=time.monotonic();time.sleep(30)
                if proc.poll() is not None:raise RuntimeError('benchmark server exited during workload')
                cpu=(ticks(proc.pid)-start_tick)/os.sysconf('SC_CLK_TCK');memory=rss(proc.pid);interval=time.monotonic()-work_start
                text=log.read_text(encoding='utf-8',errors='replace')
                if 'force loaded' not in text.lower():raise RuntimeError('chunk workload was not confirmed in the log')
                offset=log.stat().st_size
                proc.stdin.write(b'say N20_TICK_BEGIN\ntick query\nsay N20_TICK_END\n');proc.stdin.flush()
                query=[];query_deadline=time.monotonic()+10
                while time.monotonic()<query_deadline:
                    with log.open('rb') as captured:captured.seek(offset);lines=captured.read().decode('utf-8',errors='replace').splitlines()
                    begin=next((i for i,line in enumerate(lines) if 'N20_TICK_BEGIN' in line),None)
                    end=next((i for i,line in enumerate(lines) if 'N20_TICK_END' in line),None)
                    if begin is not None and end is not None and end>begin:
                        query=lines[begin+1:end];break
                    if proc.poll() is not None:raise RuntimeError('benchmark server exited during tick query')
                    time.sleep(.25)
                proc.stdin.write(b'stop\n');proc.stdin.flush()
                if proc.wait(timeout=60)!=0:raise RuntimeError('benchmark server did not stop cleanly')
                print('xms_mib=%d startup_s=%.2f active_rss_kib=%d active_cpu_s=%.2f interval_s=%.2f'%
                      (heap,ready,memory,cpu,interval),flush=True)
                for line in query[-2:]:print('tick_query:',line,flush=True)
                if not query:print('tick_query: unavailable',flush=True)
            finally:
                if proc.poll() is None:
                    os.killpg(proc.pid,signal.SIGTERM)
                    try:proc.wait(timeout=10)
                    except subprocess.TimeoutExpired:os.killpg(proc.pid,signal.SIGKILL);proc.wait()
