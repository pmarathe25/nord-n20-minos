import datetime as dt
import importlib.machinery
import json
import subprocess
import tempfile
from pathlib import Path

m = importlib.machinery.SourceFileLoader(
    'n20_retention', str(Path(__file__).resolve().parents[1] / 'runtime/usr/local/sbin/n20-admin')
).load_module()

with tempfile.TemporaryDirectory(prefix='n20-retention-') as tmp:
    root = Path(tmp)
    repo, password, world = root / 'repo', root / 'password', root / 'world'
    password.write_text('isolated test password\n')
    world.mkdir()
    (world / 'level.dat').write_text('isolated data')
    base = ['restic', '-r', str(repo), '--password-file', str(password), '--no-cache']

    def restic(args, timeout=1800):
        return subprocess.run(base + args, check=True, capture_output=True, text=True, timeout=timeout).stdout

    restic(['init'])
    # Older than eight weeks, with multiple auto snapshots in one week.
    start = dt.datetime(2024, 1, 1, 12, 0)
    for week in range(12):
        for day in (0, 1):
            date = (start + dt.timedelta(weeks=week, days=day)).strftime('%Y-%m-%d %H:%M:%S')
            restic(['backup', '--tag', 'auto', '--time', date, str(world)])
    # Exceed both the old manual limit and the pre-restore limit.
    for i in range(30):
        date = (start + dt.timedelta(hours=i)).strftime('%Y-%m-%d %H:%M:%S')
        restic(['backup', '--tag', 'manual', '--time', date, str(world)])
    for i in range(7):
        date = (start + dt.timedelta(hours=i)).strftime('%Y-%m-%d %H:%M:%S')
        restic(['backup', '--tag', 'pre-restore', '--time', date, str(world)])
    # Enough recent autos to exercise the keep-last and daily policies.
    for day in range(15):
        for hour in (0, 1):
            date = (start + dt.timedelta(weeks=20, days=day, hours=hour)).strftime('%Y-%m-%d %H:%M:%S')
            restic(['backup', '--tag', 'auto', '--time', date, str(world)])

    before = json.loads(restic(['snapshots', '--json']))
    m.run_restic = restic
    m.set_operation = lambda message: None
    m.refresh_cache = lambda: None
    m.PRUNE = root / 'last-prune'
    m.retention(allow_prune=False)
    after = json.loads(restic(['snapshots', '--json']))
    ids = {item['id'] for item in after}
    manual = [item for item in before if 'manual' in item['tags']]
    assert len(manual) == 30 and all(item['id'] in ids for item in manual)
    assert sum('pre-restore' in item['tags'] for item in after) == 5
    autos = [item for item in before if 'auto' in item['tags']]
    for week in range(12):
        latest = (start + dt.timedelta(weeks=week, days=1)).date().isoformat()
        assert any(item['id'] in ids and item['time'].startswith(latest) for item in autos)
    assert sum('auto' in item['tags'] for item in after) < len(autos)
    assert all(item['id'] in ids for item in sorted(autos, key=lambda x: x['time'])[-24:])
    for day in range(1, 15):
        date = (start + dt.timedelta(weeks=20, days=day, hours=1)).date().isoformat()
        assert any(item['id'] in ids and item['time'].startswith(date) for item in autos)

print('BACKUP_RETENTION_PASSED')
