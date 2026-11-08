#!/bin/sh
# Builds only the open-source radio helpers and PM stub. Does not install them.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=${1:-"$root/build/radio"}
prefix=${QRTR_PREFIX:-/usr}
multiarch=$(cc -print-multiarch)
mkdir -p "$out"
common="-std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare"
cd "$root/third_party/rmtfs-n20"
cc $common -I"$prefix/include" qmi_rmtfs.c rmtfs.c rproc.c sharedmem.c storage.c util.c \
  -L"$prefix/lib/$multiarch" -lqrtr -l:libudev.so.1 -lpthread -o "$out/rmtfs-n20"
cd "$root/third_party/pd-mapper-n20"
cc $common -I"$prefix/include" pd-mapper.c assoc.c json.c servreg_loc.c \
  -L"$prefix/lib/$multiarch" -lqrtr -o "$out/pd-mapper-n20"
cd "$root/third_party/tqftpserv-n20"
cc $common -I"$prefix/include" tqftpserv.c translate.c zstd-decompress.c \
  -L"$prefix/lib/$multiarch" -lqrtr -lzstd -o "$out/tqftpserv-n20"
cc -shared -fPIC -nostdlib -Wl,-soname,libn20-pm-stub.so \
  "$root/radio/cnss-pm-stub.c" -o "$out/libn20-pm-stub.so"
printf 'Radio helpers built in %s\n' "$out"
