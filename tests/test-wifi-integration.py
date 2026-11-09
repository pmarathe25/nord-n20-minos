"""Integration test with a fake supplicant and DHCP, never the host network."""
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import threading
import time

SOURCE = Path(__file__).resolve().parents[1] / 'runtime/usr/local/sbin/n20-wifi-manager'

def until(predicate, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate(): return
        time.sleep(.05)
    raise AssertionError('Timed out waiting for expected Wi-Fi manager state')

with tempfile.TemporaryDirectory(prefix='n20-wifi-integration-') as tmp:
    root = Path(tmp)
    run = root / 'run'; run.mkdir()
    wpa_dir = root / 'wpa'; wpa_dir.mkdir()
    ctrl = wpa_dir / 'wlan0'
    wlan = root / 'wlan0'; wlan.mkdir()
    config = root / 'config'; config.write_text('test\n')
    dhcp_log = root / 'dhcp-events'
    dhcp = root / 'fake-dhcp'
    dhcp.write_text('#!/usr/bin/python3\nimport os,signal,sys,time\np=' + repr(str(dhcp_log)) + '\n'
        'with open(p,"a") as f: f.write(("deconfig" if sys.argv[1:] == ["deconfig"] else "start " + str(os.getpid())) + "\\n")\n'
        'if sys.argv[1:] != ["deconfig"]:\n while True: time.sleep(60)\n')
    dhcp.chmod(0o755)
    src = SOURCE.read_text()
    replacements = {
        "RUN = '/run/n20'": 'RUN = ' + repr(str(run)),
        "CTRL = '/run/wpa_supplicant/wlan0'": 'CTRL = ' + repr(str(ctrl)),
        "os.makedirs('/run/wpa_supplicant', exist_ok=True)": 'pass',
        "config = '/etc/n20-wpa.conf'": 'config = ' + repr(str(config)),
        "'/sys/class/net/wlan0'": repr(str(wlan)),
        "'/var/log/n20-wifi-dhcp.log'": repr(str(root / 'process.log')),
        "'/usr/local/bin/busybox'": repr(str(dhcp)),
        "'/usr/local/sbin/n20-wifi-dhcp'": repr(str(dhcp)),
    }
    for old, new in replacements.items():
        assert old in src, old
        src = src.replace(old, new)
    manager_path = root / 'manager.py'; manager_path.write_text(src)
    server = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM); server.bind(str(ctrl)); server.settimeout(.1)
    state = {'connected': True, 'running': True, 'monitor': None, 'commands': []}
    def send_event():
        if state['monitor']:
            server.sendto(b'<3>CTRL-EVENT-STATE-CHANGE', state['monitor'])
    def fake_supplicant():
        while state['running']:
            try: data, address = server.recvfrom(65535)
            except socket.timeout: continue
            cmd = data.decode(); state['commands'].append(cmd)
            if cmd == 'ATTACH': state['monitor'] = address; response = 'OK'
            elif cmd == 'STATUS': response = 'wpa_state=' + ('COMPLETED\nid=1\nssid=Test' if state['connected'] else 'DISCONNECTED')
            elif cmd == 'SCAN_RESULTS': response = 'bssid / frequency / signal level / flags / ssid\n'
            elif cmd == 'ADD_NETWORK': response = '1'
            else: response = 'OK'
            server.sendto(response.encode(), address)
            if cmd == 'DISCONNECT': state['connected'] = False; send_event()
            elif cmd.startswith('SELECT_NETWORK '): state['connected'] = True; send_event()
            elif cmd == 'SCAN': send_event()
    thread = threading.Thread(target=fake_supplicant, daemon=True); thread.start()
    manager = subprocess.Popen(['/usr/bin/python3', str(manager_path)], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    client = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    try:
        def log_lines(): return dhcp_log.read_text().splitlines() if dhcp_log.exists() else []
        def starts(): return [int(x.split()[1]) for x in log_lines() if x.startswith('start ')]
        until(lambda: len(starts()) == 1 and (run / 'wifi-control').exists())
        client.sendto(b'SCAN', str(run / 'wifi-control'))
        until(lambda: 'SCAN' in state['commands'])
        client.sendto(b'DISCONNECT', str(run / 'wifi-control'))
        until(lambda: 'deconfig' in log_lines())
        packet = b'CONNECT 54657374 70617373776f7264 WPA2'
        client.sendto(packet, str(run / 'wifi-control'))
        until(lambda: 'SAVE_CONFIG' in state['commands'] and len(starts()) == 2)
        os.kill(starts()[-1], signal.SIGTERM)
        until(lambda: len(starts()) == 3, timeout=18)
        assert manager.poll() is None
        print('WIFI_CONNECTION_AND_DHCP_RECOVERY_PASSED')
    finally:
        manager.terminate()
        try: manager.wait(timeout=3)
        except subprocess.TimeoutExpired: manager.kill(); manager.wait()
        for pid in starts():
            try:
                # Do not signal an unrelated process if a test PID was reused.
                if str(dhcp).encode() in Path('/proc/%d/cmdline' % pid).read_bytes():
                    os.kill(pid, signal.SIGTERM)
            except (ProcessLookupError, FileNotFoundError): pass
        client.close(); state['running'] = False; thread.join(timeout=1); server.close()
