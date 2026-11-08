"""Forward Minecraft traffic from the PC's LAN address to the phone over USB."""
import ipaddress
import select
import socket
import socketserver
import sys

LAN = ipaddress.ip_network(sys.argv[2], strict=False)


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        if ipaddress.ip_address(self.client_address[0]) not in LAN:
            return
        upstream = None
        for address in ("192.168.137.2", "169.254.77.2"):
            try:
                upstream = socket.create_connection((address, 25565), timeout=3)
                break
            except OSError:
                continue
        if upstream is None:
            return
        with upstream:
            self.request.settimeout(None)
            upstream.settimeout(None)
            while True:
                try:
                    ready, _, exceptional = select.select([self.request, upstream], [], [self.request, upstream], 120)
                    if exceptional:
                        return
                    for source in ready:
                        chunk = source.recv(65536)
                        if not chunk:
                            return
                        (upstream if source is self.request else self.request).sendall(chunk)
                except OSError:
                    return


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    with Server((sys.argv[1], 25566), Handler) as server:
        print(f"Minecraft bridge listening on {sys.argv[1]}:25566 for {LAN}", flush=True)
        server.serve_forever()
