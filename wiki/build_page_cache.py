"""Experimentally generate model-specific llama-server page prefix states.

Run against a dedicated local server configured with one slot and a private
--slot-save-path matching --output. These files are not used by production.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
from urllib.parse import urlsplit
from urllib.request import Request, urlopen

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'runtime/usr/local/lib/n20'))
from wiki_answer import chat_request_body


def digest_file(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def request(url, path, data=None):
    encoded = None if data is None else json.dumps(data).encode()
    req = Request(url + path, data=encoded, headers={'Content-Type': 'application/json'})
    with urlopen(req, timeout=600) as response:
        return json.load(response)


def page_passage(entry):
    if entry.get('version') != 1:
        raise ValueError('unsupported parsed page version')
    article = entry['article']
    sections = [s for s in article['sections'] if s['default'] and s['blocks']]
    text = '\n\n'.join(s['heading'] + '\n' + '\n'.join(b['text'] for b in s['blocks']) for s in sections)
    if not text:
        raise ValueError('page has no default gameplay content')
    return {'title': article['title'], 'revision': article['revision'], 'section': 'Gameplay', 'anchor': '', 'text': text}


def prefix_tokens(server, passage):
    # Render full real chat prompts, then use their common token prefix. This
    # accounts for template syntax, special tokens and token boundary merges.
    sentinels = ['ALPHA_player_question_321', 'zebra_player_question_987', '42_player_question_654']
    sequences = []
    for question in sentinels:
        body = chat_request_body(question, [passage])
        rendered = request(server, '/apply-template', {'messages': body['messages'], 'chat_template_kwargs': body['chat_template_kwargs']})['prompt']
        if rendered.count(question) != 1:
            raise ValueError('chat template did not preserve the question exactly once')
        sequences.append(request(server, '/tokenize', {'content': rendered, 'add_special': True, 'parse_special': True})['tokens'])
    common = []
    for group in zip(*sequences):
        if not all(token == group[0] for token in group):
            break
        common.append(group[0])
    if not common:
        raise ValueError('chat template produced no shared prefix')
    return common


def build(args):
    url = args.server.rstrip('/')
    parsed = urlsplit(url)
    if parsed.scheme != 'http' or parsed.hostname not in ('127.0.0.1', 'localhost', '::1'):
        raise ValueError('server must be a dedicated local HTTP server')
    props = request(url, '/props')
    if props['total_slots'] != 1:
        raise ValueError('server must have exactly one slot')
    if Path(props['model_path']).resolve() != args.model.resolve():
        raise ValueError('server model does not match --model')
    args.output.mkdir(mode=0o700, parents=True, exist_ok=True)
    model_hash, runtime_hash = digest_file(args.model), digest_file(args.runtime)
    manifest = []
    errors = 0
    for path in sorted(args.cache.glob('*.json')):
        try:
            entry = json.loads(path.read_text(encoding='utf-8'))
            passage = page_passage(entry)
            tokens = prefix_tokens(url, passage)
            limit = props['default_generation_settings']['n_ctx'] - args.reserve_tokens
            if len(tokens) > limit:
                raise ValueError(f'prefix has {len(tokens)} tokens; limit is {limit}')
            identity = {'version': 1, 'model_sha256': model_hash, 'runtime_sha256': runtime_hash,
                        'build_info': props['build_info'], 'profile': args.profile,
                        'context': props['default_generation_settings']['n_ctx'],
                        'title': passage['title'], 'revision': passage['revision'],
                        'tokens_sha256': hashlib.sha256(json.dumps(tokens, separators=(',', ':')).encode()).hexdigest()}
            key = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()
            filename = key + '.state'
            response = request(url, '/completion', {'prompt': tokens, 'n_predict': 0, 'cache_prompt': True, 'id_slot': 0})
            if response.get('truncated'):
                raise ValueError('server truncated prefix')
            request(url, '/slots/0?action=save', {'filename': filename})
            state = args.output / filename
            if not state.is_file() or not state.stat().st_size:
                raise ValueError('server did not write state to --output; check --slot-save-path')
            manifest.append({**identity, 'file': filename, 'bytes': state.stat().st_size,
                             'state_sha256': digest_file(state), 'prefix_tokens': len(tokens)})
            print(json.dumps({'title': passage['title'], 'tokens': len(tokens), 'bytes': state.stat().st_size}), flush=True)
        except (KeyError, ValueError, OSError) as error:
            errors += 1
            print(json.dumps({'file': path.name, 'error': str(error)}), file=sys.stderr, flush=True)
    temp = args.output / '.manifest.tmp'
    temp.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    os.replace(temp, args.output / 'manifest.json')
    return manifest, errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--runtime', type=Path, required=True, help='llama-server binary used by this server')
    parser.add_argument('--profile', required=True, help='exact runtime/backend/context configuration identity')
    parser.add_argument('--server', default='http://127.0.0.1:8080')
    parser.add_argument('--reserve-tokens', type=int, default=512)
    args = parser.parse_args()
    if args.reserve_tokens < 1:
        parser.error('--reserve-tokens must be positive')
    _, errors = build(args)
    if errors:
        sys.exit(1)


if __name__ == '__main__':
    main()
