#!/usr/bin/python3
"""Reply to vanilla Minecraft Java @wiki chat from an off-thread worker."""

import argparse
from collections import OrderedDict
from concurrent.futures import ThreadPoolExecutor
import ctypes
import http.client
import json
import os
from pathlib import Path
import re
import select
import socket
import stat
import subprocess
from threading import Condition, Thread
import time
from urllib.parse import quote

from wiki_answer import UNKNOWN, chat_request_body, grounded_reply
from wiki_fast import exact_answer


CHAT = re.compile(
    r"\[\d{2}:\d{2}:\d{2}\] \[Server thread/INFO\]: "
    r"(?:\[Not Secure\] )?<([A-Za-z0-9_]{1,16})> @wiki\s+(.+)"
)
SOURCE = re.compile(r"\[([1-9][0-9]?)\]")
UNAVAILABLE = "Wiki lookup is temporarily unavailable."
MAX_LINE = 4096
REPLY_CACHE_SIZE = 64
MODEL_FIRST_REPLY_SECONDS = 75
MODEL_STREAM_IDLE_SECONDS = 15
MODEL_TOTAL_SECONDS = 105
MAX_MODEL_LINE = 8192
MAX_MODEL_STREAM = 262144


class LogTail:
    def __init__(self, path):
        self.path = path
        self.identity = None
        self.offset = 0
        self.pending = b""
        self.discard = False

    def read(self):
        with os.fdopen(os.open(self.path, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW), "rb") as log:
            info = os.fstat(log.fileno())
            if not stat.S_ISREG(info.st_mode):
                raise ValueError("chat log is not a regular file")
            identity = (info.st_dev, info.st_ino)
            if self.identity is None:
                self.offset = info.st_size
            elif identity != self.identity or info.st_size < self.offset:
                self.offset = 0
                self.pending = b""
                self.discard = False
            self.identity = identity
            log.seek(self.offset)
            chunk = log.read(32768)
            self.offset += len(chunk)
        lines = (self.pending + chunk).split(b"\n")
        self.pending = lines.pop()
        if self.discard and lines:
            lines.pop(0)
            self.discard = False
        if len(self.pending) > MAX_LINE or self.discard:
            self.pending = b""
            self.discard = True
        return (line.decode("utf-8", errors="replace") for line in lines if len(line) <= MAX_LINE)


class UnixHTTP(http.client.HTTPConnection):
    def __init__(self, path, timeout):
        super().__init__("localhost", timeout=timeout)
        self.path = path

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(self.path)


def watch_log_directory(path):
    # Watch the parent so replacing the log does not invalidate the watch.
    modify, close_write, moved_to, create = 0x02, 0x08, 0x80, 0x100
    try:
        libc = ctypes.CDLL(None)
        initialize, add_watch = libc.inotify_init1, libc.inotify_add_watch
    except AttributeError:
        return None
    fd = initialize(os.O_NONBLOCK | os.O_CLOEXEC)
    if fd < 0:
        return None
    if add_watch(fd, os.fsencode(path.parent), modify | close_write | moved_to | create) >= 0:
        return fd
    os.close(fd)
    return None


def find_passages(args, question):
    command = [args.wiki, "--cache", args.cache, "--question"]
    if args.offline:
        command.append("--offline")
    command.extend(["--", question])
    result = subprocess.run(command, capture_output=True, check=True, timeout=20)
    return json.loads(result.stdout)["passages"]


