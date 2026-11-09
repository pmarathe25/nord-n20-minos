#!/usr/bin/env python3
"""Prepare pinned, disposable Fabric benchmark assets; never deploy a server."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import pwd
import subprocess
import urllib.request


ROOT = Path('/tmp/n20-tuning-assets')
MODS = {
    'lithium': ('fWv5bnZk', '2ca92fb530135d2baddf0ba4cc4f93976234876a38c4aa02b487b2552400f005afcfbeebf5373fb4396fe2020537a42d7597b8da5a52b904355ef51d5fef07ad'),
    'c2me': ('aOP6eqZA', '5db513d05f909d7b09d035b40f0d31945ec8911f4d7b75fa258bb6e6338761b8847c9d1804d40b647e1781cfc143ff7dd0e0a4389f19ec46e031be7bbb7b5052'),
    'dh': ('TGgEbP9A', '0b90b1b08d99974bfc3c8e86b2ceb252e2f91715ed66b925004c28335fb44e1bddbbb6b8043e4d4d53c87ed247789cd1788b1ba4e3f4394efe154e0c59cff236'),
}
INSTALLER = 'https://maven.fabricmc.net/net/fabricmc/fabric-installer/1.1.2/fabric-installer-1.1.2.jar'
INSTALLER_SHA256 = '61e035bf7bf70153e127440ce34de47c9036f0a2d0c65d1529454bd35ceefe4f'


def download(url):
    with urllib.request.urlopen(url, timeout=60) as response:
        data = response.read(64 * 1024 * 1024 + 1)
    if len(data) > 64 * 1024 * 1024:
        raise RuntimeError('download exceeds asset size limit')
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepare', action='store_true', required=True)
    parser.parse_args()
    if os.geteuid() != 0:
        parser.error('root required to prepare root-controlled test assets')
    # Refuse existing directories and symlinks; never overwrite an installation.
    ROOT.mkdir(mode=0o755)
    ROOT.chmod(0o755)
    manifest = {}
    for name, (version_id, expected) in MODS.items():
        version = json.loads(download('https://api.modrinth.com/v2/version/' + version_id))
        asset = next(f for f in version['files'] if f['primary'])
        if not asset['url'].startswith('https://cdn.modrinth.com/'):
            raise RuntimeError('unexpected mod download origin')
        data = download(asset['url'])
        if hashlib.sha512(data).hexdigest() != expected:
            raise RuntimeError('mod hash mismatch: ' + name)
        path = ROOT / (name + '.jar')
        path.write_bytes(data)
        path.chmod(0o644)
        manifest[name] = dict(version=version['version_number'], type=version['version_type'],
                              url=asset['url'], sha512=expected)
    installer = download(INSTALLER)
    if hashlib.sha256(installer).hexdigest() != INSTALLER_SHA256:
        raise RuntimeError('installer hash mismatch')
    (ROOT / 'installer.jar').write_bytes(installer)
    (ROOT / 'installer.jar').chmod(0o644)
    manifest['fabric'] = dict(loader='0.20.1', installer='1.1.2', installer_sha256=INSTALLER_SHA256)
    (ROOT / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    prepared = ROOT / 'fabric'
    prepared.mkdir(mode=0o755)
    user = pwd.getpwnam('minecraft')
    os.chown(prepared, user.pw_uid, user.pw_gid)
    try:
        subprocess.run(['/usr/bin/setpriv', '--no-new-privs', '--bounding-set=-all',
                        '--reuid=minecraft', '--regid=minecraft', '--clear-groups',
                        '/opt/java/bin/java', '-jar', str(ROOT / 'installer.jar'),
                        'server', '-mcversion', '26.3', '-loader', '0.20.1', '-dir', str(prepared)],
                       cwd=prepared, check=True, timeout=180)
    finally:
        for path in prepared.rglob('*'):
            if path.is_symlink():
                raise RuntimeError('unexpected symlink in prepared installer output')
            os.chown(path, 0, 0)
            path.chmod(0o755 if path.is_dir() else 0o644)
        os.chown(prepared, 0, 0)
        prepared.chmod(0o755)
    (prepared / 'server.jar').symlink_to('/opt/n20-minecraft/current/server.jar')
    print('Prepared benchmark assets in ' + str(ROOT))


if __name__ == '__main__':
    main()
