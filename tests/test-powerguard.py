import importlib.machinery
import tempfile
from pathlib import Path
m=importlib.machinery.SourceFileLoader('n20_admin_power',str(Path(__file__).resolve().parents[1] / 'runtime/usr/local/sbin/n20-admin')).load_module()
top=Path(tempfile.mkdtemp(prefix='n20-power-test-',dir='/tmp'))
m.ROOT=top/'server';m.ROOT.mkdir();m.RUN=top/'run';m.META=top/'meta';m.HOLD=m.ROOT/'.restore';m.LOW=m.ROOT/'.low';m.STOP=top/'stop';m.OP=m.RUN/'operation';m.LOCK=m.RUN/'operation.lock';m.PID=top/'pid';m.CACHE=m.META/'cache';m.ERR=m.META/'error'
m.directories();m.pid_running=lambda:True;m.send_console=lambda command:None
def fake_stop():m.STOP.touch();return True,False
m.stop_for_snapshot=fake_stop
m.low_battery_pause();assert m.LOW.exists() and m.STOP.exists()
m.low_battery_resume();assert not m.LOW.exists() and not m.STOP.exists()
m.pid_running=lambda:False;m.low_battery_pause();assert not m.LOW.exists()
m.LOW.touch();m.STOP.touch();m.HOLD.touch();m.low_battery_resume();assert m.LOW.exists() and m.STOP.exists()
print('LOW_BATTERY_PAUSE_RESUME_AND_RESTORE_GUARD_PASSED')
