#!/usr/bin/env python3
"""Test signed replies on loopback; never contact a public server or set time."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'third_party/pyroughtime'))
from pyroughtime import RoughtimeClient, RoughtimeError, RoughtimePacket, RoughtimeServer

private, public = RoughtimeServer.create_key()
certificate, delegate = RoughtimeServer.create_delegated_key(private)
server = RoughtimeServer(public, certificate, delegate)
server.start('127.0.0.1', 0)
try:
    port = server._sock.getsockname()[1]
    client = RoughtimeClient()
    original_query = client._udp_query
    captured = []

    def record(*args):
        response = original_query(*args)
        captured.append(response)
        return response

    client._udp_query = record
    result = client.query('127.0.0.1', port, public)
    assert result.midp() > 1704067200

    client._udp_query = lambda *args: captured[0]
    try:
        client.query('127.0.0.1', port, public)
    except RoughtimeError as exc:
        assert 'NONC' in str(exc)
    else:
        raise AssertionError('Replayed response accepted')

    def tamper(*args):
        reply, sent, received, data = original_query(*args)
        signature = reply.get_tag('SIG').get_value_bytes()
        assert len(signature) == 64 and data.count(signature) == 1
        data = data.replace(signature, bytes([signature[0] ^ 1]) + signature[1:], 1)
        return RoughtimePacket(packet=data, expect_header=True), sent, received, data

    client._udp_query = tamper
    try:
        client.query('127.0.0.1', port, public)
    except RoughtimeError as exc:
        assert 'signature' in str(exc).lower()
    else:
        raise AssertionError('Modified signature accepted')

    def tamper_delegation(*args):
        reply, sent, received, data = original_query(*args)
        signature = reply.get_tag('CERT').get_tag('SIG').get_value_bytes()
        assert len(signature) == 64 and data.count(signature) == 1
        data = data.replace(signature, bytes([signature[0] ^ 1]) + signature[1:], 1)
        return RoughtimePacket(packet=data, expect_header=True), sent, received, data

    client._udp_query = tamper_delegation
    try:
        client.query('127.0.0.1', port, public)
    except RoughtimeError as exc:
        assert 'long term certificate' in str(exc).lower()
    else:
        raise AssertionError('Modified delegation accepted')
finally:
    server.stop()

print('ROUGHTIME_REPLAY_AND_TAMPER_REJECTED')
