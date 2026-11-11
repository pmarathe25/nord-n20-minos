"""Measure an optimistic exact token-prefix reuse bound for Wiki prompts."""
import argparse
import json
from pathlib import Path
import random
import statistics
import struct
import subprocess

from wiki_answer import chat_request_body


def lcp(a, b):
    count = 0
    for left, right in zip(a, b):
        if left != right:
            break
        count += 1
    return count


def read_exact(stream, length):
    result = bytearray()
    while len(result) < length:
        part = stream.read(length - len(result))
        if not part:
            raise RuntimeError("tokenizer ended early")
        result.extend(part)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wiki", required=True)
    parser.add_argument("--cache", required=True)
    parser.add_argument("--cases", type=Path, nargs="+", required=True)
    parser.add_argument("--tokenizer", nargs=argparse.REMAINDER, required=True)
    args = parser.parse_args()
    if not args.tokenizer:
        parser.error("--tokenizer must be followed by its command and arguments")
    cases = []
    seen = set()
    for path in args.cases:
        for case in json.loads(path.read_text(encoding="utf-8")):
            if case["question"] not in seen:
                cases.append(case)
                seen.add(case["question"])
    encoded = []
    with subprocess.Popen([args.wiki, "--cache", args.cache, "--batch", "--offline"],
                          stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                          encoding="utf-8") as wiki, \
         subprocess.Popen(args.tokenizer, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL) as tokenizer:
        for case in cases:
            wiki.stdin.write(json.dumps(case["question"]) + "\n")
            wiki.stdin.flush()
            result = json.loads(wiki.stdout.readline())
            if "error" in result:
                raise RuntimeError(result["error"])
            content = chat_request_body(case["question"], result["passages"])["messages"][1]["content"]
            data = content.encode("utf-8")
            tokenizer.stdin.write(struct.pack("<I", len(data)) + data)
            tokenizer.stdin.flush()
            count, = struct.unpack("<i", read_exact(tokenizer.stdout, 4))
            if not 0 <= count <= 1048576:
                raise RuntimeError("invalid token count")
            tokens = struct.unpack(f"<{count}i", read_exact(tokenizer.stdout, count * 4))
            encoded.append((case["id"], tokens, bool(result["passages"])))
        wiki.stdin.close()
        tokenizer.stdin.close()
        if wiki.wait(timeout=10) or tokenizer.wait(timeout=10):
            raise RuntimeError("retriever or tokenizer failed")
    if len(encoded) < 2:
        raise ValueError("need at least two distinct questions")
    common = min(lcp(encoded[0][1], item[1]) for item in encoded[1:])
    best = []
    pair_count = {n: 0 for n in (8, 16, 32, 64)}
    for i, (_, tokens, _) in enumerate(encoded):
        matches = [max(0, lcp(tokens, other[1]) - common)
                   for j, other in enumerate(encoded) if j != i]
        best.append(max(matches))
        for j in range(i + 1, len(encoded)):
            extra = max(0, lcp(tokens, encoded[j][1]) - common)
            for threshold in pair_count:
                pair_count[threshold] += extra >= threshold
    # Optimistically assume an exact restorable state exists at every shared
    # prefix of each recent prompt. Real recurrent state cannot be rewound.
    recent = {}
    rng = random.Random(621)
    for capacity in (1, 4, 8, 16):
        averages, fractions = [], []
        for _ in range(64):
            order = list(range(len(encoded)))
            rng.shuffle(order)
            values = []
            history = []
            for i in order:
                values.append(max((max(0, lcp(encoded[i][1], encoded[j][1]) - common)
                                   for j in history[-capacity:]), default=0))
                history.append(i)
            averages.append(statistics.mean(values))
            fractions.append(sum(x >= 16 for x in values) / len(values))
        recent[capacity] = {"mean_extra_tokens": round(statistics.mean(averages), 3),
                            "fraction_at_least_16": round(statistics.mean(fractions), 4)}
    print(json.dumps({"questions": len(encoded), "with_passages": sum(x[2] for x in encoded),
                      "common_user_content_tokens": common,
                      "user_content_tokens_median": statistics.median(len(x[1]) for x in encoded),
                      "optimistic_best_extra_tokens_median": statistics.median(best),
                      "optimistic_best_extra_tokens_max": max(best),
                      "questions_with_best_extra_at_least": {n: sum(x >= n for x in best) for n in pair_count},
                      "unordered_pairs_with_extra_at_least": pair_count,
                      "optimistic_recent_cache": recent}, sort_keys=True))


if __name__ == "__main__":
    main()
