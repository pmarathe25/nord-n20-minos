#!/usr/bin/env python3
"""Disposable vanilla 26.3 distance sweep; paired with benchmark-chunk-host.js.

Explicit --stop-production uses the installed idle-checked stop helper, and
restores the prior running state in finally. Never edits production settings or
worlds. Offline authentication is confined to a verified loopback listener.
"""
import argparse
import hashlib
import importlib.machinery
import importlib.util
import ipaddress
import json
import os
from pathlib import Path
import pwd
import select
import signal
import statistics
import subprocess
import tempfile
import time
import sys
import zipfile

REQUIRE_UNPLUGGED = False


def emit(event, **values):
    print(json.dumps(dict(event=event, **values)), flush=True)


def read(path):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return ''


def metrics(pid):
    stat = read('/proc/%s/stat' % pid)
    if not stat:
        raise RuntimeError('benchmark server exited')
    fields = stat[stat.rfind(')') + 2:].split()
    status = dict(line.split(':', 1) for line in read('/proc/%s/status' % pid).splitlines() if ':' in line)
    memory = dict(line.split(':', 1) for line in read('/proc/meminfo').splitlines() if ':' in line)
    battery = '/sys/class/power_supply/battery/'
    result = dict(cpu_s=(int(fields[11]) + int(fields[12])) / os.sysconf('SC_CLK_TCK'),
                  rss_mib=int(status['VmRSS'].split()[0]) / 1024,
                  available_mib=int(memory['MemAvailable'].split()[0]) / 1024,
                  battery_c=float(read(battery + 'temp')) / 10,
                  current_raw=read(battery + 'current_now'),
                  voltage_raw=read(battery + 'voltage_now'),
                  battery_status=read(battery + 'status'),
                  capacity=read(battery + 'capacity'),
                  usb_online=read('/sys/class/power_supply/usb/online'),
                  ac_online=read('/sys/class/power_supply/ac/online'),
                  brightness=read('/sys/class/backlight/panel0-backlight/brightness'))
    if REQUIRE_UNPLUGGED and (result['usb_online'] != '0' or result['ac_online'] != '0'):
        raise RuntimeError('external power appeared during battery comparison')
    if result['available_mib'] < 600 or result['battery_c'] >= 42:
        raise RuntimeError('resource guard: available memory or battery temperature')
    if result['brightness'] != '0':
        raise RuntimeError('display state changed during benchmark: brightness=' + repr(result['brightness']))
    return result


def verify_listener(pid):
    inodes = set()
    for path in Path('/proc/%s/fd' % pid).iterdir():
        try:
            value = os.readlink(path)
            if value.startswith('socket:['):
                inodes.add(value[8:-1])
        except FileNotFoundError:
            pass
    listeners = []
    for name in ('tcp', 'tcp6'):
        for line in Path('/proc/net/' + name).read_text().splitlines()[1:]:
            fields = line.split()
            if fields[3] == '0A' and fields[9] in inodes:
                address, port = fields[1].split(':')
                if name == 'tcp':
                    ip = ipaddress.ip_address(bytes.fromhex(address)[::-1])
                else:
                    ip = ipaddress.ip_address(b''.join(bytes.fromhex(address[i:i+8])[::-1] for i in range(0,32,8)))
                    ip = ip.ipv4_mapped or ip
                listeners.append((str(ip), int(port, 16)))
    if listeners != [('127.0.0.1', 25665)]:
        raise RuntimeError('unexpected benchmark listeners: ' + repr(listeners))


