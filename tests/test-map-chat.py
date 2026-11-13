"""Exercise marker commands without touching a real server or world."""
import json
import math
import os
from collections import deque
import re
import select
from pathlib import Path
import sys
import tempfile
import threading
import time
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
        self.direction_query = patch.object(m, 'console_direction_query')
        self.direction_queries = self.direction_query.start()
        self.addCleanup(self.direction_query.stop)
        self.capture_query = patch.object(m, 'console_view_capture')
        self.captures = self.capture_query.start()
        self.addCleanup(self.capture_query.stop)
        self.service = m.MarkerQueue(self.args)

    def capture(self, player, pos, dimension):
        self.service.handle(PREFIX + f'System chat: {player} has the following entity data: {pos}')
        self.service.handle(PREFIX + f'System chat: {player} has the following entity data: "{dimension}"')
        token = self.service.inflight[0]["token"]
        self.service.handle(PREFIX + f'System chat: Found no elements matching {token}')
        self.service.advance()

    def capture_direction(self, player, pos='[0.0d, 64.0d, 0.0d]', rotation='[0.0f, 0.0f]', dimension='minecraft:overworld'):
        token = self.service.inflight[0]['token']
        for value in (f'"{dimension}"', rotation, pos, f'"{dimension}"'):
            self.service.handle(PREFIX + f'System chat: {player} has the following entity data: {value}')
        self.service.handle(PREFIX + f'System chat: Found no elements matching {token}')
        self.service.advance()

    def capture_view(self, player, eye='[1.0d, 65.0d, 2.0d]', rotation='[0.0f, 0.0f]', dimension='minecraft:overworld'):
        token = self.service.inflight[0]['token']
        self.service.handle(PREFIX + f'System chat: {token} has the following entity data: {eye}')
        self.service.handle(PREFIX + f'System chat: {player} has the following entity data: {rotation}')
        self.service.handle(PREFIX + f'System chat: {player} has the following entity data: "{dimension}"')
        self.service.handle(PREFIX + f'System chat: Found no elements matching {token}')
        self.service.advance()

    def finish_scan(self, hit=None, missing=None):
        batch = self.service.inflight[0]
        token = batch['token']
        for index in range(len(batch['cells'])):
            if index != missing:
                value = m.ray_sample_base(token) + 2 * index + (index == hit)
                self.service.handle(PREFIX + f'System chat: Number provider returned value {value}')
        self.service.handle(PREFIX + f'System chat: Found no elements matching {token}')
        self.service.next_ray_batch = 0
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
            self.assertIn('small village', m.find_marker(self.args, 'the settlement')[0]['text'])
            self.assertIn('small village', json.dumps(generate.call_args.args[4]))

    def test_timeout_drains_stale_responses_before_next_pin(self):
        self.service.handle(PREFIX + '<Alex> @pin first')
        self.service.handle(PREFIX + '<Alex> @pin second')
        self.service.advance()
        old_token = self.service.inflight[0]['token']
        self.service.inflight[0]['deadline'] = 0
        with patch.object(m, 'console_write') as write:
            self.service.advance()
        self.assertTrue(self.service.pending['resync'])
        new_token = self.service.inflight[0]['token']
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

    def test_thousands_of_pins_are_independent_of_model_context_limit(self):
        markers = [{'note': f'landmark {n}', 'pos': [n, 64, n], 'dimension': 'minecraft:overworld'}
                   for n in range(5000)]
        m.save_markers(self.args.pins_file, markers)
        loaded = m.load_markers(self.args.pins_file)
        self.assertEqual(len(loaded), 5000)
        self.assertIn('landmark 4999', m.direct_marker_reply(loaded, 'landmark 4999')[0]['text'])
        batches = list(m.model_marker_batches(loaded))
        self.assertEqual(sum(map(len, batches)), 5000)
        self.assertIn('landmark 4999', [item['note'] for item in batches[-1]])
        for batch in batches:
            self.assertLessEqual(len(m.marker_note_prompt(batch).encode('utf-8')), m.MAX_MARKER_PROMPT_BYTES)
            self.assertNotIn('minecraft:overworld', m.marker_request_body(batch, 'where?')['messages'][1]['content'])

    def test_pin_can_be_added_after_old_prompt_size_limit(self):
        markers = [{'note': f'location {n} ' + 'x' * 140, 'pos': [n, 64, n], 'dimension': 'minecraft:overworld'}
                   for n in range(8)]
        m.save_markers(self.args.pins_file, markers)
        self.assertGreater(len(m.marker_note_prompt(markers).encode('utf-8')), 700)
        self.service.handle(PREFIX + '<Alex> @pin another place')
        self.service.advance()
        self.capture('Alex', '[5.0d, 64.0d, 7.0d]', 'minecraft:overworld')
        self.assertEqual(len(m.load_markers(self.args.pins_file)), 9)

    def test_map_sorts_case_insensitively_without_reordering_storage(self):
        markers = [{'note': note, 'pos': [0, 64, 0], 'dimension': 'minecraft:overworld'}
                   for note in ('zebra', 'Apple', 'apricot', 'apple')]
        with patch.object(m, 'load_markers', return_value=markers):
            self.service.handle(PREFIX + '<Alex> @map')
            self.service.advance()
        listing = self.replies[0][1]
        self.assertEqual([line.split(':')[0] for line in listing.splitlines()],
                         ['[Map] Apple', 'apple', 'apricot', 'zebra'])
        self.assertEqual([marker['note'] for marker in markers], ['zebra', 'Apple', 'apricot', 'apple'])

    def test_large_map_is_paged_without_dropping_results(self):
        markers = [{'note': f'place {n:04d}', 'pos': [n, 64, 0], 'dimension': 'minecraft:overworld'}
                   for n in reversed(range(1000))]
        with patch.object(m, 'load_markers', return_value=markers):
            self.service.handle(PREFIX + '<Alex> @map')
            self.service.advance()
            while self.service.pending:
                self.capture_direction('Alex')
        public = '\n'.join(text for _, text, kw in self.replies if kw.get('broadcast'))
        for n in range(1000):
            self.assertIn(f'place {n:04d}:', public)
        self.direction_queries.assert_not_called()

    def test_keyword_find_can_bypass_busy_or_unavailable_model(self):
        markers = [{'note': 'Small village with a well', 'pos': [1, 2, 3], 'dimension': 'minecraft:overworld'},
                   {'note': 'Snow village by a tower', 'pos': [4, 5, 6], 'dimension': 'minecraft:overworld'}]
        requests = deque([((42, '1'), 'Alex', 'unrelated wiki question'),
                          ((42, '1'), 'Steve', '@map village with a well')])
        with patch.object(m, 'load_markers', return_value=markers), patch.object(m, 'generate') as model:
            result = m.take_fast_find(self.args, requests)
        self.assertEqual(result[0:2], ('Steve', '@map village with a well'))
        self.assertIn('Small village with a well', result[2][0]['text'])
        self.assertEqual(len(requests), 1)
        model.assert_not_called()

    def test_search_requires_whole_query_and_question_mark_uses_model(self):
        markers = [{'note': 'Village by a well'}, {'note': 'village with a well by a mountain'}]
        self.assertEqual(m.direct_marker_indices(markers, 'village'), [0, 1])
        self.assertEqual(m.direct_marker_indices(markers, 'VILLAGE WITH A WELL'), [1])
        for query in ('village?', 'village well', 'village without a well', 'vill'):
            self.assertIsNone(m.direct_marker_indices(markers, query))

    def test_find_returns_all_literal_and_keyword_matches(self):
        markers = [{'note': note, 'pos': [n, 64, n], 'dimension': 'minecraft:overworld'}
                   for n, note in enumerate(('Village by a well', 'village with a well by a mountain', 'iron farm'))]
        for query in ('village', 'well'):
            with self.subTest(query=query):
                result = m.direct_marker_reply(markers, query)[0]['text']
                self.assertIn('Village by a well', result)
                self.assertIn('village with a well by a mountain', result)
                self.assertNotIn('iron farm', result)

    def test_map_routes_listing_and_search_and_question_mark_waits_for_model(self):
        requests = deque()
        with patch.object(m, 'load_markers', return_value=[{'note': 'village', 'pos': [1, 2, 3], 'dimension': 'minecraft:overworld'}]):
            listing = PREFIX + '<Alex> @map'
            self.service.handle(listing)
            m.accept_chat(listing, self.args, requests, None, 0, False)
            self.service.advance()
            self.assertFalse(requests)
            self.assertTrue(any('village:' in text for _, text, _ in self.replies))
            for query in ('village', 'village?'):
                line = PREFIX + '<Alex> @map ' + query
                self.service.handle(line)
                m.accept_chat(line, self.args, requests, None, 0, False)
            self.assertFalse(self.service.queue)
            first = m.take_fast_find(self.args, requests)
            self.assertEqual(first[1], '@map village')
            self.assertIsNone(m.take_fast_find(self.args, requests))
            self.assertEqual(requests[0][2], '@map village?')

    def test_removed_find_command_is_not_queued(self):
        requests = deque()
        line = PREFIX + '<Alex> @find village'
        self.service.handle(line)
        m.accept_chat(line, self.args, requests, None, 0, True)
        self.assertFalse(requests)
        self.assertFalse(self.service.queue)

    def test_map_directions_use_fresh_snapshot_and_are_private(self):
        markers = [{'note': 'target', 'pos': [10, 70, 10], 'dimension': 'minecraft:overworld'},
                   {'note': 'nether', 'pos': [10, 70, 10], 'dimension': 'minecraft:the_nether'}]
        with patch.object(m, 'load_markers', return_value=markers):
            self.service.enqueue_listing('Alex', markers, markers)
            self.service.advance()
            self.direction_queries.assert_called_once()
            self.assertFalse(any('go ' in text for _, text, _ in self.replies))
            self.capture_direction('Alex', pos='[0.0d, 64.0d, 0.0d]', rotation='[0.0f, 0.0f]')
        private = [(text, kw) for _, text, kw in self.replies if not kw.get('broadcast')]
        self.assertEqual(len(private), 1)
        self.assertTrue(any('go up and forward and to your left' in text for text, _ in private))
        self.assertTrue(any('another' in text or 'direction unavailable' in text for text, _ in private))

    def test_map_without_search_does_not_request_directions(self):
        markers = [{'note': 'target', 'pos': [10, 70, 10], 'dimension': 'minecraft:overworld'}]
        with patch.object(m, 'load_markers', return_value=markers):
            self.service.handle(PREFIX + '<Alex> @map')
            self.service.advance()
        self.direction_queries.assert_not_called()
        self.assertTrue(any('target:' in text for _, text, _ in self.replies))

    def test_directions_rotate_with_player_facing(self):
        marker = {'pos': [10, 64, 0], 'dimension': 'minecraft:overworld'}
        self.assertIn('left', m.relative_direction(marker, [0, 64, 0], [0, 0], 'minecraft:overworld'))
        self.assertIn('back', m.relative_direction(marker, [0, 64, 0], [90, 0], 'minecraft:overworld'))
        self.assertIn('right', m.relative_direction(marker, [0, 64, 0], [180, 0], 'minecraft:overworld'))

    def test_nearest_landmark_excludes_target_and_other_dimensions(self):
        target = {'note': 'target', 'pos': [10, 70, 10], 'dimension': 'minecraft:overworld'}
        near = {'note': 'village', 'pos': [4, 65, 14], 'dimension': 'minecraft:overworld'}
        far = {'note': 'far', 'pos': [100, 70, 10], 'dimension': 'minecraft:overworld'}
        other = {'note': 'other', 'pos': [10, 70, 10], 'dimension': 'minecraft:the_nether'}
        self.assertEqual(m.landmark_direction(target, [target, far, other, near], True),
                         'From village: 6 blocks east, 4 blocks north, 5 blocks up')
        self.assertIsNone(m.landmark_direction(target, [target, other], True))
        self.assertEqual(m.landmark_direction(target, [target], False), 'From target: at the same location')

    def test_death_locations_and_multiple_players_are_persisted(self):
        self.args.deaths_file = Path(self.tmp.name) / 'deaths.json'
        token = 'n20_pin_' + 'a' * 32
        batch = {'kind': 'death_poll', 'server': (42, '1'), 'token': token, 'players': {}}
        self.service.pending = {'action': 'death_poll'}
        self.service.inflight.append(batch)
        with patch.object(m, 'console_write') as write:
            for name, number in [('Alex', 1), ('Steve', 2)]:
                self.service.handle(PREFIX + f'{name} has the following entity data: [I; 0, 0, 0, {number}]')
                self.service.handle(PREFIX + f'{name} has the following entity data: {{pos: [I; {number}, 64, -4], dimension: "minecraft:overworld"}}')
                self.service.handle(PREFIX + f'{name} has {number} [n20_death]')
                self.service.handle(PREFIX + f'{name} has {number} [n20_death]')
            self.service.handle(PREFIX + f'Found no elements matching {token}')
        saved = m.load_deaths(self.args.deaths_file)
        self.assertEqual(len(saved), 2)
        self.assertEqual(saved['0' * 31 + '1']['deaths'][0]['pos'], [1, 64, -4])
        self.assertEqual(len(self.service.death_notices), 2)
        self.assertEqual(write.call_count, 2)
        self.assertGreaterEqual(self.service.death_notices[0][0], time.monotonic() + .9)

    def test_death_history_caps_at_five_and_deduplicates_retry(self):
        self.args.deaths_file = Path(self.tmp.name) / 'deaths.json'
        identity = '0' * 31 + '1'
        with patch.object(m, 'console_write'):
            for score in [1, 2, 3, 4, 5, 6, 6]:
                self.service.finish_death_query({'kind': 'death_poll', 'server': (42, '1'),
                    'players': {'Alex': {'uuid': identity, 'location': {'pos': [score, 64, 0],
                    'dimension': 'minecraft:overworld'}, 'scores': [score, score]}}})
        saved = m.load_deaths(self.args.deaths_file)[identity]
        self.assertEqual([death['pos'][0] for death in saved['deaths']], [6, 5, 4, 3, 2])
        self.assertEqual(len(self.service.death_notices), 6)

    def test_death_command_uses_uuid_and_private_directions(self):
        self.args.deaths_file = Path(self.tmp.name) / 'deaths.json'
        identity = '0' * 31 + '1'
        m.save_markers(self.args.deaths_file, {identity: {'score': 1, 'deaths': [
            {'pos': [5, 65, 10], 'dimension': 'minecraft:overworld'}]}})
        with patch.object(m, 'console_write'):
            self.service.handle(PREFIX + '<Alex> @death')
            self.service.advance()
            token = self.service.inflight[0]['token']
            self.service.handle(PREFIX + 'Alex has the following entity data: [I; 0, 0, 0, 1]')
            self.service.handle(PREFIX + f'Found no elements matching {token}')
            self.service.advance()
            self.capture_direction('Alex')
        self.assertTrue(any('Death 1: 5, 65, 10' in text for _, text, _ in self.replies))
        self.assertTrue(any('[Death] Directions:' in text for _, text, _ in self.replies))
        self.assertTrue(all(not kwargs.get('broadcast') for _, _, kwargs in self.replies))

    def test_search_directions_include_nearest_landmark(self):
        target = {'note': 'target', 'pos': [10, 70, 10], 'dimension': 'minecraft:overworld'}
        near = {'note': 'village', 'pos': [4, 65, 14], 'dimension': 'minecraft:overworld'}
        self.service.enqueue_listing('Alex', [target], [target, near])
        self.service.advance()
        self.capture_direction('Alex')
        private = [text for _, text, kw in self.replies if not kw.get('broadcast')]
        self.assertTrue(any('From village: 6 blocks east, 4 blocks north, 5 blocks up' in text for text in private))

    def test_pin_that_saves_hit_and_waits_before_map(self):
        with patch.object(m, 'console_raycast') as cast:
            self.service.handle(PREFIX + '<Alex> @pin_that village gate')
            self.service.handle(PREFIX + '<Steve> @map')
            self.capture_view('Alex')
            cast.assert_called_once()
            # Camera changes after capture must not alter the captured ray.
            self.service.handle(PREFIX + 'System chat: Alex has the following entity data: [180.0f, 0.0f]')
            self.finish_scan(hit=2)
        self.assertEqual(m.load_markers(self.args.pins_file)[0]['pos'], [1, 65, 4])
        self.assertTrue(any('village gate: 1, 65, 4' in text for _, text, _ in self.replies))
        self.assertTrue(all(kw.get('broadcast') is True for _, _, kw in self.replies))

    def test_pin_that_missing_capture_and_missing_target_do_not_save(self):
        with patch.object(m, 'console_raycast'):
            self.service.handle(PREFIX + '<Alex> @pin_that test')
            token = self.service.inflight[0]['token']
            self.service.handle(PREFIX + f'System chat: Found no elements matching {token}')
            self.service.advance()
            self.service.handle(PREFIX + '<Alex> @pin_that test')
            self.capture_view('Alex')
            self.service.pending['cells'] = self.service.pending['cells'][:3]
            self.finish_scan()
        self.assertFalse(self.args.pins_file.exists())
        self.assertTrue(any('Could not capture' in text for _, text, _ in self.replies))
        self.assertTrue(any('No block was found' in text for _, text, _ in self.replies))

    def test_multiple_players_capture_views_before_queued_scans(self):
        with patch.object(m, 'console_raycast') as scan:
            self.service.handle(PREFIX + '<Alex> @pin_that first')
            self.service.handle(PREFIX + '<Steve> @pin_that second')
            self.assertEqual(self.captures.call_count, 2)
            self.capture_view('Alex', eye='[1.0d, 65.0d, 2.0d]')
            self.capture_view('Steve', eye='[10.0d, 70.0d, 20.0d]', rotation='[90.0f, 0.0f]')
            self.assertEqual(scan.call_count, 1)
            self.finish_scan(hit=1)
            self.assertEqual(scan.call_count, 2)
            self.finish_scan(hit=0)
        markers = m.load_markers(self.args.pins_file)
        self.assertEqual([item['note'] for item in markers], ['first', 'second'])
        self.assertAlmostEqual(markers[0]['pos'][2], 3.0)
        self.assertAlmostEqual(markers[1]['pos'][0], 9.0)

    def test_scans_yield_between_batches_to_other_players(self):
        with patch.object(m, 'console_raycast') as scan:
            self.service.handle(PREFIX + '<Alex> @pin_that first')
            self.service.handle(PREFIX + '<Steve> @pin_that second')
            self.capture_view('Alex')
            self.capture_view('Steve', eye='[10.0d, 70.0d, 20.0d]')
            self.finish_scan()  # Alex's first batch is transparent.
            self.assertEqual(self.service.pending['player'], 'Steve')
            self.finish_scan(hit=1)
            self.assertEqual(self.service.pending['player'], 'Alex')
            self.finish_scan(hit=0)
        self.assertEqual(scan.call_count, 3)
        self.assertEqual([p['note'] for p in m.load_markers(self.args.pins_file)], ['second', 'first'])

    def test_ray_batches_are_paced(self):
        with patch.object(m, 'console_raycast') as scan, patch.object(m.time, 'monotonic', return_value=100.0):
            self.service.handle(PREFIX + '<Alex> @pin_that first')
            self.capture_view('Alex')
            token = self.service.inflight[0]['token']
            for index in range(len(self.service.inflight[0]['cells'])):
                value = m.ray_sample_base(token) + 2 * index
                self.service.handle(PREFIX + f'System chat: Number provider returned value {value}')
            self.service.handle(PREFIX + f'System chat: Found no elements matching {token}')
            self.service.advance()
            self.assertEqual(scan.call_count, 1)
        with patch.object(m, 'console_raycast') as later, patch.object(m.time, 'monotonic', return_value=100.06):
            self.service.advance()
            later.assert_called_once()

    def test_busy_console_retries_the_same_batch(self):
        with patch.object(m, 'console_raycast', side_effect=[BlockingIOError(), None]) as scan:
            self.service.handle(PREFIX + '<Alex> @pin_that first')
            self.capture_view('Alex')
            self.assertIsNone(self.service.pending)
            self.assertEqual(len(self.service.queue), 1)
            self.service.next_ray_batch = 0
            self.service.advance()
            self.assertEqual(scan.call_args_list[0].args[3], scan.call_args_list[1].args[3])
            self.finish_scan(hit=0)
        self.assertEqual(m.load_markers(self.args.pins_file)[0]['note'], 'first')

    def test_voxel_traversal_covers_diagonals_and_negative_boundaries(self):
        south = {'eye': [0.5, 64.5, 0.5], 'rotation': [0, 0], 'dimension': 'minecraft:overworld'}
        self.assertEqual(m.ray_voxels(south, 3), [(0, 64, 0), (0, 64, 1), (0, 64, 2), (0, 64, 3)])
        diagonal = {'eye': [0.5, 64.5, 0.5], 'rotation': [-45, 0], 'dimension': 'minecraft:overworld'}
        self.assertEqual(m.ray_voxels(diagonal, 3), [(0, 64, 0), (1, 64, 1), (2, 64, 2)])
        west = {'eye': [0.0, 64.5, 0.5], 'rotation': [90, 0], 'dimension': 'minecraft:overworld'}
        self.assertEqual(m.ray_voxels(west, 2), [(-1, 64, 0), (-2, 64, 0)])

    def test_voxel_traversal_contains_dense_ray_samples(self):
        for yaw, pitch in ((0, 0), (17, 11), (-45, -20), (90, 45), (178, 89)):
            snapshot = {'eye': [-3.2, 64.7, 1.3], 'rotation': [yaw, pitch], 'dimension': 'minecraft:overworld'}
            cells = set(m.ray_voxels(snapshot, 8))
            for step in range(800):
                point = m.ray_position(snapshot, step / 100)
                self.assertIn(tuple(map(math.floor, point)), cells)

    def test_long_dimension_names_use_a_smaller_atomic_batch(self):
        dimension = 'x:' + 'a' * 254
        with patch.object(m, 'console_raycast') as scan, patch.object(m, 'RAY_BATCH_SIZE', 20):
            self.service.handle(PREFIX + '<Alex> @pin_that target')
            self.capture_view('Alex', dimension=dimension)
            snapshot, token, cells = scan.call_args.args[1:]
            self.assertLess(len(cells), m.RAY_BATCH_SIZE)
            self.assertLessEqual(len(''.join(m.raycast_commands(snapshot, token, cells)).encode('ascii')), 4096)

    def test_unloaded_cell_before_a_hit_does_not_save(self):
        with patch.object(m, 'console_raycast') as scan:
            self.service.handle(PREFIX + '<Alex> @pin_that far target')
            self.capture_view('Alex')
            self.finish_scan(hit=2, missing=1)
            self.assertEqual(scan.call_count, 1)
        self.assertFalse(self.args.pins_file.exists())
        self.assertTrue(any('unavailable block' in text for _, text, _ in self.replies))

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

    @unittest.skipUnless(hasattr(os, 'mkfifo'), 'requires a POSIX FIFO')
    def test_ray_batch_is_written_atomically(self):
        fifo = Path(self.tmp.name) / 'console'
        os.mkfifo(fifo)
        reader = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
        self.addCleanup(os.close, reader)
        chunks = []
        def drain():
            while True:
                try:
                    data = os.read(reader, 1024)
                except BlockingIOError:
                    time.sleep(.001)
                    continue
                if data:
                    chunks.append(data)
                    time.sleep(.001)
                elif chunks:
                    return
                else:
                    time.sleep(.001)
        thread = threading.Thread(target=drain, daemon=True)
        thread.start()
        token = 'n20_pin_' + 'f' * 32
        snapshot = {'eye': [0, 65, 0], 'rotation': [0, 0], 'dimension': 'minecraft:overworld'}
        cells = m.ray_voxels(snapshot)[:m.RAY_BATCH_SIZE]
        m.console_raycast(fifo, snapshot, token, cells)
        thread.join(5)
        self.assertFalse(thread.is_alive())
        self.assertEqual(b''.join(chunks).decode('ascii'), ''.join(m.raycast_commands(snapshot, token, cells)))

    @unittest.skipUnless(hasattr(os, 'mkfifo'), 'requires a POSIX FIFO')
    def test_full_console_does_not_block_or_partially_write_a_ray_batch(self):
        fifo = Path(self.tmp.name) / 'console'
        os.mkfifo(fifo)
        fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
        self.addCleanup(os.close, fd)
        written = 0
        while True:
            try:
                written += os.write(fd, b'x' * 4096)
            except BlockingIOError:
                break
        snapshot = {'eye': [0, 65, 0], 'rotation': [0, 0], 'dimension': 'minecraft:overworld'}
        started = time.monotonic()
        with self.assertRaises(BlockingIOError):
            m.console_raycast(fifo, snapshot, 'n20_pin_' + 'a' * 32, m.ray_voxels(snapshot)[:3])
        self.assertLess(time.monotonic() - started, 1)
        actual = bytearray()
        while len(actual) < written:
            actual.extend(os.read(fd, written - len(actual)))
        self.assertEqual(actual, b'x' * written)

    @unittest.skipUnless(hasattr(os, 'fork'), 'requires a Unix FIFO and fork')
    def test_bridge_sends_map_results_publicly_and_directions_privately_without_model(self):
        import multiprocessing
        self.reply.stop()
        self.query.stop()
        self.direction_query.stop()
        self.capture_query.stop()
        log = Path(self.tmp.name) / 'server.log'
        log.write_text('')
        fifo = Path(self.tmp.name) / 'console'
        os.mkfifo(fifo)
        fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
        self.addCleanup(os.close, fd)
        m.save_markers(self.args.pins_file, [
            {'note': 'village west', 'pos': [-10, 70, 10], 'dimension': 'minecraft:overworld'},
            {'note': 'Village east', 'pos': [10, 64, 10], 'dimension': 'minecraft:overworld'}])
        args = SimpleNamespace(log=log, console=fifo, pid_file=self.args.pid_file,
                               socket=Path(self.tmp.name) / 'model.sock', pins_file=self.args.pins_file,
                               status_file=Path(self.tmp.name) / 'status', history_file=None)
        ready = Path(self.tmp.name) / 'ready'
        original = m.LogTail
        class SignallingTail(original):
            def read(self):
                lines = super().read()
                ready.touch()
                return lines
        def run_bridge():
            with patch.object(m, 'LogTail', SignallingTail), patch.object(m, 'server_identity', return_value=(42, '1')), \
                 patch.object(m, 'model_health', return_value='unavailable'), patch.object(m, 'resources_available', return_value=False):
                m.run(args)
        process = multiprocessing.get_context('fork').Process(target=run_bridge)
        process.start()
        try:
            deadline = time.monotonic() + 5
            while not ready.exists() and time.monotonic() < deadline:
                time.sleep(.01)
            self.assertTrue(ready.exists())
            with log.open('a') as stream:
                stream.write(PREFIX + '<Alex> @map village\n')
            commands = []
            buffer = b''
            deadline = time.monotonic() + 5
            token = None
            while time.monotonic() < deadline and token is None:
                if select.select([fd], [], [], .1)[0]:
                    buffer += os.read(fd, 65536)
                    lines = buffer.split(b'\n')
                    buffer = lines.pop()
                    commands.extend(lines)
                    for line in lines:
                        found = re.search(rb'data get storage n20:(n20_pin_[0-9a-f]{32}) ', line)
                        if found:
                            token = found[1].decode()
            self.assertIsNotNone(token)
            self.assertTrue(any(line.startswith(b'tellraw @a ') and b'village west' in line and b'Village east' in line for line in commands))
            with log.open('a') as stream:
                for value in ('"minecraft:overworld"', '[0.0f, 0.0f]', '[0.0d, 64.0d, 0.0d]', '"minecraft:overworld"'):
                    stream.write(PREFIX + f'System chat: Alex has the following entity data: {value}\n')
                stream.write(PREFIX + f'System chat: Found no elements matching {token}\n')
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline and not any(line.startswith(b'tellraw Alex ') and b'Directions:' in line for line in commands):
                if select.select([fd], [], [], .1)[0]:
                    buffer += os.read(fd, 65536)
                    lines = buffer.split(b'\n')
                    buffer = lines.pop()
                    commands.extend(lines)
            self.assertTrue(any(line.startswith(b'tellraw Alex ') and b'Directions:' in line for line in commands))
        finally:
            process.terminate()
            process.join(5)


if __name__ == '__main__':
    unittest.main()
