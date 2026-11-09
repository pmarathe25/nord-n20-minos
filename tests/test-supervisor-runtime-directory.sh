#!/bin/sh
# Verify real service launch order with isolated mounts and fake services.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ "${1:-}" != --inside ]; then
    exec unshare -m sh "$0" --inside "$root"
fi
root=$2
mount --make-rprivate /
tmp=$(mktemp -d /tmp/n20-runtime-order.XXXXXXXX)
case "$tmp" in /tmp/n20-runtime-order.*) ;; *) exit 1 ;; esac
cleanup() {
    umount -l /etc/ssh 2>/dev/null || :
    umount -l /usr/local/sbin 2>/dev/null || :
    umount -l /usr/local/bin 2>/dev/null || :
    umount -l /var/log 2>/dev/null || :
    umount -l /srv/minecraft 2>/dev/null || :
    umount -l /run 2>/dev/null || :
    rm -rf -- "$tmp"
}
trap cleanup EXIT
mkdir -p "$tmp/bin" "$tmp/sbin" "$tmp/server" "$tmp/log" "$tmp/ssh" "$tmp/security/proc/sys/fs" "$tmp/security/usr/local/sbin"
chmod 777 "$tmp/security/usr/local/sbin"
for key in protected_hardlinks protected_symlinks protected_fifos protected_regular; do printf '0\n' > "$tmp/security/proc/sys/fs/$key"; done
cp /usr/local/bin/busybox "$tmp/bin/busybox"
cat >"$tmp/sbin/n20-mtp-start" <<'EOF'
#!/bin/sh
safe=1
[ -d /run/n20 ] && [ "$(stat -c '%u %a' /run/n20)" = '0 700' ] || safe=0
[ "$(stat -c %a "$CHECK_SECURITY_ROOT/usr/local/sbin")" = 755 ] || safe=0
for key in protected_hardlinks protected_symlinks protected_fifos protected_regular; do
    [ "$(cat "$CHECK_SECURITY_ROOT/proc/sys/fs/$key")" = 1 ] || safe=0
done
if [ "$safe" = 1 ]; then
    printf 'safe\n' > "$CHECK_FILE"
else
    printf 'unsafe\n' > "$CHECK_FILE"
fi
EOF
cat >"$tmp/sbin/n20-security-sysctls" <<'EOF'
#!/bin/sh
exec /bin/sh "$CHECK_SECURITY_HELPER" --test-root "$CHECK_SECURITY_ROOT"
EOF
for service in n20-radio-start n20-wifi-manager; do
    printf '#!/bin/sh\nsleep 2\n' > "$tmp/sbin/$service"
done
chmod 755 "$tmp/sbin/"*
export CHECK_FILE="$tmp/result"
export CHECK_SECURITY_ROOT="$tmp/security"
export CHECK_SECURITY_HELPER="$root/runtime/usr/local/sbin/n20-security-sysctls"
mount -t tmpfs -o mode=755 tmpfs /run
mount --bind "$tmp/server" /srv/minecraft
mount --bind "$tmp/log" /var/log
mount --bind "$tmp/bin" /usr/local/bin
mount --bind "$tmp/sbin" /usr/local/sbin
mount --bind "$tmp/ssh" /etc/ssh
timeout 4 sh "$root/runtime/usr/local/sbin/minecraft-supervisor" &
pid=$!
i=0
while [ ! -f "$CHECK_FILE" ]; do
    i=$((i+1)); [ "$i" -lt 30 ] || { echo 'MTP stub not started' >&2; exit 1; }
    sleep .1
done
[ "$(cat "$CHECK_FILE")" = safe ] || { echo 'MTP launched before safe runtime directory' >&2; exit 1; }
status=0; wait "$pid" || status=$?
[ "$status" -eq 124 ] || { echo 'Unexpected supervisor exit' >&2; exit 1; }
rm "$CHECK_FILE" "$CHECK_SECURITY_ROOT/proc/sys/fs/protected_regular"
if timeout 3 sh "$root/runtime/usr/local/sbin/minecraft-supervisor" >/dev/null 2>&1; then
    echo 'Supervisor accepted a failed protection check' >&2; exit 1
else
    status=$?
    [ "$status" -eq 1 ] || { echo 'Unexpected failed-protection result' >&2; exit 1; }
fi
[ ! -e "$CHECK_FILE" ] || { echo 'Service started after protection failure' >&2; exit 1; }
rm "$tmp/sbin/n20-security-sysctls"
if timeout 3 sh "$root/runtime/usr/local/sbin/minecraft-supervisor" >/dev/null 2>&1; then
    echo 'Supervisor accepted missing filesystem protection helper' >&2; exit 1
else
    status=$?
    [ "$status" -eq 1 ] || { echo 'Unexpected missing-helper result' >&2; exit 1; }
fi
echo 'RUNTIME_DIRECTORY_READY_BEFORE_SERVICES_PASSED'
