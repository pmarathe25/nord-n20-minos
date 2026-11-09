import importlib.machinery
import json
import os
import socket
import struct
import tempfile
from pathlib import Path
m=importlib.machinery.SourceFileLoader('n20_save_events',str(Path(__file__).resolve().parents[1]/'runtime/usr/local/sbin/n20-save-events')).load_module()
with tempfile.TemporaryDirectory(prefix='n20-save-secret-') as temp:
    root=Path(temp);source=root/'source';source.write_text('management-server-secret=example\n');m.PROPS=root/'properties'
    m.PROPS.symlink_to(source)
    try:m.secret();raise AssertionError('symlink properties accepted')
    except OSError:pass
    m.PROPS.unlink();os.link(source,m.PROPS)
    try:m.secret();raise AssertionError('hardlinked properties accepted')
    except ValueError:pass
    m.PROPS.unlink();os.mkfifo(m.PROPS)
    try:m.secret();raise AssertionError('FIFO properties accepted')
    except ValueError:pass
    m.PROPS.unlink();source.rename(m.PROPS);assert m.secret()=='example'
for value in (None, [], 'x', 4, True): assert not m.valid_message(value)
assert m.valid_message({'method':'minecraft:notification/server/saved'})
assert m.bounded_json('{"message":"[{}]\\\""}')=={'message':'[{}]"'}
a,b=socket.socketpair()
try:
    payload=json.dumps([1,2]).encode(); a.sendall(bytes([0x81,len(payload)])+payload)
    assert not m.valid_message(next(m.frames(b)))
    a.sendall(b'\x81\x05ab'); a.close()
    try: next(m.frames(b)); raise AssertionError('truncated frame accepted')
    except EOFError: pass
finally: a.close(); b.close()
a,b=socket.socketpair()
try:
    payload=b'['*2000+b'0'+b']'*2000
    a.sendall(b'\x81\x7e'+struct.pack('>H',len(payload))+payload)
    try: next(m.frames(b));raise AssertionError('deeply nested JSON accepted')
    except ValueError:pass
finally: a.close(); b.close()
a,b=socket.socketpair()
try:
    a.sendall(b'\x01\x01{\x80\x01}')
    assert next(m.frames(b))=={}
finally: a.close(); b.close()

class StalledSocket:
    def __init__(self): self.calls=0;self.timeouts=[]
    def recv(self,n):
        self.calls+=1
        if self.calls==1:return b'\x81'
        raise socket.timeout('partial frame')
    def settimeout(self,value):self.timeouts.append(value)
stalled=StalledSocket()
try: next(m.frames(stalled));raise AssertionError('stalled frame accepted')
except socket.timeout:pass
assert len(stalled.timeouts)==2 and 0<stalled.timeouts[0]<=10 and stalled.timeouts[1] is None

class StalledFragment(StalledSocket):
    def recv(self,n):
        self.calls+=1
        if self.calls==1:return b'\x01'
        if self.calls==2:return b'\x01'
        if self.calls==3:return bytes([123])
        raise socket.timeout('unfinished fragmented message')
fragment=StalledFragment()
try: next(m.frames(fragment));raise AssertionError('unfinished fragment accepted')
except socket.timeout:pass
assert fragment.timeouts[-1] is not None and 0<fragment.timeouts[-1]<=10

class SlowSocket:
    def settimeout(self,value):pass
    def recv(self,n):return b'x'
real_monotonic=m.time.monotonic
clock=iter([0.0,4.0,8.0,12.0])
try:
    m.time.monotonic=lambda:next(clock)
    try:m.recv_exact(SlowSocket(),4,10.0);raise AssertionError('slow trickle accepted')
    except socket.timeout:pass
finally:m.time.monotonic=real_monotonic
print('MANAGEMENT_MESSAGE_VALIDATION_PASSED')
