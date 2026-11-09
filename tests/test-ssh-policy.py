"""Validate the effective OpenSSH policy without starting a listener."""
from pathlib import Path
import subprocess

config = Path(__file__).resolve().parents[1] / 'runtime/etc/ssh/sshd_config_n20'
result = subprocess.run(['/usr/sbin/sshd','-T','-f',str(config)],check=True,capture_output=True,text=True)
values = {}
for line in result.stdout.splitlines():
    key, _, value = line.partition(' ')
    values.setdefault(key, []).append(value)
for key, expected in {
    'authenticationmethods':'publickey', 'allowusers':'root',
    'passwordauthentication':'no', 'kbdinteractiveauthentication':'no',
    'permitemptypasswords':'no', 'maxauthtries':'3',
    'logingracetime':'30', 'maxstartups':'10:30:60',
}.items():
    assert values.get(key) == [expected], (key, values.get(key))
assert values['pubkeyauthentication'] == ['yes']
print('SSH_EFFECTIVE_AUTHENTICATION_POLICY_PASSED')