def read_completion(response, on_content):
    # The local worker sends one JSON data line per event, or [DONE].
    parts = []
    finish = None
    remaining = MAX_MODEL_STREAM
    data = None
    while remaining:
        line = response.readline(min(MAX_MODEL_LINE, remaining))
        remaining -= len(line)
        if not line.endswith(b"\n"):
            break
        line = line.rstrip(b"\r\n")
        if line.startswith(b":") and data is None:
            continue
        if line.startswith(b"data: ") and data is None:
            data = line[6:]
            continue
        if line:
            raise ValueError("invalid model stream event")
        if data is None:
            continue
        if data == b"[DONE]" and finish is not None:
            return "".join(parts), finish
        event = json.loads(data)
        data = None
        if finish is not None or not isinstance(event, dict) or "error" in event:
            raise ValueError("unexpected model stream event")
        choice, = event["choices"]
        if not isinstance(choice, dict) or choice["index"] != 0:
            raise ValueError("unexpected model stream choice")
        delta = choice["delta"]
        if not isinstance(delta, dict):
            raise ValueError("invalid model stream delta")
        content = delta.get("content")
        if content is not None:
            if not isinstance(content, str):
                raise ValueError("invalid model stream content")
            if content:
                on_content()
                parts.append(content)
        finish = choice["finish_reason"]
        if finish is not None and finish not in ("stop", "length", "tool_calls"):
            raise ValueError("invalid model finish reason")
    raise ValueError("incomplete or oversized model stream")


def generate(path, question, passages):
    body = chat_request_body(question, passages)
    body.update(stream=True, sse_ping_interval=-1)
    connection = UnixHTTP(path, timeout=MODEL_FIRST_REPLY_SECONDS)
    started = time.monotonic()
    limit = started + MODEL_TOTAL_SECONDS
    deadline = min(limit, started + MODEL_FIRST_REPLY_SECONDS)
    progress = Condition()
    finished = False
    watchdog = None
    try:
        connection.connect()
        sock = connection.sock

        def wait_for_deadline():
            # Interrupt a read even if the worker sends a trickle of bytes.
            with progress:
                while not finished:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        try:
                            sock.shutdown(socket.SHUT_RDWR)
                        except OSError:
                            pass
                        return
                    progress.wait(remaining)

        def record_content():
            nonlocal deadline
            with progress:
                now = time.monotonic()
                if now >= deadline:
                    raise TimeoutError("model deadline expired")
                deadline = min(limit, now + MODEL_STREAM_IDLE_SECONDS)
                progress.notify()

        watchdog = Thread(target=wait_for_deadline, daemon=True)
        watchdog.start()
        connection.request(
            "POST", "/v1/chat/completions",
            json.dumps(body),
            {"Content-Type": "application/json"},
        )
        with connection.getresponse() as response:
            if response.status != 200 or response.headers.get_content_type() != "text/event-stream":
                raise ValueError("model unavailable")
            raw, finish = read_completion(response, record_content)
        with progress:
            if time.monotonic() >= deadline:
                raise TimeoutError("model deadline expired")
        if finish != "stop":
            return UNKNOWN
        return grounded_reply(raw, len(passages))
    finally:
        connection.close()
        if watchdog is not None:
            with progress:
                finished = True
                progress.notify()
            watchdog.join()


def render(answer, passages):
    source = SOURCE.search(answer)
    cited = int(source[1]) if source else 0
    if not 1 <= cited <= len(passages):
        return [{"text": "[Wiki] " + (UNAVAILABLE if answer == UNAVAILABLE else UNKNOWN)}]
    answer = " ".join(SOURCE.sub("", answer).replace("**", "").replace("§", "").split())
    if not answer or answer.casefold() == UNKNOWN.casefold():
        return [{"text": "[Wiki] " + UNKNOWN}]
    passage = passages[cited - 1]
    url = "https://minecraft.wiki/w/" + quote(passage["title"].replace(" ", "_"), safe="")
    if passage["anchor"]:
        url += "#" + quote(passage["anchor"], safe="")
    return [
        {"text": "[Wiki] " + (answer[:357] + "..." if len(answer) > 360 else answer)},
        {"text": " [source]", "underlined": True, "color": "aqua",
         "click_event": {"action": "open_url", "url": url}},
    ]


def resources_available():
    memory = Path("/proc/meminfo").read_text()
    available = re.search(r"^MemAvailable:\s+(\d+) kB$", memory, re.M)
    temperature = int(Path("/sys/class/power_supply/battery/temp").read_text())
    return available and int(available[1]) >= 900 * 1024 and 0 <= temperature < 400


