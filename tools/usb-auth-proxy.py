"""Restricted HTTPS CONNECT proxy for the phone's Minecraft authentication."""
import select
import socket
import socketserver
import sys
import time

ALLOWED = ("minecraftservices.com", "minecraft.net", "mojang.com", "microsoft.com")


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        if self.client_address[0] not in ("169.254.77.2", "192.168.137.2"):
            return
        self.request.settimeout(10)
        data = bytearray()
        while b"\r\n\r\n" not in data and len(data) < 16384:
            part = self.request.recv(4096)
            if not part:
                return
            data.extend(part)
        try:
            first = bytes(data).split(b"\r\n", 1)[0].decode("ascii")
            method, target, _ = first.split(" ", 2)
            if method == "GET" and target == "/time":
                body = f"{int(time.time())}\n".encode()
                self.request.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " + str(len(body)).encode() + b"\r\nConnection: close\r\n\r\n" + body)
                return
            hostname, port_text = target.rsplit(":", 1)
            port = int(port_text)
        except (ValueError, UnicodeError):
            return
        if method != "CONNECT" or port != 443 or not any(
            hostname == domain or hostname.endswith("." + domain) for domain in ALLOWED
        ):
            self.request.sendall(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n")
            return
        try:
            with socket.create_connection((hostname, port), timeout=15) as upstream:
                self.request.sendall(b"HTTP/1.1 200 Connection Established\r\n\r\n")
                trailing = bytes(data).split(b"\r\n\r\n", 1)[1]
                if trailing:
                    upstream.sendall(trailing)
                self.request.settimeout(None)
                upstream.settimeout(None)
                while True:
                    ready, _, exceptional = select.select([self.request, upstream], [], [self.request, upstream], 120)
                    if exceptional or not ready:
                        return
                    for source in ready:
                        chunk = source.recv(65536)
                        if not chunk:
                            return
                        (upstream if source is self.request else self.request).sendall(chunk)
        except OSError as exc:
            print(f"Proxy error for {hostname}: {exc}", flush=True)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    address = sys.argv[1]
    with Server((address, 18888), Handler) as server:
        print(f"Minecraft authentication proxy listening on {address}:18888", flush=True)
        server.serve_forever()
