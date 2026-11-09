#!/usr/bin/env python3
"""Exercise agreement and fail-closed checks without network or clock changes."""
import importlib.machinery
import sys
import types
from pathlib import Path

probe = importlib.machinery.SourceFileLoader(
    'roughtime_probe', str(Path(__file__).resolve().parents[1] / 'tools/roughtime-probe.py')
).load_module()
base = 1800000000
assert abs(probe.agreement([(base, 3, 99), (base + 1, 3, 100)], 100) - base) <= 2
for samples in (
    [(base, 3, 100)],
    [(base, 3, 100), (base + 100, 3, 100)],
    [(base, 17, 100), (base, 3, 100)],
    [(base, 3, 89), (base, 3, 100)],
    [(100, 3, 100), (100, 3, 100)],
):
    try:
        probe.agreement(samples, 100)
    except ValueError:
        continue
    raise AssertionError('invalid sample accepted')

class FakeResult:
    version = '1'
    versions = '1'
    radius = 3
    def ver(self): return self.version
    def vers(self): return self.versions
    def radi(self): return self.radius
    def midp(self): return base

class FakeClient:
    def query(self, *args, **kwargs): return fake_result

fake_result = FakeResult()
original = sys.modules.get('pyroughtime')
sys.modules['pyroughtime'] = types.SimpleNamespace(RoughtimeClient=FakeClient)
try:
    assert probe.query(probe.SERVERS[0])[0] == base
    for version, versions, radius in [('2','1 2',3),('1','2',3),('1','1',0),('1','1',11)]:
        fake_result.version, fake_result.versions, fake_result.radius = version, versions, radius
        try: probe.query(probe.SERVERS[0])
        except ValueError: continue
        raise AssertionError('invalid protocol response accepted')
finally:
    if original is None: del sys.modules['pyroughtime']
    else: sys.modules['pyroughtime'] = original
print('ROUGHTIME_AGREEMENT_PASSED')
