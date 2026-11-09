#!/usr/bin/env python3
"""Log non-secret battery and load counters without changing device state."""
import argparse
import csv
import json
from pathlib import Path
import time

p=argparse.ArgumentParser()
p.add_argument('--duration',type=int,default=600)
p.add_argument('--interval',type=float,default=10)
args=p.parse_args()
if args.duration<=0 or args.interval<=0: p.error('duration and interval must be positive')
battery=Path('/sys/class/power_supply/battery')
fields=('status','capacity','voltage_now','current_now','charge_counter','temp')
def read(path):
    try:return path.read_text().strip()
    except OSError:return ''
def players():
    try:
        data=json.loads(read(Path('/run/n20/players.json')))
        age=time.time()-float(data.get('time',0))
        names=data.get('players')
        if -5<=age<=90 and isinstance(names,list) and str(data.get('server_pid'))==read(Path('/run/minecraft.pid')):
            return len(names)
    except (ValueError,TypeError,AttributeError):pass
    # The event watcher can be quiet for long periods. Use a fresh dashboard
    # status as a fallback, without probing the server and affecting its load.
    try:
        state=dict(line.split('=',1) for line in read(Path('/run/n20/state')).splitlines() if '=' in line)
        age=time.time()-float(state.get('updated_unix',0));count=int(state.get('players',''))
        if state.get('server')=='online' and -5<=age<=90 and 0<=count<=10000:return count
    except (ValueError,TypeError):pass
    return ''
def cpu():
    try:
        stat=read(Path('/proc/stat')).splitlines()[0].split()[1:]
        values=list(map(int,stat));return sum(values),values[3]+values[4]
    except (IndexError,ValueError):return '',''
writer=csv.writer(__import__('sys').stdout,lineterminator='\n')
writer.writerow(('elapsed_s','unix_s',*fields,'brightness','players','cpu_total','cpu_idle'))
start=time.monotonic();deadline=start+args.duration
while True:
    now=time.monotonic();total,idle=cpu()
    writer.writerow((round(now-start,2),round(time.time(),2),*(read(battery/f) for f in fields),
                     read(Path('/sys/class/backlight/panel0-backlight/brightness')),players(),total,idle))
    __import__('sys').stdout.flush()
    if now>=deadline:break
    time.sleep(min(args.interval,max(0,deadline-time.monotonic())))
