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

    def test_literal_match_uses_whole_query_and_question_mark_forces_model(self):
        markers = [{'note': 'desert village by a well'}, {'note': 'snowy village'}]
        self.assertEqual(wiki_chat.direct_marker_indices(markers, 'DESERT village'), [0])
        self.assertEqual(wiki_chat.direct_marker_indices(markers, 'village'), [0, 1])
        for query in ('desert village?', 'Find the desert village', 'village well', 'vill'):
            self.assertIsNone(wiki_chat.direct_marker_indices(markers, query))

    def test_bridge_returns_saved_coordinates_without_model(self):
        markers = [{'note': 'iron farm beside river', 'pos': [10, 64, -4],
                    'dimension': 'minecraft:overworld'}]
        with patch.object(wiki_chat, 'load_markers', return_value=markers), \
                patch.object(wiki_chat, 'generate') as generate:
            reply = wiki_chat.find_marker(SimpleNamespace(pins_file=None), 'iron farm')
        self.assertIn('iron farm beside river: 10, 64, -4', reply[0]['text'])
        generate.assert_not_called()

    def test_shared_keyword_lists_every_matching_location(self):
        markers = [{'note': 'desert village', 'pos': [1, 2, 3], 'dimension': 'minecraft:overworld'},
                   {'note': 'snow village', 'pos': [4, 5, 6], 'dimension': 'minecraft:overworld'}]
        with patch.object(wiki_chat, 'load_markers', return_value=markers), \
                patch.object(wiki_chat, 'generate') as generate:
            reply = wiki_chat.find_marker(SimpleNamespace(pins_file=None), 'village')
        self.assertIn('desert village', reply[0]['text'])
        self.assertIn('snow village', reply[0]['text'])
        generate.assert_not_called()

    def test_model_can_return_multiple_saved_locations(self):
        markers = [{'note': 'a cottage', 'pos': [1, 2, 3], 'dimension': 'minecraft:overworld'},
                   {'note': 'a hut', 'pos': [4, 5, 6], 'dimension': 'minecraft:overworld'}]
        with patch.object(wiki_chat, 'load_markers', return_value=markers), \
             patch.object(wiki_chat, 'resources_available', return_value=True), \
             patch.object(wiki_chat, 'generate', return_value='[1,2]'):
            reply = wiki_chat.find_marker(SimpleNamespace(pins_file=None, socket=None), 'shelters')
        self.assertIn('a cottage', reply[0]['text'])
        self.assertIn('a hut', reply[0]['text'])
        for invalid in ('[1,1]', '[3]', '[true]', '[1,"2"]', 'answer: [1]'):
            self.assertIsNone(wiki_chat.parse_marker_selection(invalid, 2))

    def test_question_mark_forces_model_despite_literal_hits(self):
        markers = [{'note': 'desert village', 'pos': [1, 2, 3], 'dimension': 'minecraft:overworld'},
                   {'note': 'snow village', 'pos': [4, 5, 6], 'dimension': 'minecraft:overworld'}]
        with patch.object(wiki_chat, 'load_markers', return_value=markers), \
             patch.object(wiki_chat, 'resources_available', return_value=True), \
             patch.object(wiki_chat, 'generate', return_value='[2]') as generate:
            reply = wiki_chat.find_marker(SimpleNamespace(pins_file=None, socket='unused'), 'village?')
        self.assertIn('snow village', reply[0]['text'])
        self.assertNotIn('desert village', reply[0]['text'])
        generate.assert_called_once()

    def test_model_searches_every_batch_without_exposing_coordinates(self):
        markers = [{'note': f'building {n}', 'pos': [n, 64, n], 'dimension': 'minecraft:overworld'}
                   for n in range(10)]
        with patch.object(wiki_chat, 'MAX_MARKER_PROMPT_BYTES', 85), \
             patch.object(wiki_chat, 'load_markers', return_value=markers), \
             patch.object(wiki_chat, 'resources_available', return_value=True), \
             patch.object(wiki_chat, 'generate', return_value='[1]') as generate:
            result = wiki_chat.find_marker(SimpleNamespace(pins_file=None, socket='unused'), 'buildings?')
        self.assertGreater(generate.call_count, 1)
        self.assertEqual(len(result.markers), generate.call_count)
        for call in generate.call_args_list:
            self.assertNotIn('minecraft:overworld', call.args[4]['messages'][1]['content'])

    def test_invalid_model_batch_is_not_reported_as_complete(self):
        markers = [{'note': f'building {n}', 'pos': [n, 64, n], 'dimension': 'minecraft:overworld'}
                   for n in range(10)]
        with patch.object(wiki_chat, 'MAX_MARKER_PROMPT_BYTES', 85), \
             patch.object(wiki_chat, 'load_markers', return_value=markers), \
             patch.object(wiki_chat, 'resources_available', return_value=True), \
             patch.object(wiki_chat, 'generate', side_effect=['[1]', 'bad']):
            result = wiki_chat.find_marker(SimpleNamespace(pins_file=None, socket='unused'), 'buildings?')
        self.assertIn('could not complete', result[0]['text'])

    def test_direct_matching_against_synthetic_cases(self):
        resolved = 0
        for case in list(benchmark.cases()) + list(benchmark.challenge_cases()):
            choice = wiki_chat.direct_marker_indices(case['markers'], case['query'])
            if choice is not None:
                resolved += 1
                expected = case.get('expected_matches', [case['expected']] if case['expected'] else [])
                self.assertEqual(sorted(n + 1 for n in choice), sorted(expected), case['id'])
        self.assertGreaterEqual(resolved, 20)


if __name__ == '__main__':
    unittest.main()
