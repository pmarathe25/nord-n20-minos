#!/usr/bin/env python3
"""Scratch-only hybrid prefix-state trial with caller-supplied cache identity.

Run under the phone's normal accelerator lock and resource guard. This program
does not deploy a worker or change installed files. The supplied cache must be
a private directory; its contents contain model state and are not for Git.
This helper is not a production trust boundary: the caller must supply the
actual firmware and driver artifacts and keep all inputs protected throughout.
"""
import argparse
try:
    import fcntl
except ImportError:  # Static/unit checks on non-POSIX hosts only.
    fcntl = None
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import tempfile
import time

ROLES = frozenset(("backend", "hybrid-runtime", "dsp-module", "dsp-shell", "dsp-firmware",
                   "gpu-firmware", "gpu-driver", "row-shader", "aux-shader",
                   "device-identity"))
STATE_NAME = "prefix.state"
MANIFEST_NAME = "prefix.manifest.json"
MODULE_NAMES = ("libn20_projection_skel.so", "libn20_projection_block_major_skel.so")


def digest_json(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def identity(st):
    return (st.st_dev, st.st_ino, st.st_size, st.st_mtime_ns, st.st_ctime_ns)


def digest_file(path, private=False):
    path = Path(path)
    if not path.is_absolute() or path.is_symlink() or path.resolve(strict=True) != path:
        raise ValueError("input must be an absolute regular non-symlink file")
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0)
    fd = os.open(path, flags)
    try:
        before = os.fstat(fd)
        if not stat.S_ISREG(before.st_mode):
            raise ValueError("input is not a regular file")
        if private and (before.st_uid != os.geteuid() or stat.S_IMODE(before.st_mode) != 0o600 or before.st_nlink != 1):
            raise ValueError("unsafe private cache file")
        h = hashlib.sha256()
        while True:
            block = os.read(fd, 1024 * 1024)
            if not block:
                break
            h.update(block)
        if identity(os.fstat(fd)) != identity(before):
            raise ValueError("input changed while hashing")
        return h.hexdigest()
    finally:
        os.close(fd)


def private_dir(path):
    path = Path(path)
    if not path.is_absolute() or path.resolve() != path:
        raise ValueError("cache directory must be absolute and have no symlink components")
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != 0o700:
        raise ValueError("cache directory must be owned by the caller and mode 0700")
    return path


def artifacts(values):
    result = {}
    for value in values:
        role, sep, name = value.partition("=")
        if not sep or role not in ROLES or role in result or not name:
            raise ValueError("artifacts must use unique known ROLE=/absolute/path entries")
        result[role] = Path(name)
    if set(result) != ROLES:
        raise ValueError("all required artifact roles must be provided")
    return result


def tree_identity(root):
    root = Path(root)
    if not root.is_absolute() or root.resolve() != root or not root.is_dir():
        raise ValueError("runtime tree must be an absolute non-symlink directory")
    entries = []
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            target = path.resolve(strict=True)
            if not target.is_file() or root not in target.parents:
                raise ValueError("runtime tree symlink escapes or is not a file")
            entries.append([str(path.relative_to(root)), "link", os.readlink(path), digest_file(target)])
        elif path.is_file():
            entries.append([str(path.relative_to(root)), "file", digest_file(path)])
        elif not path.is_dir():
            raise ValueError("unexpected runtime tree entry")
    return entries


def runtime_module(path):
    """Identify the statically linked client URI in a known scratch runtime.

    This checks the experimental runtimes' convention; it cannot prove what an
    arbitrary shared object loads. Inputs must remain trusted and immutable.
    """
    path = Path(path)
    if not path.is_absolute() or path.resolve(strict=True) != path:
        raise ValueError("unsafe hybrid runtime path")
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0)
    fd = os.open(path, flags)
    try:
        before = os.fstat(fd)
        if not stat.S_ISREG(before.st_mode) or before.st_size > 32 * 1024 * 1024:
            raise ValueError("unsafe hybrid runtime")
        with os.fdopen(os.dup(fd), "rb") as stream:
            content = stream.read()
        if identity(os.fstat(fd)) != identity(before):
            raise ValueError("hybrid runtime changed while reading")
    finally:
        os.close(fd)
    names = [name for name in MODULE_NAMES if ("file:///" + name + "?").encode() in content]
    if len(names) != 1:
        raise ValueError("hybrid runtime module URI missing or ambiguous")
    return names[0]