def run_case(top, view, simulation, index, durations, user):
    root = top / ('case-%02d-%d-%d' % (index, view, simulation))
    root.mkdir()
    os.chown(root, user.pw_uid, user.pw_gid)
    props = ('server-ip=127.0.0.1\nserver-port=25665\nonline-mode=false\n'
             'enforce-secure-profile=false\nwhite-list=false\nenable-rcon=false\n'
             'management-server-enabled=false\nenable-query=false\nmax-players=1\n'
             'gamemode=creative\nforce-gamemode=true\nallow-flight=true\ndifficulty=normal\n'
             'view-distance=%d\nsimulation-distance=%d\npause-when-empty-seconds=0\n'
             'level-seed=73111\nplayer-idle-timeout=0\n' % (view, simulation))
    for name, text in (('server.properties', props), ('eula.txt', 'eula=true\n')):
        path = root / name
        path.write_text(text)
        os.chown(path, user.pw_uid, user.pw_gid)
        path.chmod(0o600)
    log = root / 'server.log'
    with log.open('wb') as output:
        proc = subprocess.Popen(['/usr/bin/setpriv', '--no-new-privs', '--bounding-set=-all',
                                 '--reuid=minecraft', '--regid=minecraft', '--clear-groups',
                                 '/opt/java/bin/java', '-Xms512M', '-Xmx2G',
                                 '-DbundlerRepoDir=/opt/n20-minecraft/current/repo',
                                 '-jar', '/opt/n20-minecraft/current/server.jar', 'nogui'],
                                cwd=root, stdin=subprocess.PIPE, stdout=output, stderr=subprocess.STDOUT,
                                start_new_session=True)

        def command(text):
            proc.stdin.write((text + '\n').encode('ascii'))
            proc.stdin.flush()

        try:
            start = time.monotonic()
            while b'Done (' not in log.read_bytes():
                if proc.poll() is not None or time.monotonic() - start > 180:
                    raise RuntimeError('server startup failed: ' + log.read_text(errors='replace')[-1500:])
                time.sleep(.25)
            verify_listener(proc.pid)
            command('setworldspawn 0 100 0')
            emit('ready', case=index, view=view, simulation=simulation, startup_s=time.monotonic()-start)
            deadline = time.monotonic() + 30
            while 'BenchClient joined the game' not in log.read_text(errors='replace'):
                if time.monotonic() > deadline:
                    raise RuntimeError('client did not join: ' + log.read_text(errors='replace')[-2000:])
                time.sleep(.25)
            command('op BenchClient')
            command('tp BenchClient 0 100 0')
            feedback = b''
            last_chunks = -1
            last_change = time.monotonic()
            loaded = 0
            for phase, duration in zip(('initial_load', 'stationary', 'exploration'), durations):
                emit('phase', case=index, phase=phase, seconds=duration)
                begin = time.monotonic()
                baseline = metrics(proc.pid)
                samples = []
                next_move = begin + 2
                x = 0
                offset = log.stat().st_size
                command('tick query')
                while time.monotonic() - begin < duration:
                    now = time.monotonic()
                    if select.select([sys.stdin], [], [], 0)[0]:
                        data = os.read(sys.stdin.fileno(), 65536)
                        if not data:
                            raise RuntimeError('benchmark host disconnected')
                        feedback += data
                        while b'\n' in feedback:
                            line, feedback = feedback.split(b'\n', 1)
                            state = json.loads(line)
                            if state['case'] != index:
                                continue
                            if state['errors']:
                                raise RuntimeError('client failed: ' + repr(state['errors']))
                            if state['closed']:
                                raise RuntimeError('client disconnected during measurement')
                            loaded = state['loaded']
                            if state['chunks'] != last_chunks:
                                last_chunks = state['chunks']
                                last_change = now
                    if phase == 'initial_load' and now-begin >= 30 and loaded > 9 and now-last_change >= 20:
                        break
                    if phase == 'exploration' and now >= next_move:
                        x += 16
                        command('tp BenchClient %d 100 0' % x)
                        next_move += 2
                    samples.append(metrics(proc.pid))
                    time.sleep(min(1, max(0, duration - (time.monotonic() - begin))))
                command('tick query')
                elapsed = time.monotonic() - begin
                final = metrics(proc.pid)
                time.sleep(.3)
                with log.open('rb') as captured:
                    captured.seek(offset)
                    lines = captured.read().decode('utf-8', errors='replace').splitlines()
                selected = [line.split(']: ', 1)[-1] for line in lines if any(s in line.lower() for s in ('tick rate', 'tick time', 'time per tick', 'percentile', "can't keep up", 'mspt'))]
                currents = [float(s['current_raw']) for s in samples if s['current_raw']]
                emit('phase_result', case=index, view=view, simulation=simulation, phase=phase,
                     seconds=elapsed, cpu_cores=(final['cpu_s']-baseline['cpu_s'])/elapsed,
                     chunks_quiet_s=time.monotonic()-last_change,
                     peak_rss_mib=max(s['rss_mib'] for s in samples),
                     min_available_mib=min(s['available_mib'] for s in samples),
                     battery_start_c=baseline['battery_c'], battery_end_c=final['battery_c'],
                     battery_peak_c=max(s['battery_c'] for s in samples),
                     current_raw=sorted(set(s['current_raw'] for s in samples)),
                     current_mean_raw=statistics.mean(currents),
                     current_median_raw=statistics.median(currents),
                     current_min_raw=min(currents), current_max_raw=max(currents),
                     voltage_mean_raw=statistics.mean(float(s['voltage_raw']) for s in samples),
                     capacity_start=baseline['capacity'], capacity_end=final['capacity'],
                     usb_online=sorted(set(s['usb_online'] for s in samples)),
                     ac_online=sorted(set(s['ac_online'] for s in samples)),
                     wifi_power_save=subprocess.run(['iw','dev','wlan0','get','power_save'],capture_output=True,text=True).stdout.strip(),
                     battery_status=sorted(set(s['battery_status'] for s in samples)),
                     tick_queries=selected)
            command('stop')
            if proc.wait(timeout=90) != 0:
                raise RuntimeError('benchmark server did not stop cleanly')
            emit('case_end', case=index)
        finally:
            if proc.poll() is None:
                try:
                    command('stop')
                    proc.wait(timeout=30)
                except (BrokenPipeError, subprocess.TimeoutExpired):
                    os.killpg(proc.pid, signal.SIGTERM)
                    try:
                        proc.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        os.killpg(proc.pid, signal.SIGKILL)
                        proc.wait()


