"""Exercise event subscription and wakeups without changing Wi-Fi state."""
import ast
from pathlib import Path
import select
import socket
import tempfile
import threading
import time
import os

source = (Path(__file__).resolve().parents[1] / 'runtime/usr/local/sbin/n20-wifi-manager').read_text()
tree = ast.parse(source)
functions = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in ('event_monitor', 'wait_for_activity')]
with tempfile.TemporaryDirectory(prefix='n20-wifi-events-') as tmp:
    ctrl_path = str(Path(tmp) / 'supplicant')
    supp = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    supp.bind(ctrl_path)
    ns = {'RUN': tmp, 'CTRL': ctrl_path, 'socket': socket, 'select': select, 'os': os}
    exec(compile(ast.Module(body=functions, type_ignores=[]), '<wifi-events>', 'exec'), ns)
    def reply():
        data, address = supp.recvfrom(128)
        assert data == b'ATTACH'
        supp.sendto(b'OK\n', address)
        supp.sendto(b'<3>CTRL-EVENT-CONNECTED', address)
    thread = threading.Thread(target=reply)
    thread.start()
    monitor = ns['event_monitor']()
    thread.join(timeout=2)
    assert not thread.is_alive()
    control, client = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
    start = time.monotonic()
    assert ns['wait_for_activity'](control, monitor, 2) == (None, True)
    assert time.monotonic() - start < 1
    client.send(b'SCAN')
    start = time.monotonic()
    assert ns['wait_for_activity'](control, monitor, 2) == (b'SCAN', False)
    assert time.monotonic() - start < 1
    start = time.monotonic()
    assert ns['wait_for_activity'](control, monitor, 0.1) == (None, False)
    assert time.monotonic() - start >= 0.08
    control.close(); client.close(); monitor.close(); supp.close()
print('WIFI_EVENT_WAKEUPS_PASSED')
