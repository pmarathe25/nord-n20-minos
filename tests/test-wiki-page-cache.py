"""Isolated tests for the experimental page-prefix cache generator."""
import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('build_page_cache', ROOT / 'wiki/build_page_cache.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class PageCacheTests(unittest.TestCase):
    def test_gameplay_render_omits_optional_and_empty_sections(self):
        entry = {'version': 1, 'article': {'title': 'Test', 'revision': 3, 'sections': [
            {'heading': 'Overview', 'default': True, 'blocks': [{'text': 'Useful'}]},
            {'heading': 'History', 'default': False, 'blocks': [{'text': 'Old'}]},
            {'heading': 'Empty', 'default': True, 'blocks': []},
        ]}}
        result = module.page_passage(entry)
        self.assertEqual(result['text'], 'Overview\nUseful')
        self.assertNotIn('Old', result['text'])

    def test_generator_fingerprints_model_and_rejects_oversized_page(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); cache = root / 'cache'; output = root / 'out'
            cache.mkdir(); output.mkdir(); model = root / 'model'; runtime = root / 'runtime'
            model.write_bytes(b'model one'); runtime.write_bytes(b'runtime')
            page = {'version': 1, 'article': {'title': 'Test', 'revision': 42, 'sections': [
                {'heading': 'Overview', 'default': True, 'blocks': [{'text': 'Fact'}]}]}}
            (cache / 'entry.json').write_text(json.dumps(page))
            args = SimpleNamespace(server='http://127.0.0.1:8080', cache=cache, output=output,
                                   model=model, runtime=runtime, profile='test', reserve_tokens=10)
            context = [50]
            def fake_request(url, endpoint, data=None):
                if endpoint == '/props':
                    return {'total_slots': 1, 'model_path': str(model), 'build_info': 'test-build',
                            'default_generation_settings': {'n_ctx': context[0]}}
                if endpoint == '/apply-template':
                    return {'prompt': data['messages'][1]['content']}
                if endpoint == '/tokenize':
                    return {'tokens': list(data['content'].encode())}
                if endpoint == '/completion':
                    return {'truncated': False}
                if endpoint == '/slots/0?action=save':
                    (output / data['filename']).write_bytes(b'state')
                    return {}
                raise AssertionError(endpoint)
            with patch.object(module, 'request', fake_request):
                _, errors = module.build(args)
                self.assertEqual(errors, 1)  # context too small
                context[0] = 4096
                first, errors = module.build(args)
                self.assertEqual(errors, 0)
                self.assertEqual(len(first), 1)
                self.assertEqual(first[0]['state_sha256'], module.digest_file(output / first[0]['file']))
                model.write_bytes(b'model two')
                second, errors = module.build(args)
                self.assertEqual(errors, 0)
                self.assertNotEqual(first[0]['file'], second[0]['file'])


if __name__ == '__main__':
    unittest.main()
