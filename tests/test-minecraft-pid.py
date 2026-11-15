"""Exercise the supervisor's PID publication and unprivileged bridge access."""
import os
import importlib.machinery
from pathlib import Path
import pwd
import stat
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
supervisor = (ROOT / 'runtime/usr/local/sbin/minecraft-supervisor').read_text()
start = supervisor.index('publish_server_pid() {')
end = supervisor.index('\n}\n', start) + 2
function = supervisor[start:end]

if os.geteuid() != 0:
    raise SystemExit('Run as root in an isolated temporary directory')
account = pwd.getpwnam('minecraft')
with tempfile.TemporaryDirectory(prefix='n20-pid-test-') as temp:
    root = Path(temp)
    os.chmod(root, 0o755)
    target = root / 'minecraft.pid'
    # Replace an old restrictive file, as can happen after a service upgrade.
    target.write_text('1\n')
    os.chmod(target, 0o600)
    subprocess.run(['/bin/sh', '-c', function + '\npublish_server_pid "$1" "$2"',
                    'pid-test', str(target), str(os.getpid())], check=True)
    info = target.lstat()
    assert stat.S_ISREG(info.st_mode) and info.st_uid == 0 and info.st_gid == account.pw_gid
    assert stat.S_IMODE(info.st_mode) == 0o640 and info.st_nlink == 1
    assert target.read_text() == str(os.getpid()) + '\n'
    bridge = ROOT / 'runtime/usr/local/lib/n20'
    check = '''
import sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
import wiki_chat
path = Path(sys.argv[2])
assert wiki_chat.server_identity(path) is not None
try:
    path.write_text("1\\n")
except PermissionError:
    pass
else:
    raise AssertionError("Minecraft account could modify root-owned PID")
'''
    subprocess.run(['runuser', '-u', 'minecraft', '--', sys.executable, '-c', check,
                    str(bridge), str(target)], check=True)
    # Publication must not treat a directory as the PID file.
    bad = root / 'directory'
    bad.mkdir()
    result = subprocess.run(['/bin/sh', '-c', function + '\npublish_server_pid "$1" "$2"',
                             'pid-test', str(bad), str(os.getpid())])
    assert result.returncode != 0

    # A live Wiki supervisor can repair a PID created by a still-running old
    # server supervisor without allowing link-based access to other files.
    wiki = importlib.machinery.SourceFileLoader(
        'wiki_supervisor_pid_test', str(ROOT / 'runtime/usr/local/sbin/n20-wiki-supervisor')).load_module()
    os.chown(target, 0, 0)
    os.chmod(target, 0o600)
    assert wiki.ensure_server_pid_access(target)
    info = target.lstat()
    assert info.st_uid == 0 and info.st_gid == account.pw_gid and stat.S_IMODE(info.st_mode) == 0o640
    link = root / 'link'
    link.symlink_to(target)
    assert not wiki.ensure_server_pid_access(link)
    hardlink = root / 'hardlink'
    os.link(target, hardlink)
    os.chmod(target, 0o600)
    assert not wiki.ensure_server_pid_access(target)
    assert stat.S_IMODE(target.stat().st_mode) == 0o600
print('Minecraft PID publication and bridge access passed')
