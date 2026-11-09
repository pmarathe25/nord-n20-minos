import importlib.machinery,json,os,tempfile,time
from pathlib import Path
m=importlib.machinery.SourceFileLoader('n20_admin_ui',str(Path(__file__).resolve().parents[1] / 'runtime/usr/local/sbin/n20-admin')).load_module()
top=Path(tempfile.mkdtemp(prefix='n20-settings-validation-',dir='/tmp'))
m.ROOT=top/'server';m.ROOT.mkdir();m.WORLD=m.ROOT/'world';m.RUN=top/'run';m.META=top/'meta';m.PID=top/'pid';m.STOP=top/'stop';m.LEGACY_HOLD=m.ROOT/'.n20-restore-hold'; m.HOLD=m.META/'restore-hold';m.LEGACY_LOW=m.ROOT/'.n20-low-battery-hold'; m.LOW=m.META/'low-battery-hold';m.OP=m.RUN/'operation';m.LOCK=m.RUN/'operation.lock';m.ERR=m.META/'error';m.CACHE=m.META/'cache';m.directories()
m.BUNDLE=top/'bundle';(m.BUNDLE/'repo').mkdir(parents=True);(m.BUNDLE/'server.jar').write_bytes(b'test')
secret=top/'private';secret.write_text('difficulty=secret\n');config=m.ROOT/'server.properties'
config.symlink_to(secret);assert m.props()=={}
try:m.set_property('difficulty','normal');raise AssertionError('symlink settings accepted')
except m.AdminError:pass
assert secret.read_text()=='difficulty=secret\n';config.unlink()
os.link(secret,config);assert m.props()=={}
try:m.set_property('difficulty','normal');raise AssertionError('hardlinked settings accepted')
except m.AdminError:pass
config.unlink()
os.mkfifo(config);assert m.props()=={}
try:m.set_property('difficulty','normal');raise AssertionError('FIFO settings accepted')
except m.AdminError:pass
config.unlink()
config.write_bytes(b'x'*(1024*1024+1));assert m.props()=={}
try:m.set_property('difficulty','normal');raise AssertionError('oversized settings accepted')
except m.AdminError:pass
config.unlink()
(m.ROOT/'whitelist.json').symlink_to(secret);assert m.whitelist_entries() is None;(m.ROOT/'whitelist.json').unlink()
(m.ROOT/'server.properties').write_text('difficulty=easy\nmax-players=5\nview-distance=6\nsimulation-distance=4\nwhite-list=false\n')
(m.ROOT/'whitelist.json').write_text('[]');m.PID.write_text('111');m.pid_running=lambda:True;m.mc_status=lambda:{'players':{'online':0}}
assert m.configured_server_port()==25565
config.write_text('server-port=25570\n');assert m.configured_server_port()==25570
proc=top/'fake-proc';net=top/'fake-net';(proc/'111'/'fd').mkdir(parents=True);net.mkdir()
(proc/'111'/'comm').write_text('java\n');os.symlink('socket:[42]',proc/'111'/'fd'/'4')
(net/'tcp').write_text('header\n0: 00000000:63E2 00000000:0000 0A 0 0 0 0 0 42\n');(net/'tcp6').write_text('header\n')
assert m.status_endpoint(proc,net)==('127.0.0.1',25570)
(net/'tcp').write_text('header\n0: 00000000:63E2 00000000:0000 0A 0 0 0 0 0 43\n')
try:m.status_endpoint(proc,net);raise AssertionError('unowned listener accepted')
except ValueError:pass
(net/'tcp').write_text('header\n0: 6401A8C0:63E2 00000000:0000 0A 0 0 0 0 0 42\n')
try:m.status_endpoint(proc,net);raise AssertionError('non-loopback listener accepted for loopback probe')
except ValueError:pass
(net/'tcp').write_text('header\n');(net/'tcp6').write_text('header\n0: 00000000000000000000000000000000:63E2 00000000000000000000000000000000:0000 0A 0 0 0 0 0 42\n')
assert m.status_endpoint(proc,net)==('::1',25570)
config.write_text('server-port=0\n')
try:m.configured_server_port();raise AssertionError('invalid port accepted')
except ValueError:pass
config.write_text('difficulty=easy\n')
(m.ROOT/'whitelist.json').write_text('{}')
m.make_state(); state=(m.RUN/'state').read_text()
assert 'whitelist=unknown\n' in state and 'allow_count=-1\n' in state and 'backup_count=-1\n' in state
m.CACHE.write_text('{invalid');m.LOW.write_text('hold')
m.make_state(); state=(m.RUN/'state').read_text()
assert 'backup_count=-1\n' in state and 'error=Battery low: waiting for charge\n' in state
m.LOW.unlink();m.CACHE.write_text('[]')
config.write_text('difficulty=easy\nmax-players=5\nview-distance=6\nsimulation-distance=4\nwhite-list=false\n')
(m.ROOT/'whitelist.json').write_text('[]')
m.make_state(); state=(m.RUN/'state').read_text()
assert 'whitelist=off\n' in state and 'allow_count=0\n' in state and 'backup_count=0\n' in state
assert 'charge_stop=55\n' in state and 'charge_resume=50\n' in state
# The dashboard should report the same time-gate failures as the supervisor.
assert not m.credible_clock(100) and not m.credible_clock(4102444801)
(m.META/'trusted-time').write_text('1800000000\n')
assert m.credible_clock(1800000000) and not m.credible_clock(1799913599)
assert m.credible_clock(1799913600)
m.pid_running=lambda:False
(m.BUNDLE/'server.jar').unlink();m.make_state();state=(m.RUN/'state').read_text()
assert 'error=Server files unavailable: install protected bundle\n' in state
(m.BUNDLE/'server.jar').write_bytes(b'test')
real_time=m.time.time
try:
    m.time.time=lambda:1799913599
    m.make_state(); state=(m.RUN/'state').read_text()
    assert 'server=starting\n' in state and 'error=Clock invalid: set time using USB or SSH\n' in state
