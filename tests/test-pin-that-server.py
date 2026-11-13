#!/usr/bin/env python3
"""Exercise ray commands in a disposable, loopback-only Minecraft server."""
import argparse
import os
from pathlib import Path
import pwd
import shutil
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'runtime/usr/local/lib/n20'))
from wiki_chat import RAY_BATCH_SIZE, RAY_DATA, RAY_SAMPLE, ray_sample_base, ray_voxels, raycast_commands, view_capture_commands


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--jar', type=Path, required=True)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--eula', type=Path, required=True)
    args = parser.parse_args()
    if os.geteuid() != 0 or 'eula=true' not in args.eula.read_text().splitlines():
        parser.error('requires root and a previously accepted EULA')
    account = pwd.getpwnam('minecraft')
    with tempfile.TemporaryDirectory(prefix='n20-raycast-test-', dir='/tmp') as name:
        root = Path(name)
        os.chown(root, account.pw_uid, account.pw_gid)
        shutil.copyfile(args.eula, root / 'eula.txt')
        (root / 'server.properties').write_text('server-ip=127.0.0.1\nserver-port=0\nonline-mode=true\nwhite-list=true\nenforce-whitelist=true\nmanagement-server-enabled=false\nenable-rcon=false\nview-distance=2\nsimulation-distance=2\npause-when-empty-seconds=0\nlevel-type=minecraft:flat\ngenerate-structures=false\n')
        for path in (root / 'eula.txt', root / 'server.properties'):
            os.chown(path, account.pw_uid, account.pw_gid)
        output = root / 'test.log'
        with output.open('wb') as log:
            proc = subprocess.Popen(['nice', '-n', '15', 'setpriv', '--no-new-privs', '--bounding-set=-all', '--reuid=minecraft', '--regid=minecraft', '--clear-groups', '/opt/java/bin/java', '-Xms128M', '-Xmx768M', '-DbundlerRepoDir=' + str(args.repo), '-jar', str(args.jar), 'nogui'], cwd=root, stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            def wait_for(value, seconds=120):
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    if proc.poll() is not None:
                        raise RuntimeError('disposable server exited')
                    if value in output.read_bytes():
                        return
                    time.sleep(.02)
                raise RuntimeError('disposable server timed out')
            def send(value):
                proc.stdin.write(value.encode('ascii'))
                proc.stdin.flush()
            try:
                wait_for(b'Done (')
                send('forceload add 0 0 0 512\n')
                deadline = time.monotonic() + 60
                while time.monotonic() < deadline:
                    send('execute in minecraft:overworld positioned 0 101 512 if blocks ~ ~ ~ ~ ~ ~ ~ ~ ~ all run compute default integer {type:"minecraft:constant",value:765499}\n')
                    time.sleep(.5)
                    if 'Number provider returned value 765499' in output.read_text(errors='replace'):
                        break
                else:
                    raise RuntimeError('distant test chunk did not load')
                send('setblock 0 101 1 minecraft:glass\nsetblock 0 101 2 minecraft:red_stained_glass\nsetblock 0 101 3 minecraft:blue_stained_glass_pane\nsetblock 0 101 4 minecraft:stone\nsetblock 0 101 6 minecraft:stone\nsetblock 0 101 41 minecraft:stone\nsetblock 0 101 512 minecraft:stone\nsummon minecraft:armor_stand 0.5 100 0.5 {Tags:["n20_test"],NoGravity:1b,Rotation:[0.0f,0.0f]}\n')
                selector = '@e[type=minecraft:armor_stand,tag=n20_test,limit=1]'
                capture_token = 'n20_pin_' + 'a' * 32
                send(view_capture_commands('Alex', capture_token).replace('Alex', selector))
                wait_for(('Found no elements matching ' + capture_token).encode(), 45)
                captured = [match for line in output.read_text(errors='replace').splitlines()
                            if (match := RAY_DATA.fullmatch(line)) and match[1] == capture_token]
                if len(captured) != 1:
                    raise AssertionError('expected one eye-position capture')
                eye = [float(value.strip().removesuffix('d')) for value in captured[0][2].strip('[]').split(',')]
                snapshot = {'eye': eye, 'rotation': [0.0, 0.0], 'dimension': 'minecraft:overworld'}
                # Movement after the capture must not change the ray.
                send('tp @e[type=minecraft:armor_stand,tag=n20_test,limit=1] 10 100 0 180 0\n')
                cells = ray_voxels(snapshot)
                counter = 0
                def cast():
                    nonlocal counter
                    started = time.monotonic()
                    for start in range(0, len(cells), RAY_BATCH_SIZE):
                        batch = cells[start:start + RAY_BATCH_SIZE]
                        counter += 1
                        token = 'n20_pin_' + f'{counter:032x}'
                        offset = output.stat().st_size
                        for command in raycast_commands(snapshot, token, batch):
                            send(command)
                        wait_for(('Found no elements matching ' + token).encode(), 45)
                        lines = output.read_bytes()[offset:].decode(errors='replace').splitlines()
                        base = ray_sample_base(token)
                        results = {}
                        for line in lines:
                            match = RAY_SAMPLE.fullmatch(line)
                            if match:
                                index, hit = divmod(int(match[1]) - base, 2)
                                if 0 <= index < len(batch):
                                    results[index] = bool(hit)
                        for index, cell in enumerate(batch):
                            if index not in results:
                                raise AssertionError('ray reached an unavailable test block')
                            if results[index]:
                                print('isolated ray seconds and checks:', round(time.monotonic() - started, 2), start + index + 1)
                                return cell
                        time.sleep(.05)
                    print('isolated ray seconds and checks:', round(time.monotonic() - started, 2), len(cells))
                    return None
                if cast() != (0, 101, 2):
                    raise AssertionError('ray did not hit stained glass behind clear glass')
                send('setblock 0 101 2 minecraft:glass_pane\n')
                if cast() != (0, 101, 3):
                    raise AssertionError('ray did not hit stained glass pane behind clear glass pane')
                send('setblock 0 101 3 minecraft:tinted_glass\n')
                if cast() != (0, 101, 3):
                    raise AssertionError('ray did not hit tinted glass')
                send('setblock 0 101 3 air\nsetblock 0 101 4 air\nsetblock 0 101 6 air\n')
                if cast() != (0, 101, 41):
                    raise AssertionError('ray missed a one-block distant target')
                send('setblock 0 101 41 air\n')
                if cast() != (0, 101, 512):
                    raise AssertionError('ray did not find the maximum-range block')
                send('setblock 0 101 512 air\n')
                if cast() is not None:
                    raise AssertionError('ray reported a hit in empty air')
                send(f'execute if entity @e[type=minecraft:area_effect_cloud,tag={capture_token}] run data get storage n20:raycheck leak\n')
                final = 'n20_pin_' + '9' * 32
                send(f'data get storage n20:{final} {final}\n')
                wait_for(('Found no elements matching ' + final).encode(), 10)
                if 'Found no elements matching leak' in output.read_text(errors='replace'):
                    raise AssertionError('temporary ray entity was not removed')
                print('Disposable server glass, block-by-block near and distant hits, miss, and cleanup passed')
                send('stop\n')
                if proc.wait(timeout=60) != 0:
                    raise RuntimeError('disposable server did not stop cleanly')
            finally:
                if proc.poll() is None:
                    os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait()


if __name__ == '__main__':
    main()
