"""Retain questions across unavailable lookups and deliver once ready."""
from collections import OrderedDict, deque
from concurrent.futures import Future
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch
import json
import multiprocessing
import os
import select
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'runtime/usr/local/lib/n20'))
import wiki_chat as m


class Worker:
    def __init__(self, result): self.result = result
    def submit(self, *_):
        future = Future()
        if isinstance(self.result, Exception): future.set_exception(self.result)
        else: future.set_result(self.result)
        return future


class RetryTests(unittest.TestCase):
    def test_failure_retries_once_and_does_not_write_history(self):
        args = SimpleNamespace(console='unused', pid_file='unused')
        queue = deque(m.QueuedRequest((42, 'start'), 'Player'+str(i), 'Question '+str(i)) for i in range(16))
        original = queue[0]
        record = Mock()
        with patch.object(m, 'server_identity', return_value=(42, 'start')), patch.object(m, 'reply') as send:
            with patch.object(m.time, 'monotonic', return_value=100):
                future, *_ = m.start_queued(args, queue, Worker(m.LookupDeferred()), {}, None, 0, lambda _: None)
                self.assertTrue(m.queue_request(args, queue, future, (42,'start'), 'NewPlayer', 'Extra'))
                self.assertTrue(m.queue_request(args, queue, future, (42,'start'), 'Player1', 'Extra'))
                m.finish_queued(args, queue, future, record)
                self.assertIs(queue[0], original)
                self.assertEqual(len(queue), 18)
                record.assert_not_called()
            notices = [call for call in send.call_args_list if 'queued until' in call.args[2][0]['text']]
            self.assertEqual(len(notices), 1)
            self.assertFalse(notices[0].kwargs.get('broadcast', False))
            with patch.object(m.time, 'monotonic', return_value=129):
                self.assertIsNone(m.start_queued(args, queue, None, {}, None, 0, lambda _: None))
            with patch.object(m.time, 'monotonic', return_value=130):
                future, *_ = m.start_queued(args, queue, Worker(m.LookupDeferred()), {}, None, 0, lambda _: None)
                m.finish_queued(args, queue, future, record)
            self.assertEqual(len([c for c in send.call_args_list if 'queued until' in c.args[2][0]['text']]), 1)
            answer = [{'text': '[Wiki] Answer'}]
            with patch.object(m.time, 'monotonic', return_value=160):
                future, *_ = m.start_queued(args, queue, Worker(answer), {}, None, 0, lambda _: None)
                m.finish_queued(args, queue, future, record)
            record.assert_called_once_with('Player0', 'Question 0', answer)
            self.assertEqual(len(queue), 17)

    def test_active_question_retries_after_large_questions_are_queued(self):
        args = SimpleNamespace(console='unused', pid_file='unused')
        queue = deque(m.QueuedRequest((42,'start'), 'Player'+str(i), '\U0001f600'*256) for i in range(8))
        with patch.object(m, 'server_identity', return_value=(42,'start')), patch.object(m, 'reply'):
            future, *_ = m.start_queued(args, queue, Worker(m.LookupDeferred()), {}, None, 0, lambda _: None)
            self.assertTrue(m.queue_request(args, queue, future, (42,'start'), 'NewPlayer', 'x'))
            m.finish_queued(args, queue, future, Mock())
            self.assertEqual(sum(len(item[2].encode()) for item in queue), 8193)

    def test_wait_does_not_expire_and_old_server_is_discarded(self):
        args = SimpleNamespace(console='unused', pid_file='unused')
        queue = deque([m.QueuedRequest((42,'start'), 'Alex', 'Question')])
        with patch.object(m.time, 'monotonic', return_value=86400), patch.object(m, 'server_identity', return_value=(42,'start')):
            self.assertIsNone(m.start_queued(args, queue, None, {}, None, 0, lambda _: None, False))
            self.assertEqual(len(queue), 1)
            future, *_ = m.start_queued(args, queue, Worker(m.LookupDeferred()), {}, None, 0, lambda _: None)
        with patch.object(m, 'server_identity', return_value=(43,'new')), patch.object(m, 'reply') as send:
            record = Mock(); m.finish_queued(args, queue, future, record)
            self.assertFalse(queue); send.assert_not_called(); record.assert_not_called()

    def test_resource_retrieval_and_model_failures_defer_but_unknown_finishes(self):
        args = SimpleNamespace(socket='unused')
        with patch.object(m, 'resources_available', return_value=False):
            with self.assertRaises(m.LookupDeferred): m.answer_question(args, 'Question', OrderedDict())
        with patch.object(m, 'resources_available', return_value=True), patch.object(m, 'find_passages', side_effect=TimeoutError()):
            with self.assertRaises(m.LookupDeferred): m.answer_question(args, 'Question', OrderedDict())
        passage = {'title':'Example', 'text':'Example evidence'}
        with patch.object(m, 'resources_available', return_value=True), patch.object(m, 'find_passages', return_value=[passage]), \
             patch.object(m, 'exact_answer', return_value=None), patch.object(m, 'generate_for_request', side_effect=TimeoutError()):
            with self.assertRaises(m.LookupDeferred): m.answer_question(args, 'Question', OrderedDict())
        with patch.object(m, 'resources_available', return_value=True), patch.object(m, 'find_passages', return_value=[]):
            self.assertEqual(m.answer_question(args, 'Question', OrderedDict()), m.render(m.UNKNOWN, []))

    @unittest.skipUnless(hasattr(os, 'fork'), 'requires Unix FIFO and fork')
    def test_bridge_outage_keeps_question_then_answers_and_records_once(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp); log=root/'log'; log.write_text(''); fifo=root/'console'; os.mkfifo(fifo,0o600)
            fd=os.open(fifo,os.O_RDWR|os.O_NONBLOCK)
            args=SimpleNamespace(log=log,console=fifo,pid_file=root/'pid',socket=root/'socket',status_file=root/'status',history_file=root/'history')
            ready=root/'ready'; restored=root/'restored'; attempts=root/'attempts'; original_tail=m.LogTail
            class Tail(original_tail):
                def read(self):
                    lines=super().read();ready.touch();return lines
            def lookup(*_):
                with attempts.open('a') as out:out.write('attempt\n')
                if not restored.exists():raise m.LookupDeferred()
                return [{'text':'[Wiki] Recovered answer'}]
            def run():
                with patch.object(m,'LogTail',Tail),patch.object(m,'server_identity',return_value=(42,'start')), \
                     patch.object(m,'answer_question',lookup),patch.object(m,'resources_available',return_value=True), \
                     patch.object(m,'model_health',return_value='unavailable'),patch.object(m,'REQUEST_INTERVAL_SECONDS',.1):
                    m.run(args)
            child=multiprocessing.get_context('fork').Process(target=run);child.start()
            messages=[];buffer=b''
            def until(condition):
                nonlocal buffer
                deadline=time.monotonic()+5
                while time.monotonic()<deadline:
                    if select.select([fd],[],[],.05)[0]:
                        buffer+=os.read(fd,65536);lines=buffer.split(b'\n');buffer=lines.pop();messages.extend(lines)
                    if condition():return
                self.fail('bridge did not reach expected state')
            try:
                until(ready.exists)
                with log.open('a') as out:out.write('[01:37:23] [Server thread/INFO]: <Alex> @wiki Question?\n')
                until(lambda: attempts.exists() and len(attempts.read_text().splitlines())>=3)
                self.assertFalse(args.history_file.exists())
                self.assertEqual(sum(b'queued until' in line for line in messages),1)
                self.assertFalse(any(b'temporarily unavailable' in line for line in messages))
                restored.touch()
                until(lambda: args.history_file.exists() and b'Recovered answer' in b'\n'.join(messages))
                self.assertEqual(len(json.loads(args.history_file.read_text())),1)
                self.assertEqual(sum(b'Recovered answer' in line for line in messages),1)
            finally:
                child.terminate();child.join(5);os.close(fd)


if __name__=='__main__':unittest.main()
