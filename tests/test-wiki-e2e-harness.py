"""Check the scratch harness recognizes public acknowledgments and replies."""
import importlib.util
from pathlib import Path
import unittest

path = Path(__file__).resolve().parents[1] / 'wiki/experiments/n20-wiki-e2e.py'
spec = importlib.util.spec_from_file_location('wiki_e2e', path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ConsoleTests(unittest.TestCase):
    def test_acknowledgments_and_answer(self):
        ack, value = module.parse_console_line(b'tellraw @a [{"text":"[Wiki] Looking that up..."}]')
        self.assertTrue(ack)
        self.assertEqual(value[0]['text'], '[Wiki] Looking that up...')
        ack, value = module.parse_console_line(b'tellraw @a [{"text":"[Wiki] Charcoal"}]')
        self.assertFalse(ack)
        self.assertEqual(value[0]['text'], '[Wiki] Charcoal')

    def test_rejects_private_or_invalid_reply(self):
        with self.assertRaises(ValueError):
            module.parse_console_line(b'tellraw WikiBench [{"text":"reply"}]')
        with self.assertRaises(ValueError):
            module.parse_console_line(b'tellraw @a []')


if __name__ == '__main__':
    unittest.main()
