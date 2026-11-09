#!/usr/bin/env python3
"""Test time-source policy with fake commands; never set the system clock."""
import os
from pathlib import Path
import subprocess
import tempfile

SCRIPT = Path(__file__).resolve().parents[1] / 'runtime/usr/local/sbin/n20-timekeeper'
CONFIG_DIR = Path(__file__).resolve().parents[1] / 'runtime/etc'

FAKE_BUSYBOX = r'''#!/bin/sh
cmd=$1; shift
case "$cmd" in
  date)
    if [ "$2" = +%s ]; then cat "$FAKE_NOW"; exit 0; fi
    if [ "$2" = -s ]; then printf '%s\n' "${3#@}" > "$FAKE_NOW"; echo 'date-set' >> "$TRACE"; exit 0; fi
    exit 1 ;;
  ip)
    if [ "$1" = route ]; then echo "destination dev ${USB_ROUTE:-wlan0}"; exit 0; fi
    if [ "$1" = addr ] && [ "${WIFI_UP:-0}" = 1 ]; then echo 'inet 192.0.2.1/24'; exit 0; fi
    exit 1 ;;
  wget) echo wget >> "$TRACE"; printf '%s\n' "${USB_REPLY:-}"; exit 0 ;;
  mktemp) exec /usr/bin/mktemp "$@" ;;
  sync) exec /usr/bin/sync "$@" ;;
  timeout) shift; exec "$@" ;;
  *) echo "unexpected-$cmd" >> "$TRACE"; exit 1 ;;
esac
'''
FAKE_CHRONY = r'''#!/bin/sh
printf 'chrony %s\n' "$*" >> "$TRACE"
if [ "${CHRONY_STATUS:-0}" = 0 ]; then printf '%s\n' "${CHRONY_TIME:-1800000000}" > "$FAKE_NOW"; fi
exit "${CHRONY_STATUS:-0}"
'''


def main():
    normal_config = (CONFIG_DIR / 'n20-chrony.conf').read_text().splitlines()
    assert any(line.startswith('server ') and ' nts ' in f' {line} ' for line in normal_config)
    assert 'port 0' in normal_config and 'cmdport 0' in normal_config
    assert 'nosystemcert' in normal_config
    assert sum(line.startswith('ntstrustedcerts ') for line in normal_config) == 1
    assert not any(line.strip().startswith('nocerttimecheck ') for line in normal_config)
    with tempfile.TemporaryDirectory(prefix='n20-time-test-') as temp:
        root = Path(temp)
        busybox, chrony = root / 'busybox', root / 'chronyd'
        busybox.write_text(FAKE_BUSYBOX); chrony.write_text(FAKE_CHRONY)
        busybox.chmod(0o755); chrony.chmod(0o755)
        wlan = root / 'wlan'; wlan.mkdir()
        now, trace, floor = root / 'now', root / 'trace', root / 'trusted-time'
        normal = root / 'normal.conf'
        normal.touch()

        def run(epoch, *, wifi, route='wlan0', reply='', chrony_status=0, chrony_time='1800000000'):
            now.write_text(str(epoch)); trace.write_text('')
            env = os.environ.copy()
            env.update(N20_TEST_BUSYBOX=str(busybox), N20_TEST_CHRONYD=str(chrony),
                       N20_TEST_NORMAL=str(normal),
                       N20_TEST_WLAN=str(wlan), N20_TEST_FLOOR=str(floor), FAKE_NOW=str(now), TRACE=str(trace),
                       WIFI_UP=str(int(wifi)), USB_ROUTE=route, USB_REPLY=reply,
                       CHRONY_STATUS=str(chrony_status), CHRONY_TIME=chrony_time)
            result = subprocess.run(['/bin/sh', str(SCRIPT), '--test-once'], env=env,
                                    capture_output=True, text=True, timeout=10)
            return result.returncode, trace.read_text().splitlines(), now.read_text().strip()

        code, calls, _ = run(1800000000, wifi=True, route='usb0', reply='1900000000')
        assert code == 0 and any(str(normal) in c for c in calls) and 'wget' not in calls and 'date-set' not in calls
        code, calls, current = run(100, wifi=False, route='usb0', reply='1800000000')
        assert code == 0 and current == '1800000000' and calls.count('date-set') == 1
        code, calls, current = run(100, wifi=True, route='usb0', reply='1800000000', chrony_status=1)
        assert code == 0 and current == '1800000000' and calls.count('date-set') == 1 and any(str(normal) in c for c in calls)
        code, calls, _ = run(100, wifi=True)
        assert code == 0 and any(str(normal) in c for c in calls) and 'wget' not in calls
        code, calls, current = run(100, wifi=True, chrony_status=1)
        assert code != 0 and current == '100' and 'date-set' not in calls and any(str(normal) in c for c in calls)
        code, calls, _ = run(1800000000, wifi=True, route='usb0', reply='1900000000', chrony_status=1)
        assert code != 0 and 'wget' not in calls and 'date-set' not in calls
        code, calls, _ = run(100, wifi=False, route='wlan0', reply='1800000000')
        assert code != 0 and 'wget' not in calls and 'date-set' not in calls
        code, calls, _ = run(4102444801, wifi=True)
        assert code == 0 and any(str(normal) in c for c in calls)
        floor.write_text('1800000000\n')
        code, calls, _ = run(1800000000, wifi=True, chrony_time='1799999900')
        assert code == 0 and floor.read_text().strip() == '1800000000'
        code, calls, current = run(1800000000, wifi=True, chrony_time='1705000000')
        assert code != 0 and current == '1800000000' and calls.count('date-set') == 1
        code, calls, current = run(1800000000, wifi=True, chrony_time='4102444801')
        assert code != 0 and current == '1800000000' and calls.count('date-set') == 1
        code, calls, current = run(100, wifi=True, chrony_time='1705000000')
        assert code != 0 and current == '100' and calls.count('date-set') == 1
        code, calls, current = run(100, wifi=True, chrony_time='4102444801')
        assert code != 0 and current == '100' and calls.count('date-set') == 1
        code, calls, _ = run(1705000000, wifi=True)
        assert code == 0 and any(str(normal) in c for c in calls)
        code, calls, _ = run(100, wifi=False, route='usb0', reply='1705000000')
        assert code != 0 and 'date-set' not in calls
        assert not any('ntpd' in c for c in calls)
    print('AUTHENTICATED_TIME_POLICY_PASSED')


if __name__ == '__main__':
    main()
