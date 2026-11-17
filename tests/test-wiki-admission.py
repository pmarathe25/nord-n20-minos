"""Requests are queued when the model cannot run, including map misses."""
from collections import deque
from concurrent.futures import Future
import json
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'runtime/usr/local/lib/n20'))
import wiki_chat as m

args=SimpleNamespace(console='unused',pid_file='unused')
requests=deque()
line='[01:37:23] [Server thread/INFO]: <Alex> @wiki Example question?'
with patch.object(m,'server_identity',return_value=(42,'start')), patch.object(m,'reply') as reply:
    with patch.object(m.time,'monotonic',return_value=100):
        for _ in range(2000):m.accept_chat(line,args,requests,None,0,False)
        assert len(requests)==2000 and reply.call_count==2000
    # Async map fallback uses the same admission function as chat.
    with patch.object(m.time,'monotonic',return_value=122):
        assert m.queue_request(args,requests,None,(42,'start'),'Alex','@map missing')
        for i in range(100):m.queue_request(args,requests,None,(42,'start'),'Player'+str(i),'question')
        assert len(requests)==2101
    with patch.object(m.time,'monotonic',return_value=500):
        assert m.start_queued(args,requests,None,{},None,0,lambda _:None,False) is None
        assert len(requests)==2101  # Unavailability does not expire accepted work.
        requests.clear()
        args._active_player='Alex'
        assert m.queue_request(args,requests,Future(),(42,'start'),'Alex','one')
    with patch.object(m.time,'monotonic',return_value=511):
        assert m.queue_request(args,requests,Future(),(42,'start'),'Alex','two')
    requests.clear()
    with patch.object(m.time,'monotonic',return_value=600):
        for i in range(100):m.queue_request(args,requests,None,(43,'new'),'Player'+str(i),'\U0001f600'*256)
        assert len(requests)==100
with tempfile.TemporaryDirectory(prefix='n20-handoff-wait-') as tmp:
    path=Path(tmp)/'handoff.json'
    path.write_text(json.dumps({'version':1,'queue':[[[42,'start'],'Alex','old question']],
                                'log':{'dev':1,'ino':2,'offset':3}}))
    os.utime(path,(0,0))
    queue,resume=m.restore_handoff(path)
    assert len(queue)==1 and queue[0][2]=='old question' and resume==((1,2),3) and not path.exists()
print('WIKI_ADMISSION_QUEUE_PASSED')
