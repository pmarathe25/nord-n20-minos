#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d /tmp/n20-security-sysctls.XXXXXXXX)
trap 'rm -rf -- "$tmp"' EXIT HUP INT TERM
for key in protected_hardlinks protected_symlinks protected_fifos protected_regular; do printf '0\n' > "$tmp/$key"; done
sh "$root/runtime/usr/local/sbin/n20-security-sysctls" --test-dir "$tmp"
for key in protected_hardlinks protected_symlinks protected_fifos protected_regular; do [ "$(cat "$tmp/$key")" = 1 ]; done
rm "$tmp/protected_regular"
if sh "$root/runtime/usr/local/sbin/n20-security-sysctls" --test-dir "$tmp" >/dev/null 2>&1; then
    echo 'Missing protection was accepted' >&2; exit 1
fi
fake=$tmp/root
mkdir -p "$fake/proc/sys/fs" "$fake/usr/lib/python3/dist-packages" "$fake/usr/local/sbin" "$fake/usr/local/lib/n20-radio/android" "$fake/usr/local/share/ca-certificates" "$fake/opt/java/lib/server" "$fake/opt/java/bin" "$fake/opt/n20-minecraft/bundle/repo"
ln -s bundle "$fake/opt/n20-minecraft/current"
printf 'dummy\n' > "$fake/opt/n20-minecraft/bundle/server.jar"
for key in protected_hardlinks protected_symlinks protected_fifos protected_regular; do printf '0\n' > "$fake/proc/sys/fs/$key"; done
chmod 777 "$fake" "$fake/usr" "$fake/usr/lib" "$fake/usr/lib/python3/dist-packages" "$fake/usr/local" "$fake/usr/local/sbin" "$fake/usr/local/share/ca-certificates"
printf 'dummy\n' > "$fake/usr/local/lib/n20-radio/android/library.so"
chmod 666 "$fake/usr/local/lib/n20-radio/android/library.so"
printf 'dummy\n' > "$fake/usr/local/sbin/helper"
chmod 777 "$fake/usr/local/sbin/helper"
printf 'dummy\n' > "$fake/opt/java/lib/server/libjvm.so"
chmod 666 "$fake/opt/java/lib/server/libjvm.so"
sh "$root/runtime/usr/local/sbin/n20-security-sysctls" --test-root "$fake"
for path in "$fake" "$fake/usr" "$fake/usr/lib" "$fake/usr/lib/python3/dist-packages" "$fake/usr/local" "$fake/usr/local/sbin" "$fake/usr/local/share/ca-certificates"; do [ "$(stat -c %a "$path")" = 755 ]; done
[ "$(stat -c %a "$fake/usr/local/lib/n20-radio/android/library.so")" = 644 ]
[ "$(stat -c %a "$fake/usr/local/sbin/helper")" = 755 ]
[ "$(stat -c %a "$fake/opt/java/lib/server/libjvm.so")" = 644 ]
chown 61125 "$fake/opt/n20-minecraft/bundle/server.jar"
if sh "$root/runtime/usr/local/sbin/n20-security-sysctls" --test-root "$fake" >/dev/null 2>&1; then
    echo 'Untrusted protected server owner was accepted' >&2; exit 1
fi
chown 0 "$fake/opt/n20-minecraft/bundle/server.jar"
chown 61125 "$fake/opt/java/lib/server/libjvm.so"
if sh "$root/runtime/usr/local/sbin/n20-security-sysctls" --test-root "$fake" >/dev/null 2>&1; then
    echo 'Untrusted Java library owner was accepted' >&2; exit 1
fi
chown 0 "$fake/opt/java/lib/server/libjvm.so"
ln -s /tmp/untrusted "$fake/opt/java/bin/java"
if sh "$root/runtime/usr/local/sbin/n20-security-sysctls" --test-root "$fake" >/dev/null 2>&1; then
    echo 'Escaping Java symlink was accepted' >&2; exit 1
fi
rm "$fake/opt/java/bin/java"
ln -s helper "$fake/usr/local/sbin/helper-alias"
sh "$root/runtime/usr/local/sbin/n20-security-sysctls" --test-root "$fake"
rm "$fake/usr/local/sbin/helper-alias"
for target in /tmp/untrusted ../untrusted; do
    ln -s "$target" "$fake/usr/local/sbin/helper-alias"
    if sh "$root/runtime/usr/local/sbin/n20-security-sysctls" --test-root "$fake" >/dev/null 2>&1; then
        echo 'Escaping privileged symlink was accepted' >&2; exit 1
    fi
    rm "$fake/usr/local/sbin/helper-alias"
done
chown 61125 "$fake/usr/local/sbin/helper"
if sh "$root/runtime/usr/local/sbin/n20-security-sysctls" --test-root "$fake" >/dev/null 2>&1; then
    echo 'Untrusted privileged file owner was accepted' >&2; exit 1
fi
chown 0 "$fake/usr/local/sbin/helper"
mv "$fake/usr/local/sbin" "$fake/usr/local/sbin-real"
ln -s sbin-real "$fake/usr/local/sbin"
if sh "$root/runtime/usr/local/sbin/n20-security-sysctls" --test-root "$fake" >/dev/null 2>&1; then
    echo 'Symlinked privileged directory was accepted' >&2; exit 1
fi
echo 'FILESYSTEM_PROTECTION_SETUP_PASSED'
