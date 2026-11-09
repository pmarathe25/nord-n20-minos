import importlib.machinery
import tempfile
from pathlib import Path
m=importlib.machinery.SourceFileLoader('n20_admin_power',str(Path(__file__).resolve().parents[1] / 'runtime/usr/local/sbin/n20-admin')).load_module()
top=Path(tempfile.mkdtemp(prefix='n20-power-test-',dir='/tmp'))
m.ROOT=top/'server';m.ROOT.mkdir();m.RUN=top/'run';m.META=top/'meta';m.LEGACY_HOLD=m.ROOT/'.n20-restore-hold'; m.HOLD=m.META/'restore-hold';m.LEGACY_LOW=m.ROOT/'.n20-low-battery-hold'; m.LOW=m.META/'low-battery-hold';m.STOP=top/'stop';m.OP=m.RUN/'operation';m.LOCK=m.RUN/'operation.lock';m.PID=top/'pid';m.CACHE=m.META/'cache';m.ERR=m.META/'error'
m.directories();m.pid_running=lambda:True;m.send_console=lambda command:None
m.CHARGE_SWITCH=top/'charge-switch';m.CHARGE_SWITCH.write_text('1\n')
assert m.external_power('1','0')=='1' and m.external_power('0','1')=='1'
assert m.external_power('1','')=='1' and m.external_power('0','0')=='0'
assert m.external_power('0','')=='unknown' and m.external_power('','')=='unknown'
def switch():return m.CHARGE_SWITCH.read_text().strip()
m.apply_charge_limit(55,'1');assert switch()=='0'
m.apply_charge_limit(54,'1');assert switch()=='0'
m.apply_charge_limit(50,'1');assert switch()=='1'
m.apply_charge_limit(51,'1');assert switch()=='1'
m.apply_charge_limit(99,'1');assert switch()=='0'
m.apply_charge_limit(99,'0');assert switch()=='1'
m.apply_charge_limit(99,'1');m.apply_charge_limit(-1,'1');assert switch()=='1'
m.apply_charge_limit(99,'1');m.apply_charge_limit(101,'1');assert switch()=='1'
assert m.low_battery_sample('not charging',8)
assert m.low_battery_sample('discharging',0)
assert not m.low_battery_sample('charging',8)
assert not m.low_battery_sample('not charging',9)
assert not m.low_battery_sample('not charging',-1)
def fake_stop():m.STOP.touch();return True,False
m.stop_for_snapshot=fake_stop
m.low_battery_pause();assert m.LOW.exists() and m.STOP.exists()
m.low_battery_resume();assert not m.LOW.exists() and not m.STOP.exists()
m.pid_running=lambda:False;m.low_battery_pause();assert not m.LOW.exists()
m.LOW.touch();m.STOP.touch();m.HOLD.touch();m.low_battery_resume();assert m.LOW.exists() and m.STOP.exists()
m.HOLD.unlink();m.LEGACY_HOLD.touch();assert m.restore_held();m.low_battery_resume();assert m.LOW.exists() and m.STOP.exists()
m.LEGACY_HOLD.unlink();m.LOW.unlink();m.LEGACY_LOW.touch()
assert m.low_battery_held()
m.low_battery_resume();assert not m.low_battery_held() and not m.STOP.exists()
m.LEGACY_LOW.symlink_to(m.ROOT/'missing')
assert m.low_battery_held()
m.low_battery_resume();assert not m.low_battery_held()
print('LOW_BATTERY_PAUSE_RESUME_AND_RESTORE_GUARD_PASSED')
