import importlib.machinery,json,os,tempfile,time
from pathlib import Path
m=importlib.machinery.SourceFileLoader('n20_admin_ui',str(Path(__file__).resolve().parents[1] / 'runtime/usr/local/sbin/n20-admin')).load_module()
top=Path(tempfile.mkdtemp(prefix='n20-settings-validation-',dir='/tmp'))
m.ROOT=top/'server';m.ROOT.mkdir();m.WORLD=m.ROOT/'world';m.RUN=top/'run';m.META=top/'meta';m.PID=top/'pid';m.STOP=top/'stop';m.HOLD=m.ROOT/'.restore';m.LOW=m.ROOT/'.low';m.OP=m.RUN/'operation';m.LOCK=m.RUN/'operation.lock';m.ERR=m.META/'error';m.CACHE=m.META/'cache';m.directories()
(m.ROOT/'server.properties').write_text('difficulty=easy\nmax-players=5\nview-distance=6\nsimulation-distance=4\nwhite-list=false\n')
(m.ROOT/'whitelist.json').write_text('[]');m.PID.write_text('111');m.pid_running=lambda:True;m.mc_status=lambda:{'players':{'online':0}}
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
m.mutate('whitelist-add','TestUser');assert m.whitelist_entries()==['TestUser']
m.mutate('whitelist-remove','TestUser');assert not m.whitelist_entries()
for command,value in [('whitelist-add','bad name'),('max-players','999'),('view-distance','1')]:
    try:m.mutate(command,value);raise AssertionError('invalid input accepted')
    except m.AdminError:pass
print('SETTINGS_WHITELIST_AND_INPUT_VALIDATION_PASSED')
