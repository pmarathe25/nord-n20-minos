"""Exercise Wi-Fi command construction without touching the phone's network."""
import ast
import hashlib
from pathlib import Path

source = (Path(__file__).resolve().parents[1] / 'runtime/usr/local/sbin/n20-wifi-manager').read_text()
tree = ast.parse(source)
command = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == 'command')
calls = []
def request(value):
    calls.append(value)
    return '1' if value == 'ADD_NETWORK' else 'OK'
class FakeTime:
    @staticmethod
    def monotonic(): return 123
ns = {'request': request, 'hashlib': hashlib, 'time': FakeTime, 'pending': None,
      'pending_since': 0, 'message': '', 'lastscan': 0}
exec(compile(ast.Module(body=[command], type_ignores=[]), '<wifi-command>', 'exec'), ns)
password = 'example-password'
packet = 'CONNECT {} {} WPA2'.format('TestNetwork'.encode().hex(), password.encode().hex()).encode()
ns['command'](packet)
assert 'SET_NETWORK 1 proto RSN' in calls
assert 'SET_NETWORK 1 pairwise CCMP' in calls
assert not any(password in value for value in calls)
assert calls[-1] == 'SELECT_NETWORK 1' and ns['pending'] == '1'
print('WPA2_NO_LEGACY_FALLBACK_PASSED')
