#!/usr/bin/env python3
"""Validate optional Wiki worker inputs and prepare the private prefix cache."""

import hashlib
import json
import mmap
import os
from pathlib import Path
import stat
import sys


FIRMWARE = ("a630_sqe.fw", "a615_zap.mdt", "a615_zap.b00", "a615_zap.b01", "a615_zap.b02")
FIRMWARE_PATH = Path("/sys/module/firmware_class/parameters/path")
KERNEL_INPUTS = (Path("/sys/kernel/notes"), Path("/proc/sys/kernel/random/boot_id"))
CAPABILITIES = (b"N20_PREFIX_DIR\0", b"N20_PREFIX_KEY\0")
TUNED_SOURCES = {
    "N20_Q2_PREFILL_SOURCE": "n20-q2-prefill-64.cl",
    "N20_Q2_DECODE_SOURCE": "n20-q2-decode.cl",
    "N20_Q2_PACK_SOURCE": "n20-q2-pack.cl",
    "N20_FWHT_RADIX_SOURCE": "n20-fwht-radix.cl",
}
TUNED_FLAGS = ("N20_Q2_TAIL", "N20_Q2_WIDE_VOCAB", "N20_SINGLE_ROW_COPY")
TEXTURE_SOURCES = {"N20_Q2_TEXTURE_SOURCE": "n20-q2-activation-image-prefill.cl"}
GDN_FLAGS = ("N20_GDN_CPL",)
TEXTURE_MODEL_SHA256 = "64a5183b44921bdb5ac4a6f5f92007821d6ebe5c66997d3c147fd214ab9433d4"


def trusted(path, is_dir=False):
    if not path.is_absolute() or ".." in path.parts:
        raise ValueError(f"expected an absolute, normalized path: {path}")
    for parent in reversed(path.parents):
        check_owner(parent, stat.S_ISDIR)
    check_owner(path, stat.S_ISDIR if is_dir else stat.S_ISREG)


def check_owner(path, expected_type):
    info = path.lstat()
    if info.st_uid != 0 or info.st_mode & 0o022 or not expected_type(info.st_mode):
        raise ValueError(f"unsafe Wiki runtime input: {path}")


def runtime_files(directory):
    trusted(directory, is_dir=True)
    for entry in sorted(directory.iterdir()):
        info = entry.lstat()
        if stat.S_ISDIR(info.st_mode):
            yield from runtime_files(entry)
        elif stat.S_ISLNK(info.st_mode):
            target = Path(os.readlink(entry))
            if info.st_uid != 0 or len(target.parts) != 1 or target.name in ("", ".."):
                raise ValueError(f"unsafe Wiki runtime link: {entry}")
            target = entry.with_name(target.name)
            trusted(target)
            yield entry, target
        else:
            trusted(entry)
            yield entry, entry


def library_paths(bundle, android):
    backend = bundle / "opencl"
    return backend / "vendor", android, backend / "system", backend / "bin"


def check_profile(bundle, android, marker, sources, flags):
    trusted(marker)
    *earlier, bin_dir = library_paths(bundle, android)
    name = "libggml-opencl.so"
    for directory in (*earlier, bundle / "opencl"):
        trusted(directory, is_dir=True)
        if os.path.lexists(directory / name):
            raise ValueError(f"shadowed Wiki OpenCL backend: {directory / name}")
    backend = bin_dir / name
    trusted(backend)
    with backend.open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as lib:
        if any(lib.find(option.encode() + b"\0") < 0 for option in (*sources, *flags)):
            raise ValueError("Wiki OpenCL backend lacks the tuning patch for this profile")
    for name in sources.values():
        source = bundle / "kernels" / name
        trusted(source)
        if not 0 < source.stat().st_size <= 64 * 1024:
            raise ValueError(f"invalid Wiki OpenCL source size: {source}")


def check_tuned(bundle, android, marker):
    check_profile(bundle, android, marker, TUNED_SOURCES, TUNED_FLAGS)


def check_gdn(bundle, android, marker, shader_dir, uid):
    check_profile(bundle, android, marker, {}, GDN_FLAGS)
    prepare(shader_dir, uid)


def check_texture(bundle, android, marker, shader_dir, uid):
    check_profile(bundle, android, marker, TEXTURE_SOURCES, ())
    model = bundle / "model.gguf"
    trusted(model)
    with model.open("rb") as stream:
        before = file_identity(os.fstat(stream.fileno()))
        model_digest = hashlib.file_digest(stream, "sha256").digest()
        if file_identity(os.fstat(stream.fileno())) != before:
            raise ValueError("Wiki model changed during texture validation")
        if model_digest.hex() != TEXTURE_MODEL_SHA256:
            raise ValueError("activation texture requires the tested 4B Q2_0 checkpoint")
    prepare(shader_dir, uid)
    return model_digest, before


def file_identity(info):
    return info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns


