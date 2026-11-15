#!/usr/bin/env python3
"""Read-only check of the deployed Minecraft account's private-path access."""
import os
from pathlib import Path
import pwd
import subprocess
import sys
import stat

if os.geteuid()!=0 or sys.argv[1:]!=['--check-live']:
    raise SystemExit('run as root with --check-live on the configured phone')

user=pwd.getpwnam('minecraft')
prefix=['/usr/bin/setpriv','--reuid',str(user.pw_uid),'--regid',str(user.pw_gid),
        '--clear-groups','--inh-caps=-all','--bounding-set=-all','--no-new-privs',
        '/usr/bin/test']

def denied(path,flag):
    if not Path(path).exists(): raise SystemExit('Required path unavailable: '+path)
    result=subprocess.run(prefix+[flag,path],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=5)
    if result.returncode==0: raise SystemExit('Minecraft has unexpected access: '+path)
    if result.returncode!=1: raise SystemExit('Could not check access: '+path)

for target in ('/srv/backups/world-history','/var/lib/n20','/run/n20','/run/wpa_supplicant'):
    for flag in ('-r','-w','-x'): denied(target,flag)
for target in ('/etc/n20-backup.pass','/root/.ssh/authorized_keys','/etc/n20-wpa.conf'):
    for flag in ('-r','-w'): denied(target,flag)
host_keys=list(Path('/etc/ssh').glob('ssh_host_*_key'))
if not host_keys: raise SystemExit('No SSH host private keys found')
for target in host_keys:
    for flag in ('-r','-w'): denied(str(target),flag)
for target in ('/','/usr','/usr/local','/usr/local/sbin','/srv/backups',
               '/opt/n20-minecraft','/opt/n20-minecraft/current/server.jar',
               '/opt/n20-minecraft/current/repo'):
    denied(target,'-w')

# Root services also execute system programs and import their libraries.
# Test access as the game account rather than inferring it from mode bits;
# exclude symlinks, whose apparent 0777 mode does not confer replacement rights.
scan_prefix=prefix[:-1]

# A searchable but unreadable directory can contain a writable file whose name
# is known. Enumerate as root, then check each path as the game account. Do not
# follow symlinks or inspect configuration contents.
def walk_error(error):
    raise error

paths=[]
for tree in ('/etc', '/usr', '/opt/java', '/opt/n20-minecraft', '/var/lib/n20', '/run/n20'):
    for directory, subdirs, files in os.walk(tree, followlinks=False, onerror=walk_error):
        paths.append(directory)
        paths.extend(os.path.join(directory, name) for name in subdirs+files
                     if not os.path.islink(os.path.join(directory, name)))
check='import os,sys\nfor p in sys.stdin.buffer.read().split(b"\\0"):\n if p and os.access(p, os.W_OK):\n  sys.stdout.buffer.write(p); break\n'
result=subprocess.run(scan_prefix+['/usr/bin/python3','-c',check],
    input=b'\0'.join(os.fsencode(path) for path in paths),
    stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=30,cwd='/')
if result.returncode:
    raise SystemExit('Could not inspect configuration path permissions')
if result.stdout:
    raise SystemExit('Minecraft can modify configuration path: '+os.fsdecode(result.stdout))

blocks=[]
for directory, subdirs, files in os.walk('/dev', followlinks=False, onerror=walk_error):
    blocks.extend(os.path.join(directory,name) for name in files
                  if stat.S_ISBLK(os.lstat(os.path.join(directory,name)).st_mode))
if '/dev/userdata' not in blocks: raise SystemExit('Required userdata block node unavailable')
# Opening must fail before any bytes can be read, including through raw aliases.
check_raw='''import os,sys
for p in sys.argv[1:]:
 for flags in (os.O_RDONLY,os.O_WRONLY):
  try: fd=os.open(p,flags|os.O_NONBLOCK)
  except PermissionError: continue
  except OSError: raise SystemExit("Unexpected raw-device open error")
  else: os.close(fd); raise SystemExit("Unexpected raw-device access: "+p)
'''
for uid,gid in ((user.pw_uid,user.pw_gid),(61124,61124)):
    identity=['/usr/bin/setpriv','--reuid',str(uid),'--regid',str(gid),'--clear-groups',
              '--inh-caps=-all','--bounding-set=-all','--no-new-privs']
    subprocess.run(identity+['/usr/bin/python3','-c',check_raw]+blocks,check=True,timeout=10,cwd='/')
    for target in ('/etc/n20-drive/rclone.conf','/etc/n20-backup.pass','/var/lib/n20',
                   '/run/n20','/root/.ssh/authorized_keys'):
        if not Path(target).exists(): continue
        for flag in ('-r','-w'):
            result=subprocess.run(identity+['/usr/bin/test',flag,target],timeout=5,cwd='/')
            if result.returncode!=1: raise SystemExit('Private boundary failed: '+target)
print('LIVE_PRIVATE_PATH_ACCESS_DENIED')
bridge=['/usr/bin/setpriv','--reuid',str(user.pw_uid),'--regid',str(user.pw_gid),'--groups=61125',
        '--inh-caps=-all','--bounding-set=-all','--no-new-privs']
subprocess.run(bridge+['/usr/bin/python3','-c',check_raw,'/dev/kgsl-3d0','/dev/ion'],check=True,timeout=10,cwd='/')
print('LIVE_WIKI_BRIDGE_GPU_ACCESS_DENIED')
