#!/bin/sh
# Exercise the real launcher with fake workers in an isolated mount namespace.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ "${1:-}" != --inside ]; then exec unshare -m sh "$0" --inside "$root"; fi
root=$2
mount --make-rprivate /
tmp=$(mktemp -d /tmp/n20-demand-launcher.XXXXXXXX)
case "$tmp" in /tmp/n20-demand-launcher.*) ;; *) exit 1 ;; esac
pid=
cleanup() {
    status=$?
    [ -z "$pid" ] || { kill "$pid" 2>/dev/null || :; wait "$pid" 2>/dev/null || :; }
    if [ "$status" -ne 0 ] && [ -f "$tmp/output" ]; then cat "$tmp/output" >&2; fi
    umount -l /usr/local/lib/n20-radio/android/linker64 2>/dev/null || :
    umount -l /usr/local/lib/n20 2>/dev/null || :
    umount -l /usr/local/sbin 2>/dev/null || :
    umount -l /var/lib/n20-wiki 2>/dev/null || :
    umount -l /opt 2>/dev/null || :
    umount -l /run 2>/dev/null || :
    rm -rf -- "$tmp"
}
trap cleanup EXIT
chmod 755 "$tmp"
mkdir -p "$tmp/sbin" "$tmp/lib" "$tmp/wiki-state"
cp "$root/runtime/usr/local/lib/n20/wiki_demand.py" "$tmp/lib/wiki_demand.py"
cat > "$tmp/lib/wiki_chat.py" <<'EOF'
import os,time
from pathlib import Path
p=Path('/run/n20-wiki/status')
Path('/run/proof/bridge-pid').write_text(str(os.getpid()))
while not (p/'request').exists(): time.sleep(.05)
start=Path('/proc/self/stat').read_text().rpartition(')')[2].split()[19]
fd=os.open(p/'model-demand',os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
with os.fdopen(fd,'w') as f:f.write(f'{os.getpid()} {start}\n')
while not (p/'release').exists(): time.sleep(.05)
(p/'model-demand').unlink()
while True: time.sleep(1)
EOF
chmod 644 "$tmp/lib/wiki_chat.py" "$tmp/lib/wiki_demand.py"
cat > "$tmp/sbin/n20-service-sandbox" <<'EOF'
#!/bin/sh
shift 2
exec "$@"
EOF
cat > "$tmp/sbin/n20-resource-policy" <<'EOF'
#!/bin/sh
[ ! -e /run/proof/deny ]
EOF
printf '#!/bin/sh\nexit 0\n' > "$tmp/sbin/n20-service-health"
cat > "$tmp/sbin/n20-stop-process" <<'EOF'
#!/bin/sh
kill "$1" 2>/dev/null || :
exit 0
EOF
chmod 755 "$tmp/sbin/"*
chmod 755 "$tmp/lib"
mount -t tmpfs -o mode=755 tmpfs /run
mount -t tmpfs -o mode=755 tmpfs /opt
mount --bind "$tmp/sbin" /usr/local/sbin
mount --bind "$tmp/lib" /usr/local/lib/n20
mount --bind "$tmp/wiki-state" /var/lib/n20-wiki
mkdir -p /run/n20 /run/proof /opt/n20-wiki/opencl/bin /usr/local/lib/n20-radio/android
chmod 777 /run/proof
: > /run/n20-wiki-test-enabled
: > /opt/n20-wiki/model.gguf
: > /opt/n20-wiki/opencl/cl-compat.so
: > /opt/n20-wiki/opencl/bin/llama-server
: > /opt/n20-wiki/n20-wiki
chmod 755 /opt/n20-wiki/opencl/bin/llama-server /opt/n20-wiki/n20-wiki
cat > "$tmp/linker64" <<'EOF'
#!/bin/sh
touch /run/proof/model-started
printf '%s\n' "$$" > /run/proof/model-pid
exec /bin/sleep 60
EOF
chmod 755 "$tmp/linker64"
mount --bind "$tmp/linker64" /usr/local/lib/n20-radio/android/linker64
# Keep the policy and demand paths intact. Fake the hardware nodes and shorten
# only the idle delay so this integration test does not wait ten minutes.
sed -e 's|^enabled=/etc/n20-wiki-enabled$|enabled=/run/n20-wiki-test-enabled|' \
    -e 's|^node /dev/kgsl-3d0 .*|:|' -e 's|^node /dev/ion .*|:|' \
    -e 's| -ge 600 | -ge 2 |' "$root/runtime/usr/local/sbin/n20-wiki-start" > "$tmp/launcher"
sh "$tmp/launcher" > "$tmp/output" 2>&1 &
pid=$!
i=0
while [ ! -d /run/n20-wiki/status ]; do
    i=$((i+1)); [ "$i" -lt 60 ] || { echo 'Bridge did not start' >&2; exit 1; }
    kill -0 "$pid" 2>/dev/null || { echo 'Launcher exited' >&2; exit 1; }
    sleep .1
done
sleep 3
[ ! -e /run/proof/model-started ] || { echo 'Model started without demand' >&2; exit 1; }
i=0
while [ ! -f /run/proof/bridge-pid ]; do
    i=$((i+1)); [ "$i" -lt 60 ] || { echo 'Fake bridge did not start' >&2; exit 1; }
    sleep .1
done
printf '%s 0\n' "$(cat /run/proof/bridge-pid)" > /run/n20-wiki/status/model-demand
chown minecraft:minecraft /run/n20-wiki/status/model-demand
chmod 600 /run/n20-wiki/status/model-demand
sleep 2
[ ! -e /run/proof/model-started ] || { echo 'Stale demand started the model' >&2; exit 1; }
rm /run/n20-wiki/status/model-demand
: > /run/proof/deny
: > /run/n20-wiki/status/request
i=0
while [ ! -e /run/n20-wiki/status/model-demand ]; do
    i=$((i+1)); [ "$i" -lt 60 ] || { echo 'Bridge did not request model' >&2; exit 1; }
    sleep .1
done
sleep 2
[ ! -e /run/proof/model-started ] || { echo 'Model ignored the resource guard' >&2; exit 1; }
rm /run/proof/deny
i=0
while [ ! -e /run/proof/model-started ]; do
    i=$((i+1)); [ "$i" -lt 100 ] || { echo 'Model did not start on demand' >&2; exit 1; }
    kill -0 "$pid" 2>/dev/null || { echo 'Launcher exited while starting model' >&2; exit 1; }
    sleep .1
done
: > /run/n20-wiki/status/release
i=0
while [ -e /run/n20-wiki/status/model-demand ]; do
    i=$((i+1)); [ "$i" -lt 60 ] || { echo 'Demand did not clear' >&2; exit 1; }
    sleep .1
done
sleep 4
model_pid=$(cat /run/proof/model-pid)
if [ -r "/proc/$model_pid/stat" ] && [ "$(awk '{print $4}' "/proc/$model_pid/stat")" = "$pid" ]; then
    echo 'Idle model worker did not stop' >&2; exit 1
fi
echo 'MODEL_STARTS_ON_DEMAND_AND_STOPS_WHEN_IDLE_PASSED'
