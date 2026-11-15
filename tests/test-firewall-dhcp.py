"""DHCP lease changes refresh the policy and expose failures, with fake tools."""
import os
from pathlib import Path
import subprocess
import tempfile

source=Path(__file__).resolve().parents[1]/'runtime/usr/local/sbin/n20-wifi-dhcp'
with tempfile.TemporaryDirectory(prefix='n20-firewall-dhcp-') as tmp:
    root=Path(tmp);events=root/'events';busybox=root/'busybox';firewall=root/'firewall';script=root/'dhcp'
    busybox.write_text('#!/bin/sh\nprintf "network %s\\n" "$*" >> "$EVENTS"\n')
    firewall.write_text('#!/bin/sh\nprintf "firewall\\n" >> "$EVENTS"\nexit "${FAIL_FIREWALL:-0}"\n')
    for helper in (busybox,firewall):helper.chmod(0o755)
    script.write_text(source.read_text().replace('/usr/local/bin/busybox',str(busybox)).replace('/usr/local/sbin/n20-firewall',str(firewall)))
    env=dict(os.environ,EVENTS=str(events),interface='wlan0',ip='10.1.2.3',mask='24',dns='',router='')
    for operation in ('bound','renew','deconfig'):
        events.write_text('')
        subprocess.run(['sh',str(script),operation],env=env,check=True)
        assert events.read_text().splitlines()[-1]=='firewall'
    env['FAIL_FIREWALL']='1'
    assert subprocess.run(['sh',str(script),'renew'],env=env).returncode==1
    firewall.unlink()
    assert subprocess.run(['sh',str(script),'renew'],env=env,stderr=subprocess.DEVNULL).returncode==1
print('DHCP_FIREWALL_REFRESH_FAILS_CLOSED_PASSED')
