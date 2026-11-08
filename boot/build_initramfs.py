"""Create a deterministic research initramfs; this does not touch the phone."""
import gzip
import argparse
import hashlib
import io
import pathlib
import stat
import struct
import sys
import tarfile

HERE = pathlib.Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--busybox-apk", required=True, type=pathlib.Path)
parser.add_argument("--vendor-ramdisk", required=True, type=pathlib.Path)
parser.add_argument("--output", required=True, type=pathlib.Path)
args = parser.parse_args()
APK = args.busybox_apk
APK_SHA256 = "ee469aee2958feffd7f64dd96655704025ff60af614f77a1d7323dc237c34da2"
data = APK.read_bytes()
if hashlib.sha256(data).hexdigest() != APK_SHA256:
    raise SystemExit("BusyBox package checksum mismatch")
with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tar:
    busybox = tar.extractfile("bin/busybox.static").read()
if busybox[:4] != b"\x7fELF" or busybox[4:6] != b"\x02\x01":
    raise SystemExit("BusyBox is not little-endian ELF64")
if struct.unpack_from("<H", busybox, 18)[0] != 183:
    raise SystemExit("BusyBox is not AArch64")
phoff = struct.unpack_from("<Q", busybox, 32)[0]
phentsize, phnum = struct.unpack_from("<HH", busybox, 54)
if any(struct.unpack_from("<I", busybox, phoff + i * phentsize)[0] == 3 for i in range(phnum)):
    raise SystemExit("BusyBox unexpectedly requires a dynamic loader")

vendor = args.vendor_ramdisk
vendor_data = vendor.read_bytes()
if hashlib.sha256(vendor_data).hexdigest() != "8dd5faeefde663f7037540c07c3bce6a6e0452afdb3b995a08c2bc5b98c3eea9":
    raise SystemExit("C.24 vendor ramdisk checksum mismatch")
raw = gzip.decompress(vendor_data)
members = {}
pos = 0
while pos + 110 <= len(raw):
    header = raw[pos:pos + 110]
    if header[:6] != b"070701":
        raise SystemExit("Unexpected vendor ramdisk CPIO format")
    fields = [int(header[6 + i * 8:14 + i * 8], 16) for i in range(13)]
    pos += 110
    name = raw[pos:pos + fields[11] - 1].decode()
    pos = (pos + fields[11] + 3) & ~3
    content = raw[pos:pos + fields[6]]
    pos = (pos + fields[6] + 3) & ~3
    if name == "TRAILER!!!":
        break
    if name.startswith("lib/modules/5.4-gki/") and stat.S_ISREG(fields[1]):
        members[name.rsplit("/", 1)[-1]] = content
else:
    raise SystemExit("Vendor ramdisk has no CPIO trailer")

dependencies = {}
for line in members["modules.dep"].decode().splitlines():
    left, right = line.split(":", 1)
    dependencies[left.rsplit("/", 1)[-1]] = [x.rsplit("/", 1)[-1] for x in right.split()]
order = []
visiting = set()
done = set()
def visit(name):
    if name in done:
        return
    if name in visiting or name not in members:
        raise SystemExit(f"Invalid or cyclic module dependency: {name}")
    visiting.add(name)
    for dependency in dependencies.get(name, []):
        visit(dependency)
    visiting.remove(name)
    done.add(name)
    order.append(name)
for name in members["modules.load"].decode().splitlines():
    if name:
        visit(name)

init_path = HERE / "persistent-init"
items = [
    ("bin", stat.S_IFDIR | 0o755, b"", 0, 0),
    ("dev", stat.S_IFDIR | 0o755, b"", 0, 0),
    ("proc", stat.S_IFDIR | 0o755, b"", 0, 0),
    ("sys", stat.S_IFDIR | 0o755, b"", 0, 0),
    ("early-modules", stat.S_IFDIR | 0o755, b"", 0, 0),
    ("early-modules.load", stat.S_IFREG | 0o644, ("\n".join(order) + "\n").encode(), 0, 0),
    ("init", stat.S_IFREG | 0o755, init_path.read_bytes(), 0, 0),
    ("bin/busybox", stat.S_IFREG | 0o755, busybox, 0, 0),
    ("dev/console", stat.S_IFCHR | 0o600, b"", 5, 1),
    ("dev/null", stat.S_IFCHR | 0o666, b"", 1, 3),
    ("dev/kmsg", stat.S_IFCHR | 0o600, b"", 1, 11),
]
for name in order:
    items.append(("early-modules/" + name, stat.S_IFREG | 0o644, members[name], 0, 0))
for applet in ("sh", "mount", "mkdir", "echo", "ln", "ls", "head", "sleep", "cat", "mknod", "setsid", "insmod", "reboot"):
    if applet.encode() + b"\0" not in busybox:
        raise SystemExit(f"BusyBox applet name missing: {applet}")
    items.append(("bin/" + applet, stat.S_IFLNK | 0o777, b"busybox", 0, 0))
items.append(("TRAILER!!!", 0, b"", 0, 0))

archive = bytearray()
for ino, (name, mode, content, major, minor) in enumerate(items, 1):
    name = name.encode() + b"\0"
    fields = (ino, mode, 0, 0, 1, 0, len(content), 0, 0, major, minor, len(name), 0)
    archive.extend(b"070701" + b"".join(f"{x:08x}".encode() for x in fields))
    archive.extend(name)
    archive.extend(b"\0" * (-len(archive) % 4))
    archive.extend(content)
    archive.extend(b"\0" * (-len(archive) % 4))
output = args.output
output.parent.mkdir(parents=True, exist_ok=True)
output.write_bytes(gzip.compress(bytes(archive), compresslevel=9, mtime=0))
print(f"{output}: {output.stat().st_size} bytes sha256={hashlib.sha256(output.read_bytes()).hexdigest()}")
