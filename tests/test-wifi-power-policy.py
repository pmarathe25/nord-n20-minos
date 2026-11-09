#!/usr/bin/env python3
"""Exercise automatic Wi-Fi policy with fake process and TCP state."""
import importlib.machinery
import os
from pathlib import Path
import tempfile
import time

root = Path(__file__).resolve().parents[1]
module = importlib.machinery.SourceFileLoader(
    'wifi_power_policy', str(root / 'runtime/usr/local/sbin/n20-wifi-power-policy')).load_module()

with tempfile.TemporaryDirectory(prefix='n20-wifi-power-') as tmp:
    base = Path(tmp); config = base / 'server.properties'
    config.write_text('server-port : 25565\n')
    assert module.server_port(config) == 25565
    config.write_text('server-port 25566\n')
    assert module.server_port(config) == 25566
    config.write_text('other=one\n server-port\t=\t25567 \nunrelated=two\n')
    assert module.server_port(config) == 25567
    config.write_text('server-port-extra=25565\n')
    try: module.server_port(config); raise AssertionError('ambiguous port setting accepted')
    except ValueError: pass
    config.write_text('server-port=invalid\n')
    try: module.server_port(config); raise AssertionError('invalid port accepted')
    except ValueError: pass
    config.write_text('other=value\n'); assert module.server_port(config) == 25565
    config.unlink();target=base/'target';target.write_text('server-port=1234\n');config.symlink_to(target)
    try: module.server_port(config); raise AssertionError('symlink properties accepted')
    except OSError: pass
    config.unlink();os.link(target,config)
    try: module.server_port(config); raise AssertionError('hardlinked properties accepted')
    except ValueError: pass
    config.unlink();os.mkfifo(config)
    try: module.server_port(config); raise AssertionError('FIFO properties accepted')
    except ValueError: pass
    config.unlink();target.rename(config)
    proc = base / 'proc'; process = proc / '123'; (process / 'fd').mkdir(parents=True)
    (process / 'comm').write_text('java\n'); (base / 'pid').write_text('123\n')
    os.symlink('socket:[42]', process / 'fd' / '4')
    os.symlink('socket:[42]junk', process / 'fd' / '5')
    os.symlink('socket:[not-a-number]', process / 'fd' / '6')
    os.symlink('socket:[]', process / 'fd' / '7')
    sockets = module.game_socket_inodes(base / 'pid', proc); assert sockets == {'42'}
    net = base / 'net'; net.mkdir()
    (net / 'tcp').write_text('header\n')
    listener = '0: 00000000:63DD 00000000:0000 0A 0 0 0 0 0 42\n'
    connection = '1: 00000000:63DD 00000000:0000 01 0 0 0 0 0 43\n'
    (net / 'tcp6').write_text('header\n' + listener)
    assert module.socket_activity(25565, sockets, net) is False
    assert module.socket_activity(25565, {'99'}, net) is None
    (net / 'tcp6').write_text('header\n' + listener + connection)
    assert module.socket_activity(25565, sockets, net) is True
    (net / 'tcp6').write_text('header\nbroken\n')
    try: module.socket_activity(25565, sockets, net); raise AssertionError('invalid table accepted')
    except ValueError: pass
    (process / 'comm').write_text('not-java\n')
    try: module.game_socket_inodes(base / 'pid', proc); raise AssertionError('wrong process accepted')
    except ValueError: pass
    p = module.IdlePolicy()
    assert not p.power_save(0, False)
    assert not p.power_save(9.9, False)
    assert p.power_save(10, False)
    assert not p.power_save(10.1, True)
    assert not p.power_save(30, None)
    assert not p.power_save(31, False)

    heartbeat = base / 'wifi-power-policy.heartbeat'; heartbeat.touch()
    assert module.heartbeat_fresh(heartbeat, time.time())
    os.utime(heartbeat, (time.time() - 30, time.time() - 30))
    assert not module.heartbeat_fresh(heartbeat, time.time())
print('WIFI_POWER_POLICY_AND_FAILSAFE_PASSED')