def profile(args, env):
    required = artifacts(args.artifact)
    roots = [Path(root) for root in args.runtime_tree]
    if len(set(roots)) != len(roots):
        raise ValueError("runtime trees must be unique")
    tree_hashes = {str(root): tree_identity(root) for root in sorted(roots)}
    if env.get("GGML_BACKEND_PATH"):
        raise ValueError("out-of-tree GGML_BACKEND_PATH is unsupported in this trial")
    for directory in env.get("LD_LIBRARY_PATH", "").split(os.pathsep):
        path = Path(directory)
        if (not directory or not path.is_absolute() or not path.is_dir() or path.resolve() != path or
                not any(path == root or root in path.parents for root in roots)):
            raise ValueError("every LD_LIBRARY_PATH directory must be covered by a runtime tree")
    libraries = [Path(directory) for directory in env["LD_LIBRARY_PATH"].split(os.pathsep)]
    backend = next((directory / "libggml-opencl.so" for directory in libraries
                    if (directory / "libggml-opencl.so").is_file()), None)
    if backend != required["backend"]:
        raise ValueError("OpenCL backend does not match first library-path entry")
    if not any(args.binary.parent == root or root in args.binary.parents for root in roots):
        raise ValueError("benchmark executable must be included in a runtime tree")
    preloads = [Path(value) for value in env.get("LD_PRELOAD", "").replace(":", " ").split()]
    for key, role in (("N20_HYBRID_RUNTIME", "hybrid-runtime"),
                      ("N20_HYBRID_ROW_SOURCE", "row-shader"),
                      ("N20_HYBRID_AUX_SOURCE", "aux-shader")):
        if env.get(key) != str(required[role]):
            raise ValueError(f"{key} must select the matching fingerprinted artifact")
    module_name = runtime_module(required["hybrid-runtime"])
    if required["dsp-module"].name != module_name:
        raise ValueError("DSP module does not match hybrid runtime URI")
    for key in ("ADSP_LIBRARY_PATH", "DSP_LIBRARY_PATH"):
        if key == "DSP_LIBRARY_PATH" and not env.get(key):
            continue
        search = env.get(key, "").split(";")
        for directory in search:
            path = Path(directory)
            if (not directory or not path.is_absolute() or not path.is_dir() or path.resolve() != path or
                    not any(path == root or root in path.parents for root in roots)):
                raise ValueError(f"every {key} directory must be covered by a runtime tree")
        selected = next((Path(directory) / module_name for directory in search
                         if directory and (Path(directory) / module_name).is_file()), None)
        if selected != required["dsp-module"]:
            raise ValueError(f"DSP module does not match first selected {key} entry")
    shell = required["dsp-shell"]
    if (shell.name != "fastrpc_shell_unsigned_3" or
            not any(root in shell.parents for root in roots)):
        raise ValueError("DSP shell must be included in a runtime tree")
    sources = {key: {"path": value, "sha256": digest_file(value)}
               for key, value in sorted(env.items()) if key.startswith("N20_") and key.endswith("_SOURCE")}
    # Hash all launch environment values without logging them: a profile is
    # invalidated by changes even when an option is not known to this script.
    return {"version": 1, "model": digest_file(args.model), "binary": digest_file(args.binary),
            "runner": digest_file(args.runner), "reference": digest_file(args.reference),
            "ubatch": args.ubatch, "context_tokens": 1536,
            "environment_sha256": digest_json(sorted(env.items())),
            "artifacts": {k: {"path": str(v), "sha256": digest_file(v)} for k, v in sorted(required.items())},
            "sources": sources,
            "preloads": [{"path": str(path), "sha256": digest_file(path)} for path in preloads],
            "runtime_trees": tree_hashes}


def read_manifest(path):
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0)
    fd = os.open(path, flags)
    try:
        before = os.fstat(fd)
        if (not stat.S_ISREG(before.st_mode) or before.st_uid != os.geteuid() or
                stat.S_IMODE(before.st_mode) != 0o600 or before.st_nlink != 1 or before.st_size > 1024 * 1024):
            raise ValueError("unsafe private manifest")
        with os.fdopen(os.dup(fd), "r", encoding="utf-8") as stream:
            value = json.load(stream)
        if identity(os.fstat(fd)) != identity(before):
            raise ValueError("manifest changed while reading")
    finally:
        os.close(fd)
    if not isinstance(value, dict) or set(value) != {"version", "profile", "state_sha256"} or value["version"] != 1:
        raise ValueError("invalid cache manifest")
    return value


