"""Query the Minecraft Java status endpoint without joining the world."""
import json
import socket
import struct
import sys


def varint(value):
    result = bytearray()
    while value > 127:
        result.append((value & 127) | 128)
        value >>= 7
    result.append(value)
    return bytes(result)


def recv_varint(sock):
    value = 0
    for index in range(5):
        byte = sock.recv(1)
        if not byte:
            raise EOFError("server closed connection")
        value |= (byte[0] & 127) << (7 * index)
        if not byte[0] & 128:
            return value
    raise ValueError("invalid varint")


host = sys.argv[1]
port = int(sys.argv[2])
encoded = host.encode()
handshake = varint(0) + varint(777) + varint(len(encoded)) + encoded + struct.pack(">H", port) + varint(1)
with socket.create_connection((host, port), timeout=10) as conn:
    conn.sendall(varint(len(handshake)) + handshake + b"\x01\x00")
    size = recv_varint(conn)
    packet = bytearray()
    while len(packet) < size:
        data = conn.recv(size - len(packet))
        if not data:
            raise EOFError("incomplete server reply")
        packet.extend(data)
    if packet[0] != 0:
        raise ValueError("unexpected response packet")
    index = 1
    length = 0
    shift = 0
    while True:
        value = packet[index]
        index += 1
        length |= (value & 127) << shift
        if not value & 128:
            break
        shift += 7
    result = json.loads(bytes(packet[index:index + length]))
    print(json.dumps(result, indent=2))
