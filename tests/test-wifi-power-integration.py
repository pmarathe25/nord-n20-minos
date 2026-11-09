#!/usr/bin/env python3
"""Exercise Wi-Fi policy and watchdog using a fake radio and TCP table."""
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time

source = Path(__file__).resolve().parents[1] / 'runtime/usr/local/sbin/n20-wifi-power-policy'

def until(check, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if check(): return
        time.sleep(.01)
    raise AssertionError('timed out waiting for policy transition')

with tempfile.TemporaryDirectory(prefix='n20-wifi-power-integration-') as tmp:
    root = Path(tmp); run = root / 'run'; run.mkdir()
    proc = root / 'proc'; process = proc / '123'; (process / 'fd').mkdir(parents=True)
    (process / 'comm').write_text('java\n'); os.symlink('socket:[42]', process / 'fd' / '4')
    pid = root / 'pid'; pid.write_text('123\n')
    config = root / 'server.properties'; config.write_text('server-port=25565\n')
    net = root / 'net'; net.mkdir(); (net / 'tcp').write_text('header\n')
    listener = '0: 00000000:63DD 00000000:0000 0A 0 0 0 0 0 42\n'
    client = '1: 00000000:63DD 00000000:0000 01 0 0 0 0 0 43\n'
    table = net / 'tcp6'; table.write_text('header\n' + listener)
    state = root / 'power'; state.write_text('off')
    bindir = root / 'bin'; bindir.mkdir(); iw = bindir / 'iw'
    iw.write_text('#!/usr/bin/python3\nimport pathlib,sys\np=pathlib.Path(' + repr(str(state)) + ')\n'
                  'a=sys.argv[1:]\n'
                  'if a[:4]==["dev","wlan0","get","power_save"]: print("Power save: "+p.read_text())\n'
                  'elif a[:4]==["dev","wlan0","set","power_save"]: p.write_text(a[4])\n'
                  'else: sys.exit(2)\n')
    iw.chmod(0o755)
    text = source.read_text()
    changes = {
        "RUN = Path('/run/n20')": 'RUN = Path(' + repr(str(run)) + ')',
        "PROPERTIES = Path('/srv/minecraft/server.properties')": 'PROPERTIES = Path(' + repr(str(config)) + ')',
        "NET = Path('/proc/net')": 'NET = Path(' + repr(str(net)) + ')',
        "PROC = Path('/proc')": 'PROC = Path(' + repr(str(proc)) + ')',
        "PID = Path('/run/minecraft.pid')": 'PID = Path(' + repr(str(pid)) + ')',
        'IDLE_DELAY = 10.0': 'IDLE_DELAY = 0.3',
        'INTERVAL = 0.5': 'INTERVAL = 0.05',
        'CHECK_INTERVAL = 10.0': 'CHECK_INTERVAL = 0.2',
        'time.monotonic() - started > 8': 'time.monotonic() - started > 0.8',
        'time.sleep(1)': 'time.sleep(0.1)',
        'if not stopping: time.sleep(3)': 'if not stopping: time.sleep(0.2)',
    }
    for old, new in changes.items():
        assert old in text, old
        text = text.replace(old, new)
    script = root / 'policy.py'; script.write_text(text)
    env = os.environ.copy(); env['PATH'] = str(bindir) + ':/usr/bin:/bin'
    guard = subprocess.Popen(['/usr/bin/python3', str(script)], env=env,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    child = None
    def state_is(value): return state.read_text() == value
    def worker_pid():
        p = subprocess.run(['pgrep', '-P', str(guard.pid)], capture_output=True, text=True)
        values = p.stdout.split()
        return int(values[0]) if values else None
    try:
        until(lambda: state_is('on'))
        table.write_text('header\n' + listener + client)
        until(lambda: state_is('off'))
        table.write_text('header\n' + listener)
        until(lambda: state_is('on'))
        until(lambda: worker_pid() is not None); child = worker_pid()
        os.kill(child, signal.SIGKILL)
        until(lambda: state_is('off'))
        until(lambda: state_is('on'))
        until(lambda: worker_pid() not in (None, child)); child = worker_pid()
        os.kill(child, signal.SIGSTOP)
        heartbeat = run / 'wifi-power-policy.heartbeat'
        os.utime(heartbeat, (time.time() - 30, time.time() - 30))
        until(lambda: state_is('off'), timeout=6)
        until(lambda: state_is('on'), timeout=6)
        until(lambda: worker_pid() not in (None, child)); child = worker_pid()
        os.kill(guard.pid, signal.SIGKILL); guard.wait(timeout=3)
        until(lambda: state_is('off'))
        print('WIFI_POWER_CONNECTION_AND_CRASH_FAILSAFE_PASSED')
    finally:
        if guard.poll() is None:
            guard.terminate(); guard.wait(timeout=5)
        if child:
            try: os.kill(child, signal.SIGTERM)
            except ProcessLookupError: pass
