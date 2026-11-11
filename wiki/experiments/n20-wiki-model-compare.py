"""Matched, offline Wiki-model quality and latency trial through Unix sockets.

The fixture freezes retriever output so both workers receive identical excerpts.
This exercises the production model request and reply formatting, not Minecraft
chat, the exact-answer route, retrieval latency, startup, or game ticks.
Fixtures and results contain prompts/answers and belong in private scratch.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys
import time
from urllib.parse import unquote, urlsplit

checkout_runtime = next((parent / "runtime/usr/local/lib/n20" for parent in Path(__file__).resolve().parents
                         if (parent / "runtime/usr/local/lib/n20").is_dir()), Path("/usr/local/lib/n20"))
sys.path.insert(0, str(checkout_runtime))
from wiki_answer import UNKNOWN
import wiki_chat
from wiki_chat import UNAVAILABLE, generate, render
from wiki_fast import exact_answer


def canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")


def digest(value):
    return hashlib.sha256(canonical(value)).hexdigest()


def private_write(path, value):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0), 0o600)
    with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as file:
        json.dump(value, file, ensure_ascii=False, indent=2)
        file.write("\n")


def prepare(args):
    cases = json.loads(args.cases.read_text(encoding="utf-8"))
    if not isinstance(cases, list) or not cases:
        raise ValueError("cases must be a nonempty list")
    by_id = {case["id"]: case for case in cases}
    if len(by_id) != len(cases):
        raise ValueError("duplicate case id")
    ids = args.ids or list(by_id)
    if len(set(ids)) != len(ids) or any(case_id not in by_id for case_id in ids):
        raise ValueError("unknown or duplicate requested id")
    command = [str(args.wiki.resolve()), "--cache", str(args.cache.resolve()), "--batch", "--offline"]
    fixture = []
    with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                          encoding="utf-8") as worker:
        try:
            for case_id in ids:
                case = by_id[case_id]
                worker.stdin.write(json.dumps(case["question"], ensure_ascii=False) + "\n")
                worker.stdin.flush()
                line = worker.stdout.readline()
                if not line:
                    raise RuntimeError("retriever ended early")
                found = json.loads(line)
                if "error" in found or not isinstance(found.get("passages"), list):
                    raise RuntimeError("retrieval failed for " + case_id)
                fixture.append({"case": case, "passages": found["passages"]})
            worker.stdin.close()
            if worker.wait(timeout=10):
                raise RuntimeError("retriever failed")
        finally:
            if worker.poll() is None:
                worker.terminate()
                worker.wait(timeout=10)
    obj = {"version": 1, "items": fixture}
    private_write(args.output, obj)
    print(json.dumps({"fixture_sha256": digest(obj), "cases": len(fixture),
                      "with_passages": sum(bool(row["passages"]) for row in fixture)}))


def grade(case, answer, passages):
    components = render(answer, passages)
    text = components[0]["text"].removeprefix("[Wiki] ")
    link = components[1].get("click_event", {}).get("url", "") if len(components) > 1 else ""
    url = urlsplit(link)
    title = unquote(url.path).removeprefix("/w/").replace("_", " ")
    source_ok = url.netloc == "minecraft.wiki" and title in case.get("titles", [])
    if "expect" not in case:
        correct = text == UNKNOWN
    else:
        correct = (source_ok and all(re.search(p, text, re.I) for p in case["expect"])
                   and not any(re.search(p, text, re.I) for p in case.get("reject", [])))
    return {"text": text, "source_title": title if source_ok else "", "source_ok": source_ok,
            "correct": bool(correct), "abstention": text == UNKNOWN}


def run(args):
    fixture = json.loads(args.fixture.read_text(encoding="utf-8"))
    if fixture.get("version") != 1 or not fixture.get("items"):
        raise ValueError("invalid fixture")
    records = []
    for item in fixture["items"]:
        case, passages = item["case"], item["passages"]
        fast = exact_answer(case["question"], passages) if passages else None
        start = time.monotonic()
        error = None
        captured = []
        if passages:
            try:
                original = wiki_chat.read_completion
                def capture_completion(response, on_content):
                    raw, finish = original(response, on_content)
                    captured.append((raw, finish))
                    return raw, finish
                wiki_chat.read_completion = capture_completion
                try:
                    answer = generate(str(args.socket), case["question"], passages)
                finally:
                    wiki_chat.read_completion = original
            except Exception as exc:
                error = type(exc).__name__
                answer = UNAVAILABLE
        else:
            answer = UNKNOWN
        elapsed = time.monotonic() - start
        raw, finish = captured[0] if captured else ("", None)
        explicit_unknown = bool(re.search(r"^[ \t]*answer:[ \t]*" + re.escape(UNKNOWN) + r"[ \t]*$", raw, re.I | re.M))
        graded = grade(case, answer, passages)
        if passages and finish != "stop":
            graded["correct"] = False
        result = {"id": case["id"], "private_case": "expect" not in case, "question_sha256": digest(case["question"]),
                  "passages_sha256": digest(passages), "has_passages": bool(passages),
                  "fast_path_available": fast is not None, "seconds": elapsed,
                  "error": error, "finish_reason": finish, "explicit_unknown": explicit_unknown,
                  "model_output": raw, **graded}
        records.append(result)
        print(json.dumps({"id": case["id"], "seconds": round(elapsed, 3),
                          "correct": result["correct"], "error": error,
                          "fast_path_available": result["fast_path_available"]}), flush=True)
    private_write(args.output, {"version": 1, "arm": args.arm,
                                "fixture_sha256": digest(fixture), "results": records})


def compare(args):
    baseline = json.loads(args.baseline.read_text(encoding="utf-8"))
    candidate = json.loads(args.candidate.read_text(encoding="utf-8"))
    if baseline.get("version") != 1 or candidate.get("version") != 1 or not baseline.get("fixture_sha256") or baseline["fixture_sha256"] != candidate.get("fixture_sha256"):
        raise ValueError("result fixtures do not match")
    before, after = baseline["results"], candidate["results"]
    if not before or len(before) != len(after):
        raise ValueError("incomplete paired results")
    for left, right in zip(before, after):
        if any(left[k] != right[k] for k in ("id", "private_case", "question_sha256", "passages_sha256", "has_passages", "fast_path_available")):
            raise ValueError("case order or evidence mismatch")
    eligible = [(a, b) for a, b in zip(before, after) if a["has_passages"] and not a["error"] and not b["error"]]
    summary = {"cases": len(before), "paired_successes": len(eligible),
               "baseline_errors": sum(bool(r["error"]) for r in before),
               "candidate_errors": sum(bool(r["error"]) for r in after),
               "baseline_nonstop": sum(r["has_passages"] and r["finish_reason"] != "stop" for r in before),
               "candidate_nonstop": sum(r["has_passages"] and r["finish_reason"] != "stop" for r in after),
               "baseline_correct": sum(r["correct"] for r in before),
               "candidate_correct": sum(r["correct"] for r in after),
               "private_cases": sum(r["private_case"] for r in before),
               "baseline_private_explicit_unknown": sum(r["private_case"] and r["explicit_unknown"] and r["finish_reason"] == "stop" for r in before),
               "candidate_private_explicit_unknown": sum(r["private_case"] and r["explicit_unknown"] and r["finish_reason"] == "stop" for r in after),
               "changed_text": sum(a["text"] != b["text"] for a, b in eligible),
               "regressions": [a["id"] for a, b in zip(before, after) if a["correct"] and not b["correct"]],
               "improvements": [a["id"] for a, b in zip(before, after) if not a["correct"] and b["correct"]],
               "fast_path_cases": sum(r["fast_path_available"] for r in before)}
    if eligible:
        summary["median_baseline_seconds"] = statistics.median(a["seconds"] for a, _ in eligible)
        summary["median_candidate_seconds"] = statistics.median(b["seconds"] for _, b in eligible)
        summary["median_paired_delta_seconds"] = statistics.median(b["seconds"] - a["seconds"] for a, b in eligible)
    print(json.dumps(summary, ensure_ascii=False))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    prep = sub.add_parser("prepare")
    prep.add_argument("--wiki", type=Path, required=True)
    prep.add_argument("--cache", type=Path, required=True)
    prep.add_argument("--cases", type=Path, required=True)
    prep.add_argument("--ids", nargs="+")
    prep.add_argument("--output", type=Path, required=True)
    prep.set_defaults(func=prepare)
    execute = sub.add_parser("run")
    execute.add_argument("--fixture", type=Path, required=True)
    execute.add_argument("--socket", type=Path, required=True)
    execute.add_argument("--arm", required=True)
    execute.add_argument("--output", type=Path, required=True)
    execute.set_defaults(func=run)
    result = sub.add_parser("compare")
    result.add_argument("--baseline", type=Path, required=True)
    result.add_argument("--candidate", type=Path, required=True)
    result.set_defaults(func=compare)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
