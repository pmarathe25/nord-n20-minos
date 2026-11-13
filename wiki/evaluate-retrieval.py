"""Judge offline Wiki retrieval and the bridge's exact-answer route.

The labels are heuristic: inspect misses and supporting passages before treating
them as factual judgments. The cache should be a disposable copy.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'runtime/usr/local/lib/n20'))
from wiki_fast import exact_answer


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--wiki', type=Path, required=True)
    p.add_argument('--cache', type=Path, required=True)
    p.add_argument('--cases', type=Path, nargs='+', required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    cases = [case for path in args.cases for case in json.loads(path.read_text(encoding='utf-8'))]
    totals = Counter()
    with subprocess.Popen([str(args.wiki.resolve()), '--cache', str(args.cache.resolve()), '--offline', '--batch'],
                          stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding='utf-8') as proc, \
            args.output.open('w', encoding='utf-8') as out:
        for case in cases:
            proc.stdin.write(json.dumps(case['question']) + '\n')
            proc.stdin.flush()
            result = json.loads(proc.stdout.readline())
            if 'error' in result:
                raise RuntimeError(result['error'])
            passages = result['passages']
            support = '\n'.join(p['text'] for p in passages if p['title'] in case.get('titles', []))
            title_hit = any(p['title'] in case.get('titles', []) for p in passages)
            evidence_hit = all(re.search(pattern, support, re.I) for pattern in case.get('evidence', []))
            answer = exact_answer(case['question'], passages)
            answer_text = re.sub(r'\[\d+\]', '', answer) if answer is not None else ''
            correct = None if answer is None else ('expect' in case and
                all(re.search(pattern, answer_text, re.I) for pattern in case['expect']) and
                not any(re.search(pattern, answer_text, re.I) for pattern in case.get('reject', [])))
            row = {'case': case, 'passages': passages, 'title_hit': title_hit,
                   'evidence_hit': evidence_hit if 'evidence' in case else None,
                   'exact_answer': answer, 'exact_correct': correct}
            out.write(json.dumps(row, ensure_ascii=False) + '\n')
            totals['cases'] += 1
            if 'evidence' in case:
                totals['labeled'] += 1
                totals['title_hits'] += title_hit
                totals['evidence_hits'] += evidence_hit
            if answer is not None:
                totals['exact_answers'] += 1
                totals['exact_correct'] += correct
        proc.stdin.close()
        if proc.wait(timeout=10):
            raise RuntimeError('retriever failed')
    print(json.dumps(totals))


if __name__ == '__main__':
    main()
