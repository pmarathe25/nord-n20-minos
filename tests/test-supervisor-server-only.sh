#!/bin/sh
# Exercise the maintenance-only supervisor in an isolated mount namespace.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ "${1:-}" != --inside ]; then
    exec unshare -m sh "$0" --inside "$root"
fi
root=$2
mount --make-rprivate /
tmp=$(mktemp -d /tmp/n20-supervisor-test.XXXXXXXX)
case "$tmp" in /tmp/n20-supervisor-test.*) ;; *) exit 1 ;; esac
cleanup() { umount -l /srv/minecraft 2>/dev/null || :; umount -l /run 2>/dev/null || :; rmdir "$tmp/server" "$tmp" 2>/dev/null || :; }
trap cleanup EXIT
mkdir "$tmp/server"
mount -t tmpfs -o mode=755 tmpfs /run
mount --bind "$tmp/server" /srv/minecraft
mkfifo /run/minecraft-console
chown minecraft:minecraft /run/minecraft-console
chmod 600 /run/minecraft-console
original=$(stat -c '%i' /run/minecraft-console)
timeout 3 sh "$root/runtime/usr/local/sbin/minecraft-supervisor" --server-only &
pid=$!
i=0
while [ ! -f /run/n20/server-transition.lock ]; do
    i=$((i+1)); [ "$i" -lt 20 ] || { echo 'Supervisor did not create its FIFO' >&2; exit 1; }
    sleep 0.1
done
test "$(stat -c '%a %U:%G' /run/minecraft-console)" = '600 minecraft:minecraft'
test "$(stat -c '%i' /run/minecraft-console)" = "$original"
test -f /run/n20/server-transition.lock
status=0; wait "$pid" || status=$?
[ "$status" -eq 124 ] || { echo 'Unexpected supervisor exit' >&2; exit 1; }
rm /run/minecraft-console
if sh "$root/runtime/usr/local/sbin/minecraft-supervisor" --server-only >/dev/null 2>&1; then
    echo 'Supervisor accepted a missing console' >&2; exit 1
fi
[ ! -e /run/minecraft-console ]
echo 'ISOLATED_SERVER_ONLY_SUPERVISOR_PASSED'
