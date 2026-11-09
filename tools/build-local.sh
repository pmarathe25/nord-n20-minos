#!/bin/sh
# Build on an ARM64 Debian installation; outputs are not installed.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=${1:-"$root/build"}
mkdir -p "$out"
cc -std=c11 -O2 -Wall -Wextra -Werror -I/usr/include/libdrm -I/usr/include/freetype2 \
  "$root/dashboard/n20-dashboard.c" -o "$out/n20-dashboard" -ldrm -lfreetype -lm
cc -D_GNU_SOURCE -std=c11 -O2 -Wall -Wextra -I/usr/include/libdrm \
  "$root/dashboard/capture.c" -o "$out/n20-capture-screen" -ldrm
cc -std=c11 -O2 -Wall -Wextra -Werror \
  "$root/runtime/usr/local/sbin/n20-mtp-sandbox.c" -o "$out/n20-mtp-sandbox"
printf 'Dashboard, screen capture, and MTP sandbox built in %s\n' "$out"
