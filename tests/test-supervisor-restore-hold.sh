#!/bin/sh
# Verify both restore holds gate server startup in an isolated mount namespace.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ "${1:-}" != --inside ]; then exec unshare -m sh "$0" --inside "$root"; fi
root=$2
mount --make-rprivate /
tmp=$(mktemp -d /tmp/n20-hold-test.XXXXXXXX)
case "$tmp" in /tmp/n20-hold-test.*) ;; *) exit 1 ;; esac
cleanup() {
    [ -z "${pid:-}" ] || kill "$pid" 2>/dev/null || :
    umount -l /opt/java 2>/dev/null || :
    umount -l /opt 2>/dev/null || :
    umount -l /var/lib/n20 2>/dev/null || :
    umount -l /srv/minecraft 2>/dev/null || :
    umount -l /run 2>/dev/null || :
}
trap cleanup EXIT
chmod 755 "$tmp"
mkdir -p "$tmp/server" "$tmp/meta" "$tmp/java/bin"
mount -t tmpfs -o mode=755 tmpfs /run
mount --bind "$tmp/server" /srv/minecraft
mount --bind "$tmp/meta" /var/lib/n20
mount -t tmpfs -o mode=755 tmpfs /opt
mkdir -p /opt/java /opt/n20-minecraft/current/repo
mount --bind "$tmp/java" /opt/java
mkdir -m 777 /run/proof
cat > /opt/java/bin/java <<'EOF'
#!/bin/sh
printf '%s\n' "$@" > /run/proof/args
touch /run/proof/launched
EOF
chmod 755 /opt/java /opt/java/bin /opt/java/bin/java
: > /opt/n20-minecraft/current/server.jar
: > /srv/minecraft/server.jar
printf 'eula=true\n' > /srv/minecraft/eula.txt
printf 'online-mode=true\n' > /srv/minecraft/server.properties
mkfifo /run/minecraft-console
chown minecraft:minecraft /run/minecraft-console
chmod 600 /run/minecraft-console
: > /var/lib/n20/restore-hold
timeout 100 sh "$root/runtime/usr/local/sbin/minecraft-supervisor" --server-only &
pid=$!
sleep 2
[ ! -e /run/proof/launched ] || { echo 'Root hold failed' >&2; exit 1; }
: > /srv/minecraft/.n20-restore-hold
rm /var/lib/n20/restore-hold
sleep 6
[ ! -e /run/proof/launched ] || { echo 'Legacy hold failed' >&2; exit 1; }
: > /var/lib/n20/low-battery-hold
rm /srv/minecraft/.n20-restore-hold
sleep 6
[ ! -e /run/proof/launched ] || { echo 'Root battery hold failed' >&2; exit 1; }
: > /srv/minecraft/.n20-low-battery-hold
rm /var/lib/n20/low-battery-hold
sleep 6
[ ! -e /run/proof/launched ] || { echo 'Legacy battery hold failed' >&2; exit 1; }
rm /srv/minecraft/.n20-low-battery-hold
mv /srv/minecraft/eula.txt "$tmp/valid-eula"
printf 'eula=true\n' > "$tmp/private-eula"; chmod 600 "$tmp/private-eula"
ln -s "$tmp/private-eula" /srv/minecraft/eula.txt
sleep 6
[ ! -e /run/proof/launched ] || { echo 'Root-only EULA target was accepted' >&2; exit 1; }
rm /srv/minecraft/eula.txt
mkfifo /srv/minecraft/eula.txt
sleep 13
[ ! -e /run/proof/launched ] || { echo 'FIFO EULA was accepted' >&2; exit 1; }
rm /opt/n20-minecraft/current/server.jar
rm /srv/minecraft/eula.txt
mv "$tmp/valid-eula" /srv/minecraft/eula.txt
sleep 6
[ ! -e /run/proof/launched ] || { echo 'Writable legacy JAR fallback was accepted' >&2; exit 1; }
: > /opt/n20-minecraft/current/server.jar
i=0
while [ ! -e /run/proof/launched ]; do
    i=$((i+1)); [ "$i" -lt 140 ] || { echo 'Server did not start after holds cleared' >&2; exit 1; }
    sleep 0.1
done
grep -Fx -- '-DbundlerRepoDir=/opt/n20-minecraft/current/repo' /run/proof/args >/dev/null
grep -Fx -- '/opt/n20-minecraft/current/server.jar' /run/proof/args >/dev/null
echo 'ROOT_AND_LEGACY_RECOVERY_AND_BATTERY_HOLDS_GATE_STARTUP_PASSED'
