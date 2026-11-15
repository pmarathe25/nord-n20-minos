"""Exercise model demand lifetimes, owner validation and direct answers."""
from collections import OrderedDict
import os
from pathlib import Path
import pwd
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime/usr/local/lib/n20'))
import wiki_chat as m
import wiki_demand


class DemandTests(unittest.TestCase):
    def test_fast_wiki_answer_never_requests_a_model(self):
        with tempfile.TemporaryDirectory() as tmp:
            marker = Path(tmp) / 'demand'
            args = SimpleNamespace(socket='unused', model_demand_file=marker)
            passage = {'title': 'Lightning Rod', 'section': 'Crafting', 'anchor': 'Crafting', 'revision': 1,
                       'text': 'Name: Lightning Rod; Ingredients: Copper Ingot; Crafting recipe: 3 × Copper Ingot'}
            with patch.object(m, 'resources_available', return_value=True), \
                 patch.object(m, 'find_passages', return_value=[passage]), \
                 patch.object(m, 'generate') as generate:
                result = m.answer_question(args, 'How many copper ingots make a lightning rod?', OrderedDict())
            self.assertIn('3 × copper ingot', result[0]['text'])
            generate.assert_not_called()
            self.assertFalse(marker.exists())

    def test_model_demand_lives_until_generation_completes(self):
        with tempfile.TemporaryDirectory() as tmp:
            marker = Path(tmp) / 'demand'
            args = SimpleNamespace(socket='unused', model_demand_file=marker)
            def infer(*_):
                self.assertEqual(marker.read_text().split()[0], str(os.getpid()))
                self.assertEqual(marker.stat().st_mode & 0o777, 0o600)
                return 'answer'
            with patch.object(m, 'model_health', side_effect=['unavailable', 'ready']), \
                 patch.object(m.time, 'sleep'), patch.object(m, 'generate', side_effect=infer):
                self.assertEqual(m.generate_for_request(args, 'question', None), 'answer')
            self.assertFalse(marker.exists())

    def test_failed_generation_and_startup_timeout_remove_demand(self):
        with tempfile.TemporaryDirectory() as tmp:
            marker = Path(tmp) / 'demand'
            args = SimpleNamespace(socket='unused', model_demand_file=marker)
            with patch.object(m, 'model_health', return_value='ready'), \
                 patch.object(m, 'generate', side_effect=OSError('failed')):
                with self.assertRaises(OSError): m.generate_for_request(args, 'question', None)
            self.assertFalse(marker.exists())
            with patch.object(m, 'model_health', return_value='unavailable'), \
                 patch.object(m, 'MODEL_STARTUP_SECONDS', 0):
                with self.assertRaises(TimeoutError): m.generate_for_request(args, 'question', None)
            self.assertFalse(marker.exists())

    @unittest.skipUnless(os.geteuid() == 0, 'requires root to test file ownership')
    def test_demand_validation_rejects_stale_and_unsafe_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            marker = Path(tmp) / 'demand'
            pid = os.getpid()
            started = Path('/proc/self/stat').read_text().rpartition(')')[2].split()[19]
            marker.write_text(f'{pid} {started}\n')
            uid = pwd.getpwnam('minecraft').pw_uid
            os.chown(marker, uid, -1)
            os.chmod(marker, 0o600)
            self.assertTrue(wiki_demand.requested(marker, pid))
            self.assertFalse(wiki_demand.requested(marker, pid + 10000000))
            link = Path(tmp) / 'link'
            link.symlink_to(marker)
            self.assertFalse(wiki_demand.requested(link, pid))
            os.chmod(marker, 0o644)
            self.assertFalse(wiki_demand.requested(marker, pid))
            os.chmod(marker, 0o600)
            hardlink = Path(tmp) / 'hardlink'
            os.link(marker, hardlink)
            self.assertFalse(wiki_demand.requested(marker, pid))


if __name__ == '__main__':
    unittest.main()
