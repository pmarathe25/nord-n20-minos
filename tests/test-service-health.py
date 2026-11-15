"""Bounded incident history and cgroup/Java-heap OOM attribution."""
import importlib.machinery
import json
from pathlib import Path
import tempfile
from unittest.mock import patch

root=Path(__file__).resolve().parents[1]
m=importlib.machinery.SourceFileLoader('service_health',str(root/'runtime/usr/local/sbin/n20-service-health')).load_module()
with tempfile.TemporaryDirectory(prefix='n20-health-') as tmp:
    area=Path(tmp);log=area/'java.log'
    log.write_text('[INFO] <Player> java.lang.OutOfMemoryError: Java heap space\n')
    assert not m.heap_failure(log)
    log.write_text('java.lang.OutOfMemoryError: Java heap space\n')
    assert m.heap_failure(log)=='Java heap exhausted'
    assert not m.heap_failure(log,log.stat().st_size)
    with patch.object(m,'oom_count',return_value=1):m.record('minecraft','started',meta=area)
    with patch.object(m,'oom_count',return_value=2):m.record('minecraft','exited',137,meta=area)
    data=json.loads((area/'service-health.json').read_text())
    assert data['events'][-1]['reason']=='cgroup OOM kill'
    m.record('wiki-model','backoff',meta=area)
    before=(area/'service-health.json').read_bytes()
    for _ in range(100):m.record('wiki-model','backoff',meta=area)
    assert (area/'service-health.json').read_bytes()==before
    for i in range(50):m.record('wiki-model','exited',i+1,meta=area)
    assert len(json.loads((area/'service-health.json').read_text())['events'])==32
    cg=area/'groups'
    for name in ('n20-optional/n20-wiki-model','n20-optional/n20-wiki-bridge','n20-optional'):
        p=cg/name;p.mkdir(parents=True,exist_ok=True);(p/'memory.oom_control').write_text('oom_kill 1\n')
    assert m.oom_count('wiki-model',cg)==2 and m.oom_count('wiki-launcher',cg)==2
print('BOUNDED_SERVICE_OOM_AND_HEAP_DIAGNOSTICS_PASSED')
