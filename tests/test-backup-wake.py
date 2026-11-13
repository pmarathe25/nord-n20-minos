#!/usr/bin/env python3
"""Test event delivery to both backup workers using disposable directories."""
import importlib.machinery
import select
import tempfile
from pathlib import Path

source=Path(__file__).resolve().parents[1]/'runtime/usr/local/lib/n20/backup_wake.py'
m=importlib.machinery.SourceFileLoader('backup_wake_test',str(source)).load_module()
with tempfile.TemporaryDirectory(prefix='n20-backup-wake-') as root:
    first=m.Waiter(root,'scheduler');second=m.Waiter(root,'scheduler');drive=m.Waiter(root,'drive')
    try:
        assert not select.select([first.fd,second.fd,drive.fd],[],[],0)[0]
        m.signal(root,'scheduler')
        assert set(select.select([first.fd,second.fd],[],[],1)[0])=={first.fd,second.fd}
        assert not select.select([drive.fd],[],[],0)[0]
        first.wait(0);second.wait(0)
        assert not select.select([first.fd,second.fd],[],[],0)[0]
        m.signal(root,'drive')
        assert select.select([drive.fd],[],[],1)[0]
        assert not select.select([first.fd,second.fd],[],[],0)[0]
    finally:first.close();second.close();drive.close()
with tempfile.TemporaryDirectory(prefix='n20-backup-wake-') as root:
    (Path(root)/'backup-events').symlink_to(Path(root))
    try:m.signal(root,'scheduler');raise AssertionError('symlink event directory accepted')
    except OSError:pass
print('BACKUP_WAKE_EVENT_DELIVERY_PASSED')