def fingerprint(bundle, android, launch, firmware_dir=Path("/usr/lib/firmware"),
                kernel_inputs=KERNEL_INPUTS, verified_model=None):
    backend = bundle / "opencl"
    libraries = library_paths(bundle, android)
    executable = backend / "bin/llama-server"
    model = bundle / "model.gguf"
    linker = android / "linker64"
    shim = backend / "cl-compat.so"
    for path in (executable, model, linker, shim):
        trusted(path)
    for directory in libraries:
        trusted(directory, is_dir=True)

    # ggml also searches the working directory for backends.
    files = dict(runtime_files(backend)) | dict(runtime_files(android))
    implementations = [files[dir_path / "libllama-server-impl.so"] for dir_path in libraries
                       if dir_path / "libllama-server-impl.so" in files]
    if len(implementations) != 1 or implementations[0].parent != executable.parent:
        raise ValueError("expected one Wiki server implementation beside llama-server")
    with implementations[0].open("rb") as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as impl:
        if any(impl.find(capability) < 0 for capability in CAPABILITIES):
            raise ValueError("Wiki server lacks the disk prefix patch")

    for path in (model, shim, *(firmware_dir / name for name in FIRMWARE), *kernel_inputs):
        trusted(path)
        files[path] = path
    sources = TUNED_SOURCES | TEXTURE_SOURCES
    for argument in launch:
        option, _, value = argument.partition("=")
        if option in sources:
            path = bundle / "kernels" / sources[option]
            if Path(value) != path:
                raise ValueError(f"unexpected Wiki OpenCL source for {option}")
            trusted(path)
            files[path] = path

    # Full file content and the exact sanitized launch environment/options are
    # bound to the key. The boot ID also scopes the snapshot to the running KGSL
    # firmware, whose in-memory contents cannot be read through sysfs.
    digest = hashlib.sha256(json.dumps(["n20-prefix-v1", launch]).encode())
    for path, target in sorted(files.items()):
        digest.update(os.fsencode(path) + b"\0" + os.fsencode(target) + b"\0")
        with target.open("rb") as stream:
            if path == model and verified_model is not None:
                model_digest, identity = verified_model
                if len(model_digest) != 32 or file_identity(os.fstat(stream.fileno())) != identity:
                    raise ValueError("Wiki model changed after texture validation")
                digest.update(model_digest)
            else:
                digest.update(hashlib.file_digest(stream, "sha256").digest())
    return digest.hexdigest()


def prepare(directory, uid):
    trusted(directory.parent, is_dir=True)
    try:
        directory.mkdir(mode=0o700)
    except FileExistsError:
        pass
    else:
        os.chown(directory, uid, uid)
    info = directory.lstat()
    if (not stat.S_ISDIR(info.st_mode) or info.st_uid != uid or info.st_gid != uid or
            stat.S_IMODE(info.st_mode) != 0o700):
        raise ValueError(f"unsafe Wiki prefix cache directory: {directory}")


def prepare_prefix(bundle, android, directory, uid, launch, verified_model=None):
    trusted(FIRMWARE_PATH)
    if FIRMWARE_PATH.read_text().strip() != "/newroot/usr/lib/firmware":
        raise ValueError("unexpected Nord N20 firmware search path")
    key = fingerprint(bundle, android, launch, verified_model=verified_model)
    prepare(directory, uid)
    return key


def main(args):
    bundle, android, directory = map(Path, args[:3])
    if args[4] != "--" or not args[5:]:
        raise ValueError("expected worker environment and command after --")
    print(prepare_prefix(bundle, android, directory, int(args[3]), args[5:]))


def prepare_texture(args):
    bundle, android, marker, shader_dir = map(Path, args[:4])
    uid = int(args[4])
    verified_model = check_texture(bundle, android, marker, shader_dir, uid)
    if len(args) == 5:
        return
    try:
        if args[6] != "--" or not args[7:]:
            raise ValueError("expected worker environment and command after --")
        print(prepare_prefix(bundle, android, Path(args[5]), uid, args[7:], verified_model))
    except (OSError, ValueError, IndexError) as exc:
        # Keep the texture profile if only disk-prefix preparation failed.
        # Revalidate in case the checkpoint changed after its first hash.
        check_texture(bundle, android, marker, shader_dir, uid)
        print(f"n20-wiki: disk prefix disabled: {exc}", file=sys.stderr)


if __name__ == "__main__":
    feature = "disk prefix"
    try:
        if sys.argv[1:2] == ["--check-tuned"]:
            feature = "tuned OpenCL"
            check_tuned(*map(Path, sys.argv[2:]))
        elif sys.argv[1:2] == ["--check-texture"]:
            feature = "OpenCL activation texture"
            check_texture(*map(Path, sys.argv[2:6]), int(sys.argv[6]))
        elif sys.argv[1:2] == ["--check-gdn"]:
            feature = "OpenCL recurrent prefill"
            check_gdn(*map(Path, sys.argv[2:6]), int(sys.argv[6]))
        elif sys.argv[1:2] == ["--prepare-texture"]:
            feature = "OpenCL activation texture"
            prepare_texture(sys.argv[2:])
        else:
            main(sys.argv[1:])
    except (OSError, ValueError, IndexError) as exc:
        sys.exit(f"n20-wiki: {feature} disabled: {exc}")