def main():
    global REQUIRE_UNPLUGGED
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stop-production', action='store_true', required=True)
    parser.add_argument('--cases', default='12:8,16:8,24:8,32:8,256:8')
    parser.add_argument('--durations', default='240,60,90')
    parser.add_argument('--require-unplugged', action='store_true')
    args = parser.parse_args()
    cases = [tuple(map(int, case.split(':'))) for case in args.cases.split(',')]
    durations = list(map(int, args.durations.split(',')))
    if any(len(c) != 2 or not all(2 <= n <= 256 for n in c) for c in cases):
        parser.error('distances must be between 2 and 256')
    if len(durations) != 3 or any(not 5 <= n <= 600 for n in durations):
        parser.error('three phase durations between 5 and 600 seconds required')
    if os.geteuid() != 0:
        parser.error('root required')
    REQUIRE_UNPLUGGED = args.require_unplugged
    if REQUIRE_UNPLUGGED and any(read('/sys/class/power_supply/'+name+'/online') != '0' for name in ('usb','ac')):
        parser.error('unplug external power before starting a battery comparison')
    loader = importlib.machinery.SourceFileLoader('installed_admin', '/usr/local/sbin/n20-admin')
    spec = importlib.util.spec_from_loader(loader.name, loader)
    admin = importlib.util.module_from_spec(spec)
    sys.dont_write_bytecode = True
    loader.exec_module(admin)
    with zipfile.ZipFile('/opt/n20-minecraft/current/repo/versions/26.3/server-26.3.jar') as jar:
        version = json.loads(jar.read('version.json'))
        if version['id'] != '26.3' or version['protocol_version'] != 777:
            raise RuntimeError('benchmark protocol supports only vanilla 26.3 / protocol 777')
    if 'eula=true' not in admin.read_server_file('eula.txt', 65536)[0].splitlines():
        raise RuntimeError('accepted EULA missing')
    before = hashlib.sha256(admin.read_server_file('server.properties')[0].encode()).digest()
    was_running, had_stop = admin.stop_for_snapshot(require_idle=True)
    emit('production_stopped')
    try:
        with tempfile.TemporaryDirectory(prefix='n20-chunk-benchmark-', dir='/tmp') as temp:
            top = Path(temp)
            top.chmod(0o755)
            user = pwd.getpwnam('minecraft')
            for index, (view, simulation) in enumerate(cases):
                emit('case_start', case=index, view=view, simulation=simulation)
                run_case(top, view, simulation, index, durations, user)
    finally:
        # Restore the stop marker even if validation of a replaced file fails.
        try:
            unchanged = hashlib.sha256(admin.read_server_file('server.properties')[0].encode()).digest() == before
        except (OSError, UnicodeError):
            unchanged = False
        if not had_stop:
            admin.STOP.unlink(missing_ok=True)
        deadline = time.monotonic() + 120
        ready = not was_running
        while was_running and time.monotonic() < deadline:
            try:
                if admin.online_count(admin.mc_status()) >= 0:
                    ready = True
                    break
            except Exception:
                pass
            time.sleep(1)
        emit('restored', configuration_unchanged=unchanged, server_running=bool(admin.pid_running()), server_ready=ready,
             brightness=read('/sys/class/backlight/panel0-backlight/brightness'),
             wifi_power_save=subprocess.run(['iw', 'dev', 'wlan0', 'get', 'power_save'], capture_output=True, text=True).stdout.strip())
        if not ready or not unchanged:
            raise RuntimeError('production restoration verification failed')


if __name__ == '__main__':
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    signal.signal(signal.SIGHUP, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    main()
