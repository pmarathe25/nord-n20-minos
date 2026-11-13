"""Private event wakeups for backup workers on the phone."""
import ctypes
import os
from pathlib import Path
import select
import stat
import tempfile
import time


def event_directory(run,channel):
    if channel not in ('scheduler','drive'): raise ValueError('Invalid backup wake channel')
    parent=Path(run)/'backup-events'
    for path in (parent,parent/channel):
        path.mkdir(mode=0o700,parents=True,exist_ok=True)
        info=path.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid!=os.geteuid() or info.st_mode & 0o077:
            raise OSError('Unsafe backup event directory')
    return parent/channel


def signal(run,channel):
    directory=event_directory(run,channel)
    fd,name=tempfile.mkstemp(prefix='.wake-',dir=directory)
    try:
        os.close(fd)
        os.replace(name,directory/'wake')
    finally:
        if os.path.exists(name): os.unlink(name)


class Waiter:
    def __init__(self, run,channel):
        directory=event_directory(run,channel)
        libc=ctypes.CDLL(None,use_errno=True)
        self.fd=libc.inotify_init1(os.O_NONBLOCK|os.O_CLOEXEC)
        if self.fd<0: raise OSError(ctypes.get_errno(),'inotify unavailable')
        # Only atomic replacement of the wake marker needs to wake workers.
        if libc.inotify_add_watch(self.fd,os.fsencode(directory),0x80|0x400|0x800)<0:
            error=ctypes.get_errno();os.close(self.fd);raise OSError(error,'inotify watch unavailable')

    def wait(self, timeout):
        if select.select([self.fd],[],[],max(0,timeout))[0]:
            try: os.read(self.fd,65536)
            except BlockingIOError: pass

    def close(self): os.close(self.fd)


class PollingWaiter:
    """Preserve eventual progress if inotify is unavailable."""
    def wait(self, timeout): time.sleep(min(max(0,timeout),300))
    def close(self): pass


def waiter(run,channel):
    try: return Waiter(run,channel)
    except (OSError,AttributeError): return PollingWaiter()
