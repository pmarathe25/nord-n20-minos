"""Policy generation/dual-stack rollback and isolated real legacy rule install."""
import importlib.machinery
import json
from pathlib import Path
import subprocess
import sys
import socket
from unittest.mock import patch

source=Path(__file__).resolve().parents[1]/'runtime/usr/local/sbin/n20-firewall'
m=importlib.machinery.SourceFileLoader('firewall_test',str(source)).load_module()
addresses=[{'ifname':'wlan0','addr_info':[{'family':'inet','local':'10.0.0.160','prefixlen':24}]},
           {'ifname':'usb0','addr_info':[{'family':'inet','local':'192.168.137.2','prefixlen':24}]}]
v4,v6=m.policy(4,addresses),m.policy(6,addresses)
assert '-i wlan0 -s 10.0.0.0/24 -p tcp --dport 22' in v4
assert '-i usb0 -s 192.168.137.0/24 -p tcp --dport 22' in v4
assert '--dport 22' not in v6
for text in (v4,v6):
    assert ':INPUT DROP' in text and ':FORWARD DROP' in text and ':OUTPUT ACCEPT' in text
    assert 'ESTABLISHED,RELATED' in text and '--dport 25565 -j ACCEPT' in text
assert '10.0.0.0/24' not in m.policy(4,[])
calls=[]
def run(args,**kwargs):
    calls.append((args,kwargs['input']))
    if len(calls)==2:raise subprocess.CalledProcessError(1,args)
def output(args,**kwargs):
    if '-j' in args:return json.dumps(addresses)
    return 'previous-four' if 'iptables-' in args[0] else 'previous-six'
with patch.object(m.subprocess,'check_output',side_effect=output),patch.object(m.subprocess,'run',side_effect=run):
    try:m.main()
    except subprocess.CalledProcessError:pass
    else:raise AssertionError('failed IPv6 install was accepted')
assert len(calls)==4 and calls[2][1]=='previous-four' and calls[3][1]=='previous-six'
calls.clear()
def rollback_failure(args,**kwargs):
    calls.append((args,kwargs['input']))
    if len(calls) in (2,3):raise subprocess.CalledProcessError(1,args)
with patch.object(m.subprocess,'check_output',side_effect=output),patch.object(m.subprocess,'run',side_effect=rollback_failure):
    try:m.main()
    except RuntimeError as exc:assert 'rollback errors' in str(exc)
    else:raise AssertionError('rollback failure not reported')
assert len(calls)==4 and calls[3][1]=='previous-six'
if sys.argv[1:]!=['--inside']:
    subprocess.run(['unshare','-n',sys.executable,__file__,'--inside'],check=True)
else:
    # The real vendor kernel must accept both policies, entirely off-host.
    for tool,text in (('iptables-legacy-restore',v4),('ip6tables-legacy-restore',v6)):
        subprocess.run([tool,'--test'],input=text,text=True,check=True)
        subprocess.run([tool],input=text,text=True,check=True)
    for tool in ('iptables-legacy','ip6tables-legacy'):
        rules=subprocess.check_output([tool,'-S'],text=True)
        assert '-P INPUT DROP' in rules and '-P FORWARD DROP' in rules
    peer=subprocess.Popen(['unshare','-n','sleep','30'])
    listeners=[]
    try:
        # Real packets between two disposable namespaces prove both allowed
        # game/admin traffic and denied unsolicited traffic in both families.
        import time
        time.sleep(.1)
        def ip(*args):subprocess.run(['ip',*args],check=True)
        def peer_ip(*args):subprocess.run(['nsenter','-t',str(peer.pid),'-n','ip',*args],check=True)
        ip('link','add','wlan0','type','veth','peer','name','peer0')
        ip('link','set','peer0','netns',str(peer.pid))
        ip('addr','add','10.0.0.160/24','dev','wlan0')
        ip('-6','addr','add','fd42::1/64','dev','wlan0','nodad')
        ip('link','set','wlan0','up')
        peer_ip('addr','add','10.0.0.10/24','dev','peer0')
        peer_ip('-6','addr','add','fd42::2/64','dev','peer0','nodad')
        peer_ip('link','set','peer0','up')
        for family,host in ((socket.AF_INET,'10.0.0.160'),(socket.AF_INET6,'fd42::1')):
            for port in (22,25565,25600):
                listener=socket.socket(family,socket.SOCK_STREAM)
                listener.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
                if family==socket.AF_INET6:listener.setsockopt(socket.IPPROTO_IPV6,socket.IPV6_V6ONLY,1)
                listener.bind((host,port));listener.listen(5);listeners.append(listener)
            check='''import socket,sys
host=sys.argv[1]
for port in (22,25565,25600):
 s=socket.socket(socket.AF_INET6 if ':' in host else socket.AF_INET,socket.SOCK_STREAM);s.settimeout(.5)
 try:s.connect((host,port));ok=True
 except socket.timeout:ok=False
 finally:s.close()
 assert ok==(port==25565 or (port==22 and ':' not in host)),(host,port,ok)
'''
            subprocess.run(['nsenter','-t',str(peer.pid),'-n',sys.executable,'-c',check,host],check=True)
    finally:
        for listener in listeners:listener.close()
        peer.terminate();peer.wait(timeout=3)
    print('LEGACY_FIREWALL_DUALSTACK_AND_ROLLBACK_PASSED')