def validate_cache(directory, expected):
    state, manifest = directory / STATE_NAME, directory / MANIFEST_NAME
    if not os.path.lexists(state) and not os.path.lexists(manifest):
        return False
    if not os.path.lexists(state) or not os.path.lexists(manifest):
        raise ValueError("incomplete prefix cache")
    stored = read_manifest(manifest)
    if stored["profile"] != expected:
        raise ValueError("stale prefix cache profile")
    if digest_file(state, private=True) != stored["state_sha256"]:
        raise ValueError("corrupt prefix cache state")
    return True


def command(args, operation=None, state=None):
    cmd = [str(args.runner), str(args.binary), str(args.model), str(args.prompt), str(args.ubatch), "0", "1", "host"]
    if operation:
        cmd += [str(args.reference), operation, str(state)]
    return cmd


def run(args, env, operation=None, state=None):
    started = time.monotonic()
    work = private_dir(args.work_dir)
    if any(work.iterdir()):
        raise ValueError("trial working directory is not empty")
    # Driver/model logs can contain request information; never emit or retain
    # them. Parse only aggregate dispatch counters from a temporary file.
    with tempfile.TemporaryFile(mode="w+t", encoding="utf-8") as stderr:
        result = subprocess.run(command(args, operation, state), env=env, cwd=work, stdout=subprocess.PIPE,
                                stderr=stderr, text=True, timeout=args.timeout, check=False)
        stderr.seek(0)
        log = stderr.read()
    if any(work.iterdir()):
        raise ValueError("trial working directory changed during run")
    if result.returncode:
        raise RuntimeError(f"model benchmark {operation or 'fresh'} failed with exit {result.returncode}")
    matches = re.findall(r"n20 hybrid: slots_attempted=(\d+) projections_attempted=(\d+) dispatched=(\d+) fallbacks=(\d+)", log)
    if len(matches) != 1:
        raise ValueError("missing or ambiguous hybrid dispatch counters")
    slots, attempts, dispatched, fallbacks = map(int, matches[0])
    if not slots or not attempts or not dispatched or fallbacks or attempts != dispatched:
        raise ValueError("hybrid dispatch was absent or fell back")
    lines = result.stdout.splitlines()
    load = next((line for line in lines if line.startswith("load_and_context_s=")), "")
    run_line = next((line for line in lines if line.startswith("run=1 ")), "")
    prefix = next((line for line in lines if line.startswith("prefix_tokens=")), "")
    def number(pattern, line):
        match = re.search(pattern, line)
        if not match:
            raise ValueError("benchmark output incomplete")
        return match.group(1)
    output = {"elapsed_s": time.monotonic() - started,
              "load_s": float(number(r"load_and_context_s=([0-9.]+)", load)),
              "hybrid_dispatched": dispatched}
    pairs = re.findall(r"n20 hybrid pair: attempted=(\d+) reused=(\d+)", log)
    if env.get("N20_HYBRID_PAIR"):
        if len(pairs) != 1:
            raise ValueError("missing or ambiguous hybrid pair counters")
        pair_attempts, pair_reused = map(int, pairs[0])
        if pair_reused > pair_attempts:
            raise ValueError("invalid hybrid pair counters")
        output.update(pair_attempts=pair_attempts, pair_reused=pair_reused)
    if operation == "save":
        output.update(prefix_tokens=int(number(r"prefix_tokens=(\d+)", prefix)),
                      prefix_build_s=float(number(r"build_s=([0-9.]+)", prefix)))
    else:
        output.update(first_token_s=float(number(r"first_token_s=([0-9.]+)", run_line)),
                      token_ids=number(r"token_ids=([0-9,]+)", run_line),
                      logits_hash=number(r"logits_hash=([0-9a-f]+)", run_line))
        if operation == "load":
            output.update(restore_s=float(number(r"restore_s=([0-9.]+)", run_line)),
                          prefix_tokens=int(number(r"prefix_tokens=(\d+)", run_line)))
    return output