finally:
    m.time.time=real_time
m.pid_running=lambda:True
commands=[]
def fake_console(command):
    commands.append(command)
    if command=='whitelist add TestUser': (m.ROOT/'whitelist.json').write_text(json.dumps([{'name':'TestUser'}]))
    if command=='whitelist remove TestUser': (m.ROOT/'whitelist.json').write_text('[]')
m.send_console=fake_console
m.mutate('difficulty','normal');assert m.props()['difficulty']=='normal' and commands[-1]=='difficulty normal'
m.mutate('max-players','7');assert m.props()['max-players']=='7' and (m.META/'settings-pending').exists()
m.make_state();assert 'settings_pending=1' in (m.RUN/'state').read_text()
m.PID.write_text('222');m.make_state();assert 'settings_pending=0' in (m.RUN/'state').read_text()
m.mc_status=lambda:{'players':{'online':2,'sample':[]}}
m.atomic(m.RUN/'players.json',json.dumps({'time':time.time(),'server_pid':222,'players':['Alex','Steve']}))
m.make_state();state=(m.RUN/'state').read_text();assert 'players=2' in state and 'player_0=Alex' in state and 'player_1=Steve' in state
for malformed in [None, [], {'players':None}, {'players':{'online':'2'}}, {'players':{'online':-1}}, {'players':{'online':1,'sample':{}}}]:
    m.mc_status=lambda value=malformed:value
    m.make_state();assert 'server=starting\n' in (m.RUN/'state').read_text()
for malformed in [None, [], {'players':None}, {'players':{'online':False}}, {'players':{'online':-1}}]:
    try: m.online_count(malformed);raise AssertionError('invalid player count accepted')
    except ValueError: pass
m.mc_status=lambda:{'players':{'online':1,'sample':[None,{'name':'Alex'}]}}
m.atomic(m.RUN/'players.json','[]')
m.make_state();state=(m.RUN/'state').read_text();assert 'server=online\n' in state and 'player_0=Alex\n' in state
# A malformed idle check must never stop the server, including on the second
# check after the temporary stop marker has been created.
m.mc_status=lambda:None
try: m.stop_for_snapshot(require_idle=True);raise AssertionError('invalid idle status accepted')
except m.AdminError: pass
assert not m.STOP.exists()
responses=iter([{'players':{'online':0}},None]);m.mc_status=lambda:next(responses)
try: m.stop_for_snapshot(require_idle=True);raise AssertionError('invalid second idle status accepted')
except ValueError: pass
assert not m.STOP.exists()
m.mc_status=lambda:{'players':{'online':0}}
m.mutate('whitelist-add','TestUser');assert m.whitelist_entries()==['TestUser']
m.mutate('whitelist-remove','TestUser');assert not m.whitelist_entries()
# An unreadable list is not proof that a requested removal succeeded.
real_entries=m.whitelist_entries; real_monotonic=m.time.monotonic; real_sleep=m.time.sleep
try:
    m.whitelist_entries=lambda:None
    clock=iter((0,1,16));m.time.monotonic=lambda:next(clock);m.time.sleep=lambda _:None
    try:m.mutate('whitelist-remove','TestUser');raise AssertionError('unreadable list confirmed removal')
    except m.AdminError as exc:assert 'not confirmed' in str(exc)
finally:
    m.whitelist_entries=real_entries;m.time.monotonic=real_monotonic;m.time.sleep=real_sleep
for command,value in [('whitelist-add','bad name'),('max-players','999'),('view-distance','1')]:
    try:m.mutate(command,value);raise AssertionError('invalid input accepted')
    except m.AdminError:pass
before=list(commands)
for value in ('ab','a'*17,'Alex\nstop','Alex;stop','-option','\u0410lex','Alex\x00'):
    for command in ('whitelist-add','whitelist-remove'):
        try:m.mutate(command,value);raise AssertionError('unsafe player name accepted')
        except m.AdminError:pass
assert commands==before
for value in ('-latest','1234567','abcdef0g','a'*65,'abcdef01\n','abcdef01/../../x'):
    try:m.restore(value);raise AssertionError('unsafe backup identifier accepted')
    except m.AdminError:pass
print('SETTINGS_WHITELIST_AND_INPUT_VALIDATION_PASSED')
