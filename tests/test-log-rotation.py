"""Oversize logs rotate with bounded retention and keep live append inode."""
from pathlib import Path
import os
import subprocess
import tempfile

source=Path(__file__).resolve().parents[1]/'runtime/etc/n20-logrotate.conf'
with tempfile.TemporaryDirectory(prefix='n20-logrotate-') as tmp:
    root=Path(tmp);log=root/'auth.log';config=root/'logrotate.conf'
    log.write_text('isolated authentication event\n'*(200000));log.chmod(0o600)
    before=log.stat().st_ino
    config.write_text(source.read_text().replace(source.read_text().splitlines()[0],str(log)+' {'))
    subprocess.run(['logrotate','--state',str(root/'state'),str(config)],check=True)
    assert log.stat().st_size==0 and log.stat().st_ino==before
    assert (root/'auth.log.1.gz').exists() and log.stat().st_mode&0o777==0o600
    for _ in range(5):
        log.write_text('fresh event\n')
        subprocess.run(['logrotate','--force','--state',str(root/'state'),str(config)],check=True)
    assert len(list(root.glob('auth.log.*.gz')))==4
print('PROTECTED_LOG_ROTATION_PASSED')
