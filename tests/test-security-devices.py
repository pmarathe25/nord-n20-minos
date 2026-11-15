"""Raw-node identity/permission tests; root, isolated fake /dev and sysfs."""
import importlib.machinery
import os
from pathlib import Path
import stat
import tempfile

m=importlib.machinery.SourceFileLoader('security_devices',str(Path(__file__).resolve().parents[1]/'runtime/usr/local/sbin/n20-security-devices')).load_module()
with tempfile.TemporaryDirectory(prefix='n20-device-security-') as tmp:
    root=Path(tmp)
    (root/'dev').mkdir()
    node=root/'sys/class/block/test'
    node.mkdir(parents=True)
    (node/'dev').write_text('259:0\n')
    (node/'uevent').write_text('PARTNAME=userdata\n')
    for name in ('userdata','alias'):
        os.mknod(root/'dev'/name,stat.S_IFBLK|0o644,os.makedev(259,0))
    m.secure(root)
    assert all(stat.S_IMODE(p.stat().st_mode)==0o600 for p in (root/'dev').iterdir())
    (node/'dev').write_text('259:1\n')
    try:m.secure(root)
    except RuntimeError:pass
    else:raise AssertionError('wrong identity accepted')
    (root/'dev/userdata').unlink()
    (root/'dev/userdata').symlink_to('alias')
    try:m.secure(root)
    except RuntimeError:pass
    else:raise AssertionError('symlink userdata accepted')
print('RAW_DEVICE_SECURITY_PASSED')
