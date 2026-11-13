"""Score synthetic @find queries against a local llama-server.

The locations are invented and never read the player's world. Scores measure
selection and abstention, not semantic correctness beyond the fixture labels.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
import sys
from urllib.parse import urlsplit
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime/usr/local/lib/n20'))
from wiki_chat import exact_marker, marker_request_body

NOTES = [
    ('desert village by the well', 'desert village'),
    ('snowy village near the spruce forest', 'snowy village'),
    ('abandoned mineshaft under the mesa', 'abandoned mineshaft'),
    ('iron farm beside the river', 'iron farm'),
    ('sheep pen at the hill base', 'sheep pen'),
    ('nether fortress across the lava lake', 'nether fortress'),
    ('end portal in the stronghold', 'end portal'),
    ('woodland mansion behind the dark forest', 'woodland mansion'),
    ('ocean monument west of spawn', 'ocean monument'),
    ('cherry grove on the mountain', 'cherry grove'),
    ('ancient city below the ice', 'ancient city'),
    ('horse stable outside our base', 'horse stable'),
]
TEMPLATES = ['{}', 'where is the {}?', 'find the {}', 'take me to the {}',
             'coordinates for the {}', 'show me the {}', 'locate the {}', 'our {}']
ABSENT = ['jungle temple', 'witch hut', 'desert pyramid', 'bamboo farm', 'slime farm',
          'ocean shipwreck', 'mushroom island', 'pillager outpost', 'igloo', 'swamp village',
          'gold farm', 'mob grinder', 'bee nest', 'ruined portal', 'trial chamber',
          'tree house', 'library', 'diamond mine', 'skeleton spawner', 'fishing dock']

# Each row contains notes, a natural query, and the expected one-based index
# (zero when the notes do not identify a unique matching location).
CHALLENGE = [
    (['desert village by well', 'snow village by river'], 'the snowy village', 2),
    (['desert village by well', 'snow village by river'], 'village', 0),
    (['desert village by well', 'desert village by river'], 'desert village by river', 2),
    (['desert village by well', 'desert village by river'], 'desert village', 0),
    (['iron farm by river', 'gold farm by forest'], 'where do we get iron?', 1),
    (['iron farm by river', 'gold farm by forest'], 'farm', 0),
    (['iron farm by river', 'gold farm by forest'], 'the gold farm near the trees', 2),
    (['horse stable by base', 'sheep pen by hill'], 'where are the horses?', 1),
    (['horse stable by base', 'sheep pen by hill'], 'our cows', 0),
    (['road to the village', 'iron farm by river'], 'village', 0),
    (['village was destroyed', 'iron farm by river'], 'village', 0),
    (['iron farm by river is gone', 'sheep pen by hill'], 'iron farm', 0),
    (['iron farm by river under construction', 'sheep pen by hill'], 'working iron farm', 0),
    (['iron farm by river', 'iron farm by mountain'], 'iron farm', 0),
    (['nether portal at spawn', 'end portal under library'], 'portal to the End', 2),
    (['nether portal at spawn', 'end portal under library'], 'portal', 0),
    (['mushroom island east', 'cherry grove west'], 'pink trees', 2),
    (['mushroom island east', 'cherry grove west'], 'bamboo jungle', 0),
    (['desert temple looted', 'jungle temple unexplored'], 'unexplored temple', 2),
    (['desert temple looted', 'jungle temple unexplored'], 'temple', 0),
    (['village near spawn', 'village near mountains'], 'village near mountains', 2),
    (['village near spawn', 'village near mountains'], 'village by spawn', 1),
    (['stronghold library', 'end portal in stronghold'], 'stronghold', 0),
    (['stronghold library', 'end portal in stronghold'], 'books in stronghold', 1),
    (['ocean monument by ice', 'shipwreck by shore'], 'underwater monument', 1),
    (['ocean monument by ice', 'shipwreck by shore'], 'buried treasure', 0),
    (['my base near lake', 'Steve base near lake'], 'base', 0),
    (['my base near lake', 'Steve base near lake'], 'Steve base', 2),
    (['zombie spawner in cave', 'skeleton spawner in cave'], 'skeleton spawner', 2),
    (['zombie spawner in cave', 'skeleton spawner in cave'], 'spawner', 0),
]


def challenge_cases():
    for i, (notes, query, expected) in enumerate(CHALLENGE, 1):
        markers = [{'note': note, 'pos': [10 * n, 64, -10 * n],
                    'dimension': 'minecraft:overworld'} for n, note in enumerate(notes, 1)]
        yield {'id': f'challenge-{i:02}', 'category': 'match' if expected else 'abstain',
               'query': query, 'markers': markers, 'expected': expected}


def cases():
    markers = [{'note': note, 'pos': [10 * i, 64, -10 * i],
                'dimension': 'minecraft:overworld'} for i, (note, _) in enumerate(NOTES, 1)]
    for i, (_, target) in enumerate(NOTES, 1):
        for j, template in enumerate(TEMPLATES, 1):
            # A production pin list is bounded by the serialized byte limit.
            pool = [markers[(i - 1 + n) % len(markers)] for n in range(6)]
            offset = (i + j) % len(pool)
            ordered = pool[offset:] + pool[:offset]
            expected = (-offset) % len(pool) + 1
            yield {'id': f'match-{i:02}-{j:02}', 'category': 'match',
                   'query': template.format(target), 'markers': ordered, 'expected': expected}
    for i, target in enumerate(ABSENT, 1):
        yield {'id': f'absent-{i:02}', 'category': 'absent',
               'query': 'where is the ' + target + '?', 'markers': markers[i % 7:i % 7 + 6], 'expected': 0}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model-url', default='http://127.0.0.1:8080/v1/chat/completions')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--suite', choices=('standard', 'challenge'), default='standard')
    parser.add_argument('--bridge', action='store_true', help='Use the bridge literal shortcut before the model')
    args = parser.parse_args()
    target = urlsplit(args.model_url)
    if target.scheme != 'http' or target.hostname not in ('127.0.0.1', '::1', 'localhost'):
        parser.error('--model-url must be a local HTTP llama-server endpoint')
    counts = Counter()
    with args.output.open('w', encoding='utf-8') as out:
        for case in (cases() if args.suite == 'standard' else challenge_cases()):
            direct = exact_marker(case['markers'], case['query']) if args.bridge else None
            if direct is not None:
                selected = direct + 1
                correct = selected == case['expected']
                counts[case['category']] += 1
                counts[case['category'] + '_correct'] += correct
                out.write(json.dumps({**case, 'raw': None, 'finish_reason': None,
                                      'selected': selected, 'correct': correct, 'direct': True}) + '\n')
                out.flush()
                continue
            body = marker_request_body(case['markers'], case['query'])
            request = Request(args.model_url, data=json.dumps(body).encode(),
                              headers={'Content-Type': 'application/json'})
            with urlopen(request, timeout=300) as response:
                choice = json.load(response)['choices'][0]
            raw = choice['message']['content'] or ''
            # Match the bridge's strict number parsing, including out-of-range replies.
            value = raw.strip()
            selected = int(value) if re.fullmatch(r'[0-9]{1,2}', value) else None
            if selected is not None and not 0 <= selected <= len(case['markers']):
                selected = None
            correct = choice['finish_reason'] == 'stop' and selected == case['expected']
            counts[case['category']] += 1
            counts[case['category'] + '_correct'] += correct
            out.write(json.dumps({**case, 'raw': raw, 'finish_reason': choice['finish_reason'],
                                  'selected': selected, 'correct': correct, 'direct': False}, ensure_ascii=False) + '\n')
            out.flush()
    print(json.dumps(counts))


if __name__ == '__main__':
    main()
