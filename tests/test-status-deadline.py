#!/usr/bin/env python3
"""Bound a slow server-status response without opening a network socket."""
import importlib.machinery
from pathlib import Path
import socket

m=importlib.machinery.SourceFileLoader('n20_status',str(Path(__file__).resolve().parents[1]/'runtime/usr/local/sbin/n20-admin')).load_module()
m.status_endpoint=lambda:('127.0.0.1',25565)

class FakeSocket:
    def __init__(self,data):self.data=bytearray(data);self.timeouts=[];self.sent=b''
    def __enter__(self):return self
    def __exit__(self,*args):pass
    def settimeout(self,value):self.timeouts.append(value)
    def sendall(self,data):self.sent+=data
    def recv(self,n):
        if not self.data:return b''
        # Deliver one byte at a time to model a slow-trickle peer.
        return bytes([self.data.pop(0)])

body=b'{}';packet=b'\x00'+m.vi(len(body))+body;wire=m.vi(len(packet))+packet
real_connect=m.socket.create_connection;real_time=m.time.monotonic
try:
    fast=FakeSocket(wire);m.socket.create_connection=lambda *args,**kwargs:fast
    m.time.monotonic=lambda:0.0
    assert m.mc_status()=={}
    endpoints=[];custom=FakeSocket(wire)
    m.status_endpoint=lambda:('::1',25570)
    m.socket.create_connection=lambda address,**kwargs:(endpoints.append(address) or custom)
    assert m.mc_status()=={} and endpoints==[('::1',25570)]
    assert b'::1' in custom.sent and b'\x63\xe2' in custom.sent
    m.status_endpoint=lambda:('127.0.0.1',25565)
    for malformed in (b'\x01\x00',b'\x02\x00\x80'):
        m.socket.create_connection=lambda *args,**kwargs:FakeSocket(malformed)
        try:m.mc_status();raise AssertionError('truncated status length accepted')
        except ValueError:pass
    assert m.bounded_json('{"message":"[{}]\\\""}')=={'message':'[{}]"'}
    nested=b'['*2000+b'0'+b']'*2000
    nested_packet=b'\x00'+m.vi(len(nested))+nested
    m.socket.create_connection=lambda *args,**kwargs:FakeSocket(m.vi(len(nested_packet))+nested_packet)
    try:m.mc_status();raise AssertionError('excessively nested status accepted')
    except ValueError:pass
    m.socket.create_connection=lambda *args,**kwargs:FakeSocket(wire)
    original_json=m.json.loads
    def nested_error(*args,**kwargs):raise RecursionError('nested JSON')
    try:
        m.json.loads=nested_error
        try:m.mc_status();raise AssertionError('excessively nested status accepted')
        except ValueError:pass
    finally:m.json.loads=original_json
    slow=FakeSocket(wire);m.socket.create_connection=lambda *args,**kwargs:slow
    ticks=iter([0.0,0.5,1.0,1.5,2.0])
    m.time.monotonic=lambda:next(ticks)
    try:m.mc_status();raise AssertionError('slow status response accepted')
    except socket.timeout:pass
finally:
    m.socket.create_connection=real_connect;m.time.monotonic=real_time
print('STATUS_RESPONSE_DEADLINE_PASSED')
