"""Check synthetic location-evaluation labels and production prompt parity."""
import importlib.util
from pathlib import Path
import sys
import unittest
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime/usr/local/lib/n20'))
import wiki_chat

spec = importlib.util.spec_from_file_location('benchmark_find', ROOT / 'wiki/benchmark-find.py')
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)


class EvaluationTests(unittest.TestCase):
    def test_labels_and_rotated_locations(self):
        cases = list(benchmark.cases())
        self.assertEqual(len(cases), 116)
        self.assertEqual(len({case['id'] for case in cases}), len(cases))
        for case in cases:
            self.assertLessEqual(len(wiki_chat.marker_prompt(case['markers']).encode('utf-8')),
                                 wiki_chat.MAX_MARKER_PROMPT_BYTES)
            if case['category'] == 'match':
                self.assertIn(case['markers'][case['expected'] - 1]['note'],
                              [note for note, target in benchmark.NOTES if target in case['query']])
            else:
                self.assertEqual(case['expected'], 0)
        self.assertEqual({case['expected'] for case in cases if case['category'] == 'match'}, set(range(1, 7)))

    def test_benchmark_uses_bridge_prompt(self):
        case = next(benchmark.cases())
        body = benchmark.marker_request_body(case['markers'], case['query'])
        self.assertEqual(body, wiki_chat.marker_request_body(case['markers'], case['query']))
        self.assertIn(case['query'], body['messages'][1]['content'])

    def test_challenge_fits_real_pin_limit(self):
        cases = list(benchmark.challenge_cases())
        self.assertEqual(len(cases), 30)
        for case in cases:
            self.assertLessEqual(len(wiki_chat.marker_prompt(case['markers']).encode('utf-8')),
                                 wiki_chat.MAX_MARKER_PROMPT_BYTES)
            self.assertLessEqual(case['expected'], len(case['markers']))

    def test_literal_match_requires_unique_unqualified_request(self):
        markers = [{'note': 'desert village by a well'}, {'note': 'snowy village'}]
        self.assertEqual(wiki_chat.exact_marker(markers, 'Find the desert village?'), 0)
        self.assertEqual(wiki_chat.exact_marker(markers, 'village'), -1)
        for query in ('the nearest desert village', 'not desert village',
                      'desert village without a well', 'desert village or snowy village'):
            self.assertIsNone(wiki_chat.exact_marker(markers, query))
        self.assertEqual(wiki_chat.exact_marker([{'note': 'village'}, {'note': 'village'}], 'village'), -1)
        self.assertEqual(wiki_chat.exact_marker([{'note': 'desert village destroyed'}], 'desert village'), -1)
        self.assertEqual(wiki_chat.exact_marker([{'note': 'road to desert village'}], 'desert village'), -1)
        for note in ('desert village in ruins',):
            self.assertIsNone(wiki_chat.exact_marker([{'note': note}], 'desert village'))

    def test_bridge_returns_saved_coordinates_without_model(self):
        markers = [{'note': 'iron farm beside river', 'pos': [10, 64, -4],
                    'dimension': 'minecraft:overworld'}]
        with patch.object(wiki_chat, 'load_markers', return_value=markers), \
                patch.object(wiki_chat, 'generate') as generate:
            reply = wiki_chat.find_marker(SimpleNamespace(pins_file=None), 'find the iron farm')
        self.assertIn('iron farm beside river: 10, 64, -4', reply[0]['text'])
        generate.assert_not_called()

    def test_ambiguous_location_does_not_pick_a_pin(self):
        markers = [{'note': 'desert village'}, {'note': 'snow village'}]
        with patch.object(wiki_chat, 'load_markers', return_value=markers), \
                patch.object(wiki_chat, 'generate') as generate:
            reply = wiki_chat.find_marker(SimpleNamespace(pins_file=None), 'village')
        self.assertIn("couldn't match", reply[0]['text'])
        generate.assert_not_called()


if __name__ == '__main__':
    unittest.main()
