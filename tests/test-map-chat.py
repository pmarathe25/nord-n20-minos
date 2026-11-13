"""Exercise marker commands without touching a real server or world."""
import json
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "runtime/usr/local/lib/n20"))
import wiki_chat as m

PREFIX = "[12:34:56] [Server thread/INFO]: "


class MarkerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.args = SimpleNamespace(pins_file=Path(self.tmp.name) / "pins.json", pid_file=Path(self.tmp.name) / "pid", console="unused", socket="unused")
        self.identity = patch.object(m, "server_identity", return_value=(42, "1"))
        self.identity.start()
        self.addCleanup(self.identity.stop)
        self.replies = []
        self.reply = patch.object(m, "reply", side_effect=lambda path, player, components, **kw: self.replies.append((player, components[0]["text"], kw)))
        self.reply.start()
        self.addCleanup(self.reply.stop)
        self.query = patch.object(m, "console_query")
        self.queries = self.query.start()
        self.addCleanup(self.query.stop)
        self.service = m.MarkerQueue(self.args)

    def capture(self, player, pos, dimension):
        self.service.handle(PREFIX + f'System chat: {player} has the following entity data: {pos}')
        self.service.handle(PREFIX + f'System chat: {player} has the following entity data: "{dimension}"')
        token = self.service.pending["token"]
        self.service.handle(PREFIX + f'System chat: Found no elements matching {token}')
        self.service.advance()

    def test_concurrent_pins_and_map_are_ordered_and_broadcast(self):
        self.service.handle(PREFIX + '<Alex> @pin small village with a well')
        self.service.handle(PREFIX + '<Steve> @pin mountain house')
        self.service.handle(PREFIX + '<Alex> @map')
        self.service.advance()
        self.assertEqual(self.queries.call_count, 1)
        self.capture('Alex', '[12.75d, 64.0d, -3.2d]', 'minecraft:overworld')
        self.assertEqual(self.queries.call_count, 2)
        self.assertEqual(self.service.pending['player'], 'Steve')
        self.capture('Steve', '[5.0d, 70.0d, 7.0d]', 'minecraft:the_nether')
        markers = m.load_markers(self.args.pins_file)
        self.assertEqual([item['note'] for item in markers], ['small village with a well', 'mountain house'])
        self.assertTrue(any('12, 64, -4' in text for _, text, _ in self.replies))
        self.assertTrue(any('mountain house' in text for _, text, _ in self.replies))
        self.assertTrue(all(kwargs.get('broadcast') is True for _, _, kwargs in self.replies))
        self.assertFalse(any('no saved locations' in text for _, text, _ in self.replies))
        with patch.object(m, 'resources_available', return_value=True), patch.object(m, 'generate', return_value='1') as generate:
            self.assertIn('small village', m.find_marker(self.args, 'the village')[0]['text'])
            self.assertIn('small village', json.dumps(generate.call_args.args[4]))

    def test_timeout_drains_stale_responses_before_next_pin(self):
        self.service.handle(PREFIX + '<Alex> @pin first')
        self.service.handle(PREFIX + '<Alex> @pin second')
        self.service.advance()
        old_token = self.service.pending['token']
        self.service.pending['deadline'] = 0
        with patch.object(m, 'console_write') as write:
            self.service.advance()
        self.assertTrue(self.service.pending['resync'])
        new_token = self.service.pending['token']
        self.assertIn(new_token, write.call_args.args[1])
        self.service.handle(PREFIX + 'System chat: Alex has the following entity data: [1.0d, 2.0d, 3.0d]')
        self.service.handle(PREFIX + f'System chat: Found no elements matching {old_token}')
        self.service.advance()
        self.assertEqual(self.queries.call_count, 1)
        self.service.handle(PREFIX + f'System chat: Found no elements matching {new_token}')
        self.service.advance()
        self.assertEqual(self.queries.call_count, 2)
        self.capture('Alex', '[5.0d, 6.0d, 7.0d]', 'minecraft:overworld')
        self.assertEqual([p['note'] for p in m.load_markers(self.args.pins_file)], ['second'])

    def test_unreadable_file_is_not_overwritten(self):
        self.args.pins_file.write_text('invalid')
        self.service.handle(PREFIX + '<Alex> @pin test')
        self.service.advance()
        self.capture('Alex', '[1.0d, 2.0d, 3.0d]', 'minecraft:the_nether')
        self.assertEqual(self.args.pins_file.read_text(), 'invalid')
        self.assertTrue(any('Could not save' in text for _, text, _ in self.replies))

    def test_pin_that_saves_hit_and_waits_before_map(self):
        with patch.object(m, 'console_raycast') as cast:
            self.service.handle(PREFIX + '<Alex> @pin_that village gate')
            self.service.handle(PREFIX + '<Steve> @map')
            self.service.advance()
            cast.assert_called_once()
            token = self.service.pending['token']
            for value in ('[1.0d, 64.0d, 2.0d]', '[30.0f, -12.0f]', '"minecraft:overworld"'):
                self.service.handle(PREFIX + f'System chat: Alex has the following entity data: {value}')
            self.service.handle(PREFIX + f'System chat: {token} has the following entity data: [4.25d, 65.0d, 7.75d]')
            for value in ('[1.0d, 64.0d, 2.0d]', '[30.0f, -12.0f]', '"minecraft:overworld"'):
                self.service.handle(PREFIX + f'System chat: Alex has the following entity data: {value}')
            self.service.handle(PREFIX + f'System chat: Found no elements matching {token}')
            self.service.advance()
        self.assertEqual(m.load_markers(self.args.pins_file)[0]['pos'], [4.25, 65.0, 7.75])
        self.assertTrue(any('village gate: 4, 65, 7' in text for _, text, _ in self.replies))
        self.assertTrue(all(kw.get('broadcast') is True for _, _, kw in self.replies))

    def test_pin_that_rejects_changed_view_and_missing_target(self):
        with patch.object(m, 'console_raycast'):
            for changed in (True, False):
                self.service.handle(PREFIX + '<Alex> @pin_that test')
                self.service.advance()
                token = self.service.pending['token']
                for rotation in ('[0.0f, 0.0f]', '[1.0f, 0.0f]' if changed else '[0.0f, 0.0f]'):
                    for value in ('[1.0d, 64.0d, 2.0d]', rotation, '"minecraft:overworld"'):
                        self.service.handle(PREFIX + f'System chat: Alex has the following entity data: {value}')
                self.service.handle(PREFIX + f'System chat: Found no elements matching {token}')
                self.service.advance()
        self.assertFalse(self.args.pins_file.exists())
        self.assertTrue(any('view changed' in text for _, text, _ in self.replies))
        self.assertTrue(any('No block was found' in text for _, text, _ in self.replies))

    @unittest.skipUnless(hasattr(os, 'mkfifo'), 'requires a POSIX FIFO')
    def test_console_query_writes_distinct_read_only_barrier(self):
        fifo = Path(self.tmp.name) / 'console'
        os.mkfifo(fifo)
        reader = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
        self.addCleanup(os.close, reader)
        token = 'n20_pin_' + 'a' * 32
        self.query.stop()
        m.console_query(fifo, 'Alex', token)
        command = os.read(reader, 4096).decode('ascii')
        self.assertEqual(command.splitlines(), ['data get entity Alex Pos',
                                                'data get entity Alex Dimension',
                                                f'data get storage n20:{token} {token}'])


if __name__ == '__main__':
    unittest.main()