def answer_question(args, question, cache):
    try:
        if resources_available():
            passages = find_passages(args, question)
            if not passages:
                return render(UNKNOWN, [])
            # Include the exact evidence, revisions, and order. A refresh or
            # reparse can change the right answer or the cited passage.
            key = question, json.dumps(passages, sort_keys=True, ensure_ascii=False)
            if key in cache:
                cache.move_to_end(key)
                return cache[key]
            answer = exact_answer(question, passages)
            if answer is None:
                answer = generate(args.socket, question, passages)
            components = render(answer, passages)
            if len(components) > 1:
                cache[key] = components
                if len(cache) > REPLY_CACHE_SIZE:
                    cache.popitem(last=False)
            return components
    except (OSError, subprocess.SubprocessError, ValueError, TypeError, KeyError, IndexError,
            http.client.HTTPException, RecursionError):
        print("Wiki lookup failed", flush=True)
    return render(UNAVAILABLE, [])


def server_identity(path):
    try:
        pid = int(path.read_text().strip())
        if pid > 1:
            proc = Path(f"/proc/{pid}/stat").read_text().rpartition(")")[2].split()
            return (pid, proc[19]) if proc[0] != "Z" else None
    except (OSError, ValueError, IndexError):
        pass
    return None


def reply(path, player, components):
    if not re.fullmatch(r"[A-Za-z0-9_]{1,16}", player):
        raise ValueError("invalid player name")
    command = ("tellraw " + player + " " + json.dumps(components, ensure_ascii=True) + "\n").encode()
    fd = os.open(path, os.O_WRONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        info = os.fstat(fd)
        if not stat.S_ISFIFO(info.st_mode) or info.st_uid != os.geteuid():
            raise ValueError("unsafe Minecraft console")
        if len(command) > os.fpathconf(fd, "PC_PIPE_BUF"):
            raise ValueError("Minecraft reply exceeds atomic FIFO write size")
        os.write(fd, command)
    finally:
        os.close(fd)


def run(args):
    tail = LogTail(args.log)
    cache = OrderedDict()
    pending = None
    next_request = 0
    read_fd, write_fd = os.pipe2(os.O_NONBLOCK | os.O_CLOEXEC)
    log_fd = watch_log_directory(args.log)
    poll = select.poll()
    poll.register(read_fd, select.POLLIN)
    if log_fd is not None:
        poll.register(log_fd, select.POLLIN)

    def wake_on_reply(_):
        try:
            os.write(write_fd, b"\x00")
        except BlockingIOError:
            pass

    try:
        with ThreadPoolExecutor(max_workers=1) as worker:
            while True:
                if pending and pending.done():
                    if server == server_identity(args.pid_file):
                        try:
                            reply(args.console, player, pending.result())
                        except (OSError, ValueError):
                            print("Wiki reply could not reach the Minecraft console", flush=True)
                    pending = None
                try:
                    for line in tail.read():
                        if pending or time.monotonic() < next_request:
                            break
                        match = CHAT.fullmatch(line)
                        if match and (server := server_identity(args.pid_file)):
                            player, question = match.groups()
                            question = question.strip()
                            if question and len(question) <= 256:
                                pending = worker.submit(answer_question, args, question, cache)
                                pending.add_done_callback(wake_on_reply)
                                next_request = time.monotonic() + 30
                except (OSError, ValueError):
                    pass
                for fd, event in poll.poll(250):
                    if event & select.POLLIN:
                        try:
                            os.read(fd, 16384)
                        except BlockingIOError:
                            pass
    finally:
        if log_fd is not None:
            os.close(log_fd)
        os.close(read_fd)
        os.close(write_fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wiki", default="/opt/n20-wiki/n20-wiki")
    parser.add_argument("--cache", default="/var/lib/n20-wiki/cache")
    parser.add_argument("--socket", default="/run/n20-wiki/model/llama.sock")
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--log", type=Path, default=Path("/var/log/minecraft.log"))
    parser.add_argument("--pid-file", type=Path, default=Path("/run/minecraft.pid"))
    parser.add_argument("--console", type=Path, default=Path("/run/minecraft-console"))
    run(parser.parse_args())


if __name__ == "__main__":
    main()
