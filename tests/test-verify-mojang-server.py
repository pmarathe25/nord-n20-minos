#!/usr/bin/env python3
"""Check publisher origin and full-download comparison without network access."""
import contextlib
import hashlib
import importlib.machinery
import io
import json
from pathlib import Path
import sys
import tempfile
import urllib.request

m=importlib.machinery.SourceFileLoader('n20_verify_mojang',str(Path(__file__).resolve().parents[1]/'tools/verify-mojang-server.py')).load_module()
for url in ('http://piston-meta.mojang.com/a','https://piston-meta.mojang.com.evil.invalid/a','https://user@piston-meta.mojang.com/a','https://piston-meta.mojang.com:444/a'):
    try:m.require_host(url,'piston-meta.mojang.com');raise AssertionError('unsafe URL accepted')
    except ValueError:pass
handler=m.PublisherRedirect('piston-meta.mojang.com')
try:handler.redirect_request(urllib.request.Request(m.MANIFEST),None,302,'',{},'https://evil.invalid/');raise AssertionError('unsafe redirect accepted')
except ValueError:pass
payload=b'official server bytes';sha1=hashlib.sha1(payload).hexdigest()
version={'id':'test-version','downloads':{'server':{'url':'https://piston-data.mojang.com/server','sha1':sha1,'size':len(payload)}}}
raw=json.dumps(version).encode();manifest={'versions':[{'id':'test-version','url':'https://piston-meta.mojang.com/version','sha1':hashlib.sha1(raw).hexdigest()}]}
m.metadata=lambda url:json.dumps(manifest).encode() if url==m.MANIFEST else raw
m.open_publisher=lambda url,host:io.BytesIO(payload)
with tempfile.TemporaryDirectory(prefix='n20-publisher-test-') as tmp:
    jar=Path(tmp)/'server.jar';jar.write_bytes(payload)
    original=sys.argv
    try:
        sys.argv=['verify','--jar',str(jar),'--version','test-version']
        with contextlib.redirect_stdout(io.StringIO()) as out:m.main()
        assert hashlib.sha256(payload).hexdigest() in out.getvalue()
        jar.write_bytes(b'different local bytes')
        try:m.main();raise AssertionError('different local JAR accepted')
        except ValueError:pass
        jar.unlink();jar.symlink_to('/etc/passwd')
        try:m.main();raise AssertionError('symlink JAR accepted')
        except OSError:pass
    finally:sys.argv=original
print('MOJANG_PUBLISHER_ORIGIN_AND_ARTIFACT_CHECK_PASSED')