def seed(args, env, directory, expected):
    state = directory / STATE_NAME
    fd, name = tempfile.mkstemp(prefix=".prefix-state-", dir=directory)
    os.close(fd)
    temporary = Path(name)
    try:
        result = run(args, env, "save", temporary)
        if profile(args, env) != expected:
            raise ValueError("runtime profile changed while seeding")
        state_hash = digest_file(temporary, private=True)
        manifest = {"version": 1, "profile": expected, "state_sha256": state_hash}
        fd, name = tempfile.mkstemp(prefix=".prefix-manifest-", dir=directory)
        temporary_manifest = Path(name)
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as stream:
                json.dump(manifest, stream, sort_keys=True, separators=(",", ":"))
                stream.flush(); os.fsync(stream.fileno())
            if profile(args, env) != expected:
                raise ValueError("runtime profile changed before cache publication")
            # The private trial directory is exclusively owned by this trial;
            # a partial pair is rejected on the next invocation.
            os.replace(temporary, state)
            os.replace(temporary_manifest, directory / MANIFEST_NAME)
        finally:
            temporary_manifest.unlink(missing_ok=True)
        return result
    finally:
        temporary.unlink(missing_ok=True)


def execute_trial(args, env, directory):
    expected = profile(args, env)
    existed = validate_cache(directory, expected)
    before = run(args, env)
    if profile(args, env) != expected:
        raise ValueError("runtime profile changed during fresh run")
    seed_result = None if existed else seed(args, env, directory, expected)
    if profile(args, env) != expected:
        raise ValueError("runtime profile changed before restore")
    if not validate_cache(directory, expected):
        raise RuntimeError("cache unexpectedly disappeared")
    restored = run(args, env, "load", directory / STATE_NAME)
    if profile(args, env) != expected:
        raise ValueError("runtime profile changed during restore")
    after = run(args, env)
    if profile(args, env) != expected:
        raise ValueError("runtime profile changed during final run")
    parity = all((entry["token_ids"], entry["logits_hash"]) ==
                 (before["token_ids"], before["logits_hash"]) for entry in (restored, after))
    if not parity:
        raise SystemExit("first-token/logit parity failed")
    return {"profile_sha256": digest_json(expected), "cache_preexisting": existed,
            "seed": seed_result, "fresh_before": before, "restored": restored,
            "fresh_after": after, "logits_and_token_match": parity}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("model", "binary", "runner", "prompt", "reference", "cache-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--artifact", action="append", default=[], help="required ROLE=/absolute/file")
    parser.add_argument("--runtime-tree", action="append", type=Path, required=True)
    parser.add_argument("--worker-env", action="append", default=[], help="worker-only KEY=VALUE override")
    parser.add_argument("--ubatch", type=int, default=256, choices=range(1, 257))
    parser.add_argument("--timeout", type=int, default=240)
    args = parser.parse_args()
    if not 1 <= args.timeout <= 1800:
        parser.error("timeout must be between 1 and 1800 seconds")
    if fcntl is None or not hasattr(os, "geteuid"):
        raise RuntimeError("the guarded prefix trial requires POSIX file locking")
    directory = private_dir(args.cache_dir)
    args.work_dir = directory / ".work"
    args.work_dir.mkdir(mode=0o700, exist_ok=True)
    private_dir(args.work_dir)
    lock_path = directory / ".trial.lock"
    lock_fd = os.open(lock_path, os.O_RDWR | os.O_CREAT | getattr(os, "O_NOFOLLOW", 0), 0o600)
    lock_info = os.fstat(lock_fd)
    if (not stat.S_ISREG(lock_info.st_mode) or lock_info.st_uid != os.geteuid() or
            stat.S_IMODE(lock_info.st_mode) != 0o600 or lock_info.st_nlink != 1):
        raise ValueError("unsafe trial lock")
    fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    # Include both exact launch arguments and all inherited environment values
    # in the identity. Nothing derived from those values is printed except hashes.
    env = dict(os.environ)
    seen = set()
    for assignment in args.worker_env:
        key, sep, value = assignment.partition("=")
        if not sep or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", key) or key in seen:
            raise ValueError("worker environment overrides must have unique valid names")
        seen.add(key)
        env[key] = value
    print(json.dumps(execute_trial(args, env, directory), sort_keys=True))


if __name__ == "__main__":
    main()
