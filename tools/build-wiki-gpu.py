#!/usr/bin/env python3
"""Build the tested N20 GPU worker into a new local directory; never deploy it.

Requires an Android NDK, CMake, Ninja, Git, and OpenCL headers/link library.
The output is a partial bundle: device-specific driver files, firmware and the
model are deliberately not included. See docs/wiki-chat.md before use.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile

REVISION = 'cd959bae99ff4bed22bef2604f45241c09bbb8c8'  # llama.cpp b12110
ROOT = Path(__file__).resolve().parents[1]
EXPERIMENTS = ROOT / 'wiki' / 'experiments'
PATCHES = (
    'llama-b12110-n20-opencl.patch',
    'llama-b12110-n20-activation-image.patch',
    'llama-b12110-n20-fa-cache.patch',
    'llama-b12110-n20-server.patch',
    'llama-b12110-n20-disk-prefix.patch',
    'llama-b12110-n20-gdn-columns.patch',
)
KERNELS = ('n20-q2-decode.cl', 'n20-q2-pack.cl', 'n20-fwht-radix.cl',
           'n20-q2-activation-image-prefill.cl')


def run(*args, **kwargs):
    subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source', 'ndk', 'opencl-include', 'opencl-library', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--cmake', default='cmake')
    parser.add_argument('--ninja', help='absolute path to Ninja, if not on PATH')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    source, ndk, include, library = (p.resolve(strict=True) for p in
                                     (args.source, args.ndk, args.opencl_include, args.opencl_library))
    output = args.output.resolve()
    if not 1 <= args.jobs <= 64:
        parser.error('--jobs must be between 1 and 64')
    if output.exists() or output == ROOT or ROOT in output.parents or output == source or source in output.parents:
        parser.error('--output must be a new directory outside this checkout and the upstream source')
    if not (ndk / 'build/cmake/android.toolchain.cmake').is_file() or not (include / 'CL/cl.h').is_file() or not library.is_file():
        parser.error('invalid NDK, OpenCL headers, or OpenCL link library')
    commit = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if commit != REVISION:
        parser.error('upstream source must be the exact llama.cpp b12110 revision ' + REVISION)
    output.mkdir(parents=True, exist_ok=False)
    snapshot = output / 'upstream.tar'
    # Archive the commit, not the caller's working tree; no source files are modified.
    with snapshot.open('wb') as stream:
        run('git', '-C', source, 'archive', '--format=tar', REVISION, stdout=stream)
    src = output / 'src'
    src.mkdir()
    with tarfile.open(snapshot) as archive:
        archive.extractall(src, filter='data')
    snapshot.unlink()
    for patch in PATCHES:
        run('git', '-C', src, 'apply', '--check', EXPERIMENTS / patch)
        run('git', '-C', src, 'apply', EXPERIMENTS / patch)
    build = output / 'build'
    cmake_args = [args.cmake, '-S', src, '-B', build, '-G', 'Ninja',
                  '-DCMAKE_TOOLCHAIN_FILE=' + str(ndk / 'build/cmake/android.toolchain.cmake'),
                  '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-30',
                  '-DCMAKE_BUILD_TYPE=Release', '-DGGML_OPENCL=ON',
                  '-DGGML_OPENCL_USE_ADRENO_KERNELS=ON', '-DGGML_OPENCL_EMBED_KERNELS=ON',
                  '-DGGML_OPENCL_TARGET_VERSION=200', '-DGGML_NATIVE=OFF',
                  '-DGGML_HEXAGON=OFF', '-DGGML_RPC=OFF', '-DGGML_OPENMP=OFF',
                  '-DLLAMA_CURL=OFF', '-DLLAMA_OPENSSL=OFF',
                  '-DLLAMA_BUILD_TESTS=OFF', '-DLLAMA_BUILD_EXAMPLES=OFF', '-DLLAMA_BUILD_SERVER=ON',
                  '-DGGML_CPU_ARM_ARCH=armv8.2-a+fp16+dotprod',
                  '-DCMAKE_C_FLAGS=-march=armv8.2-a+fp16+dotprod',
                  '-DCMAKE_CXX_FLAGS=-march=armv8.2-a+fp16+dotprod',
                  '-DOpenCL_INCLUDE_DIR=' + str(include), '-DOpenCL_LIBRARY=' + str(library),
                  '-DPython3_EXECUTABLE=' + sys.executable]
    if args.ninja:
        cmake_args.append('-DCMAKE_MAKE_PROGRAM=' + str(Path(args.ninja).resolve(strict=True)))
    run(*cmake_args)
    run(args.cmake, '--build', build, '--target', 'llama-server', 'ggml-opencl', '-j', args.jobs)
    bundle = output / 'bundle'
    binary_dir, kernel_dir = bundle / 'bin', bundle / 'kernels'
    binary_dir.mkdir(parents=True); kernel_dir.mkdir()
    for name in ('llama-server', 'libllama-server-impl.so', 'libllama-common.so', 'libmtmd.so',
                 'libllama.so', 'libggml.so', 'libggml-base.so', 'libggml-cpu.so', 'libggml-opencl.so'):
        shutil.copy2(build / 'bin' / name, binary_dir / name)
    shutil.copy2(src / 'LICENSE', bundle / 'llama.cpp-LICENSE')
    for name in KERNELS:
        shutil.copy2(EXPERIMENTS / name, kernel_dir / name)
    (kernel_dir / 'n20-q2-prefill-64.cl').write_bytes(
        b'#define N20_PARTIAL 64\n' + (EXPERIMENTS / 'n20-q2-prefill.cl').read_bytes())
    manifest = {'upstream_revision': REVISION, 'patches_sha256': {p: digest(EXPERIMENTS / p) for p in PATCHES},
                'bundle_sha256': {str(p.relative_to(bundle)).replace(os.sep, '/'): digest(p)
                                  for p in sorted(bundle.rglob('*')) if p.is_file()},
                'note': 'Build inputs and staged files only; device runtime, firmware and model are not included.'}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    print('Built a local partial bundle at', bundle)


if __name__ == '__main__':
    main()
