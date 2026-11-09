#!/usr/bin/env python3
"""Experimental read-only signed-time probe; never changes the system clock."""
import concurrent.futures
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'third_party' / 'pyroughtime'))

# Public keys published by each operator; rotation requires an explicit update.
SERVERS = (
    ('time.txryan.com', 2002, 'iBVjxg/1j7y1+kQUTBYdTabxCppesU/07D4PMDJk2WA='),
    ('rough.time.nl', 5319, 'v2CievhgKsxzlWPwIkYFUXeA51Akhkv5uhJCj1/kbiY='),
)

def query(server):
    from pyroughtime import RoughtimeClient
    started = time.monotonic()
    result = RoughtimeClient().query(*server, timeout=3)
    received = time.monotonic()
    if result.ver() != '1' or '1' not in result.vers().split():
        raise ValueError('Unsupported or inconsistent protocol version')
    if received - started > 6 or not 1 <= result.radi() <= 10:
        raise ValueError('Time response too slow or uncertain')
    # A response's signed timestamp was generated during the query. Widen
    # its interval by the full round trip before projecting it forward.
    return result.midp(), result.radi() + (received - started), received

def agreement(samples, now):
    if len(samples) != len(SERVERS):
        raise ValueError('All independent servers must respond')
    intervals = []
    for midpoint, radius, received in samples:
        elapsed = now - received
        if not 0 <= elapsed <= 10 or not 0 <= radius <= 16:
            raise ValueError('Stale or uncertain response')
        # Allow one second for integer timestamp rounding and query latency.
        center = midpoint + elapsed
        intervals.append((center - radius - 1, center + radius + 1))
    lower, upper = max(x[0] for x in intervals), min(x[1] for x in intervals)
    if lower > upper or lower < 1704067200 or upper > 4102444800:
        raise ValueError('Servers disagree or time outside accepted range')
    return int((lower + upper) / 2)

def main():
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(SERVERS)) as pool:
        samples = list(pool.map(query, SERVERS))
    print(agreement(samples, time.monotonic()))

if __name__ == '__main__':
    try:
        main()
    except Exception as exc:
        print('Signed-time probe failed: ' + str(exc), file=sys.stderr)
        sys.exit(1)
