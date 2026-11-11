"""Time cached Java Wiki questions from a private chat log to a console FIFO."""

import argparse
import json
import os
from pathlib import Path
import re
import select
import statistics
import subprocess
import sys
import tempfile
import time
from urllib.parse import unquote, urlsplit


def ask(args, question):
    with tempfile.TemporaryDirectory(prefix="n20-wiki-e2e-") as directory:
        directory = Path(directory)
        log = directory / "minecraft.log"
        console = directory / "console"
        pid = directory / "minecraft.pid"
        log.touch()
        pid.write_text(str(os.getpid()))
        os.mkfifo(console, 0o600)
        fd = os.open(console, os.O_RDWR | os.O_NONBLOCK)
        try:
            command = [
                sys.executable, "-B", str(args.bridge), "--offline",
                "--wiki", str(args.wiki), "--cache", str(args.cache), "--socket", str(args.socket),
                "--log", str(log), "--pid-file", str(pid), "--console", str(console),
            ]
            with subprocess.Popen(command, stdout=subprocess.DEVNULL) as worker:
                try:
                    # Allow the bridge to seek to the end of the empty log.
                    time.sleep(0.5)
                    if worker.poll() is not None:
                        raise RuntimeError("Wiki bridge exited before the request")
                    start = time.monotonic()
                    with log.open("a", encoding="utf-8") as stream:
                        stream.write(f"[00:00:00] [Server thread/INFO]: <WikiBench> @wiki {question}\n")
                    if not select.select([fd], [], [], 130)[0]:
                        raise TimeoutError("Wiki bridge did not write a reply")
                    data = os.read(fd, 4096)
                    elapsed = time.monotonic() - start
                    prefix = b"tellraw WikiBench "
                    if not data.startswith(prefix) or data.count(b"\n") != 1:
                        raise ValueError("Invalid Wiki console reply")
                    return elapsed, json.loads(data[len(prefix):])
                finally:
                    if worker.poll() is None:
                        worker.terminate()
                        try:
                            worker.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            worker.kill()
        finally:
            os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge", type=Path, required=True)
    parser.add_argument("--wiki", type=Path, default=Path("/opt/n20-wiki/n20-wiki"))
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--socket", type=Path, required=True)
    parser.add_argument("--cases", type=Path, required=True)
    parser.add_argument("--ids", nargs="+", required=True)
    args = parser.parse_args()
    cases = {case["id"]: case for case in json.loads(args.cases.read_text(encoding="utf-8"))}
    elapsed = []
    for case_id in args.ids:
        case = cases[case_id]
        seconds, reply = ask(args, case["question"])
        elapsed.append(seconds)
        text = reply[0]["text"].removeprefix("[Wiki] ")
        if "expect" in case:
            source = reply[1].get("click_event", {}).get("url", "") if len(reply) > 1 else ""
            url = urlsplit(source)
            title = unquote(url.path).removeprefix("/w/").replace("_", " ")
            correct = (
                url.netloc == "minecraft.wiki" and title in case["titles"]
                and all(re.search(pattern, text, re.I) for pattern in case["expect"])
                and not any(re.search(pattern, text, re.I) for pattern in case.get("reject", ()))
            )
        else:
            correct = text == "I couldn't verify that from the wiki excerpts."
        print(json.dumps({"id": case_id, "seconds": round(seconds, 3), "correct": correct,
                          "reply": reply}, ensure_ascii=False), flush=True)
    print(json.dumps({"median_seconds": round(statistics.median(elapsed), 3)}), flush=True)


if __name__ == "__main__":
    main()
