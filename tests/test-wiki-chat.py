"""Exercise the chat boundary with temporary files and a private FIFO."""

from collections import OrderedDict, deque
from contextlib import contextmanager
import io
import json
import os
from pathlib import Path
import socket
import sys
import tempfile
from threading import Event, Thread
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "runtime/usr/local/lib/n20"))
from wiki_answer import UNKNOWN, grounded_reply
from wiki_chat import CHAT, LogTail, RequestProgress, accept_chat, answer_question, generate, load_history, read_completion, render, reply, restore_handoff, save_history, start_queued, write_dashboard_status
from wiki_fast import exact_answer


def event(content=None, finish=None):
    delta = {} if content is None else {"content": content}
    body = {"choices": [{"index": 0, "delta": delta, "finish_reason": finish}]}
    return b"data: " + json.dumps(body).encode() + b"\n\n"


@contextmanager
def model_socket(handler, media_type=b"text/event-stream"):
    with tempfile.TemporaryDirectory() as tmp, socket.socket(socket.AF_UNIX) as server:
        path = str(Path(tmp) / "model.sock")
        server.bind(path)
        server.listen(1)
        server.settimeout(3)
        stop = Event()
        request = []

        def serve():
            client, _ = server.accept()
            with client, client.makefile("rb") as stream:
                client.settimeout(3)
                stream.readline()
                headers = {}
                for line in iter(stream.readline, b"\r\n"):
                    if not line:
                        raise ValueError("incomplete test request")
                    key, _, value = line.partition(b":")
                    headers[key.lower()] = value.strip()
                request.append(json.loads(stream.read(int(headers[b"content-length"]))))
                client.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: " + media_type + b"\r\n"
                               b"Connection: close\r\n\r\n")
                try:
                    handler(client, stop)
                except BrokenPipeError:
                    pass

        worker = Thread(target=serve)
        worker.start()
        try:
            yield path, request
        finally:
            stop.set()
            worker.join()


class WikiChatTests(unittest.TestCase):
    def test_long_model_prompts_get_more_time_without_changing_short_deadlines(self):
        import wiki_chat
        short = {'messages': [{'role': 'user', 'content': 'question'}], 'max_tokens': 140}
        long = {'messages': [{'role': 'user', 'content': 'x' * 3000}], 'max_tokens': 512}
        self.assertEqual(wiki_chat.model_deadlines(short), (wiki_chat.MODEL_FIRST_REPLY_SECONDS, wiki_chat.MODEL_TOTAL_SECONDS))
        first, total = wiki_chat.model_deadlines(long)
        self.assertGreater(first, wiki_chat.MODEL_FIRST_REPLY_SECONDS)
        self.assertGreater(total, wiki_chat.MODEL_TOTAL_SECONDS)

    def test_extended_context_only_for_saturated_comparisons(self):
        import wiki_chat
        def passage(title, text):
            return {'title': title, 'revision': 1, 'section': 'Usage', 'anchor': 'Usage', 'text': text}
        base = [passage(f'Page {i}', ('evidence ' * 50) + str(i)) for i in range(4)]
        other = passage('Second topic', 'Other comparison evidence.')
        question = 'Compare iron swords with diamond swords'
        with patch('wiki_chat.find_passages', side_effect=[[base[0]], [other]]) as find:
            expanded = wiki_chat.expand_wiki_passages(SimpleNamespace(), question, base)
        self.assertEqual(find.call_count, 2)
        self.assertEqual(expanded[:4], base)
        self.assertEqual(expanded[-1], other)
        self.assertLessEqual(len(json.dumps(wiki_chat.chat_request_body(question, expanded)['messages'], ensure_ascii=False).encode('utf-8')), 3000)
        with patch('wiki_chat.find_passages') as find:
            small = base[:2]
            self.assertIs(wiki_chat.expand_wiki_passages(SimpleNamespace(), question, small), small)
            self.assertIs(wiki_chat.expand_wiki_passages(SimpleNamespace(), 'How does a hopper work?', base), base)
            find.assert_not_called()

    def test_chat_acknowledges_and_processes_requests_in_order(self):
        from concurrent.futures import Future

        line = "[01:37:23] [Server thread/INFO]: <Alex> @wiki How does a hopper work?"
        with tempfile.TemporaryDirectory() as tmp:
            fifo = Path(tmp) / "console"
            os.mkfifo(fifo, 0o600)
            fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
            args = SimpleNamespace(console=fifo, pid_file=Path(tmp) / "pid")
            submitted = []
            requests = deque()

            class Worker:
                def submit(self, function, *arguments):
                    submitted.append((function, arguments))
                    return Future()

            worker = Worker()
            try:
                with patch("wiki_chat.server_identity", return_value=(42, "start")), \
                     patch("wiki_chat.time.monotonic", return_value=100):
                    accept_chat(line, args, requests, None, 0)
                    ack = os.read(fd, 4096)
                    self.assertTrue(ack.startswith(b"tellraw @a "))
                    self.assertEqual(json.loads(ack[len(b"tellraw @a "):]),
                                     [{"text": "[Wiki] Looking that up..."}])
                    second = line.replace("<Alex>", "<Steve>").replace("hopper", "furnace")
                    accept_chat(second, args, requests, None, 0)
                    ack = os.read(fd, 4096)
                    self.assertTrue(ack.startswith(b"tellraw @a "))
                    self.assertEqual(json.loads(ack[len(b"tellraw @a "):]),
                                     [{"text": "[Wiki] Your question is queued; I'll look it up soon."}])
                    self.assertEqual(len(submitted), 0)
                    future, player, server, next_request, question = start_queued(
                        args, requests, worker, {}, None, 0, lambda _: None)
                    self.assertEqual((player, server, next_request), ("Alex", (42, "start"), 130))
                    self.assertEqual(question, "How does a hopper work?")
                    self.assertEqual(len(submitted), 1)
                    self.assertIsNone(start_queued(args, requests, worker, {}, future, 0, lambda _: None))
                    self.assertIsNone(start_queued(args, requests, worker, {}, None, 130, lambda _: None))
                    self.assertIsNone(start_queued(args, requests, worker, {}, None, 0, lambda _: None, False))
                    accept_chat(line.replace("@wiki", "@wikipedia"), args, requests, None, 0)
                    self.assertEqual(len(submitted), 1)
                    self.assertEqual(len(requests), 1)
                with patch("wiki_chat.server_identity", return_value=(42, "start")), \
                     patch("wiki_chat.time.monotonic", return_value=130):
                    _, player, _, _, _ = start_queued(args, requests, worker, {}, None, 130, lambda _: None)
                    self.assertEqual(player, "Steve")
                    self.assertEqual(submitted[1][1][1], "How does a furnace work?")
                requests.append(((42, "old"), "Alex", "stale question"))
                with patch("wiki_chat.server_identity", return_value=(43, "new")), \
                     patch("wiki_chat.time.monotonic", return_value=200):
                    self.assertIsNone(start_queued(args, requests, worker, {}, None, 0, lambda _: None))
                    self.assertFalse(requests)
                with patch("wiki_chat.server_identity", return_value=(42, "start")), \
                     patch("wiki_chat.time.monotonic", return_value=300):
                    accept_chat(line, args, requests, None, 0, False)
                    self.assertIn(b"question is queued", os.read(fd, 4096))
                    self.assertIsNone(start_queued(args, requests, worker, {}, None, 0, lambda _: None, False))
                    self.assertEqual(len(requests), 1)
                    self.assertIsNotNone(start_queued(args, requests, worker, {}, None, 0, lambda _: None, True))
            finally:
                os.close(fd)

    def test_dashboard_status_is_atomic_bounded_and_sanitized(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "status"
            requests = deque([((1, "start"), "Alex", "First\tquestion\nnext") for _ in range(130)])
            write_dashboard_status(path, "ready", ("Steve", "Active question"), requests)
            lines = path.read_text().splitlines()
            self.assertIn("model\tready", lines)
            self.assertIn("count\t130", lines)
            self.assertIn("active\tSteve\tActive question", lines)
            self.assertEqual(sum(line.startswith("queue\t") for line in lines), 128)
            self.assertIn("queue\tAlex\tFirst question next", lines)
            self.assertEqual(len(requests), 130)
            self.assertEqual(sorted(p.name for p in Path(tmp).iterdir()), ["status"])

    def test_progress_reports_real_stages_and_streamed_characters(self):
        progress = RequestProgress()
        progress.report("retrieving", 0)
        progress.report("prefill", 0)
        progress.report("generating", 3)
        progress.report("generating", 4)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "status"
            write_dashboard_status(path, "ready", ("Alex", "Question?"), deque(), progress=progress)
            lines = path.read_text().splitlines()
            self.assertIn("phase\tgenerating", lines)
            self.assertIn("characters\t7", lines)
            self.assertTrue(any(line.startswith("elapsed\t") for line in lines))

    def test_recent_history_is_private_bounded_and_visible_in_status(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "requests.json"
            history = [{"player": "Alex", "question": f"Question {i}?", "answer": f"[Wiki] Reply {i}."}
                       for i in range(55)]
            save_history(path, history)
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            saved = load_history(path)
            self.assertEqual(len(saved), 50)
            self.assertEqual(saved[0]["question"], "Question 5?")
            status = Path(tmp) / "status"
            write_dashboard_status(status, "ready", None, deque(), history=saved)
            lines = status.read_text().splitlines()
            shown = [line for line in lines if line.startswith("history\t")]
            self.assertEqual(len(shown), 50)
            self.assertEqual(shown[0], "history\tAlex\tQuestion 54?\t[Wiki] Reply 54.")

    def test_stream_progress_counts_content_not_protocol_events(self):
        stream = io.BytesIO(event("abc") + event("de") + event(finish="stop") + b"data: [DONE]\n\n")
        counts = []
        text, finish = read_completion(stream, lambda: None, counts.append)
        self.assertEqual((text, finish, counts), ("abcde", "stop", [3, 2]))

    def test_resource_guard_uses_swap_only_when_available(self):
        import wiki_chat
        original = Path.read_text

        def fixture(path, *args, **kwargs):
            if str(path) == "/proc/meminfo":
                return "MemAvailable: 220000 kB\nSwapFree: 1000000 kB\n"
            if str(path) == "/sys/class/power_supply/battery/temp":
                return "280\n"
            return original(path, *args, **kwargs)

        with patch.object(Path, "read_text", fixture):
            self.assertTrue(wiki_chat.resources_available())
            self.assertFalse(wiki_chat.resources_available(3670016))
        def no_swap(path, *args, **kwargs):
            value = fixture(path, *args, **kwargs)
            return value.replace("SwapFree: 1000000 kB", "SwapFree: 0 kB")
        with patch.object(Path, "read_text", no_swap):
            self.assertFalse(wiki_chat.resources_available())

    def test_handoff_restores_queue_and_log_position(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "handoff.json"
            path.write_text(json.dumps({"version": 1, "queue": [[[42, "start"], "Alex", "A question?"]],
                                        "log": {"dev": 1, "ino": 2, "offset": 17}}))
            requests, resume = restore_handoff(path)
            self.assertEqual(list(requests), [((42, "start"), "Alex", "A question?")])
            self.assertEqual(resume, ((1, 2), 17))
            self.assertFalse(path.exists())
            self.assertEqual(restore_handoff(path), (deque(), None))
            target = Path(tmp) / "target"
            target.write_text("private")
            path.symlink_to(target)
            with self.assertRaises(OSError):
                restore_handoff(path)
            self.assertEqual(target.read_text(), "private")

    @unittest.skipUnless(hasattr(os, "fork"), "requires a Unix FIFO and fork")
    def test_bridge_queues_overlapping_chat_and_delivers_both_answers(self):
        import multiprocessing
        import select
        import wiki_chat

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            log = root / "server.log"
            log.write_text("")
            fifo = root / "console"
            os.mkfifo(fifo, 0o600)
            fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
            args = SimpleNamespace(log=log, console=fifo, pid_file=root / "pid", socket=root / "model.sock",
                                   status_file=root / "status", history_file=root / "history.json")
            context = multiprocessing.get_context("fork")
            ready = root / "ready"
            release = root / "release"
            original_tail = wiki_chat.LogTail

            class SignallingTail(original_tail):
                def read(self):
                    lines = super().read()
                    ready.touch()
                    return lines

            def fake_answer(_args, question, _cache, _report=None):
                if _report:
                    _report("retrieving", 0)
                deadline = time.monotonic() + 3
                while not release.exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                return [{"text": "[Wiki] Answer: " + question}]

            def bridge():
                with patch("wiki_chat.LogTail", SignallingTail), \
                     patch("wiki_chat.server_identity", return_value=(42, "start")), \
                     patch("wiki_chat.answer_question", fake_answer), \
                     patch("wiki_chat.model_health", return_value="ready"), \
                     patch("wiki_chat.resources_available", return_value=True), \
                     patch("wiki_chat.REQUEST_INTERVAL_SECONDS", 0.05):
                    wiki_chat.run(args)

            process = context.Process(target=bridge)
            process.start()
            try:
                deadline = time.monotonic() + 3
                while not ready.exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue(ready.exists())
                with log.open("a") as stream:
                    stream.write("[01:37:23] [Server thread/INFO]: <Alex> @wiki First?\n")
                    stream.write("[01:37:24] [Server thread/INFO]: <Steve> @wiki Second?\n")
                buffer = b""
                messages = []

                def receive(count):
                    nonlocal buffer
                    deadline = time.monotonic() + 4
                    while len(messages) < count and time.monotonic() < deadline:
                        if select.select([fd], [], [], 0.1)[0]:
                            buffer += os.read(fd, 4096)
                            lines = buffer.split(b"\n")
                            buffer = lines.pop()
                            messages.extend(lines)
                    self.assertGreaterEqual(len(messages), count)

                receive(2)
                self.assertIn(b"Looking that up", messages[0])
                self.assertIn(b"question is queued", messages[1])
                deadline = time.monotonic() + 3
                while time.monotonic() < deadline:
                    if args.status_file.exists() and "phase\tretrieving" in args.status_file.read_text():
                        break
                    time.sleep(0.02)
                self.assertIn("phase\tretrieving", args.status_file.read_text())
                release.touch()
                receive(4)
                self.assertTrue(messages[2].startswith(b"tellraw @a "))
                self.assertIn(b"Answer: First?", messages[2])
                self.assertTrue(messages[3].startswith(b"tellraw @a "))
                self.assertIn(b"Answer: Second?", messages[3])
                deadline = time.monotonic() + 3
                while time.monotonic() < deadline:
                    if args.history_file.exists() and len(load_history(args.history_file)) == 2:
                        break
                    time.sleep(0.02)
                self.assertEqual([item["question"] for item in load_history(args.history_file)], ["First?", "Second?"])
            finally:
                release.touch()
                process.terminate()
                process.join(5)
                os.close(fd)

    def test_complete_stream_can_finish_after_the_initial_deadline(self):
        def serve(client, stop):
            client.sendall(event("Support: [1] Relevant fact."))
            stop.wait(0.27)
            client.sendall(event("\nAnswer: Three"))
            stop.wait(0.27)
            client.sendall(event(".") + event(finish="stop") + b"data: [DONE]\n\n")

        passage = {"title": "Example", "section": "Crafting", "text": "Relevant fact."}
        with model_socket(serve) as (path, requests), \
                patch("wiki_chat.MODEL_FIRST_REPLY_SECONDS", 0.4), \
                patch("wiki_chat.MODEL_STREAM_IDLE_SECONDS", 0.4):
            self.assertEqual(generate(path, "How many?", [passage]), "Three. [1]")
            self.assertIs(requests[0]["stream"], True)
            self.assertEqual(requests[0]["max_tokens"], 140)
            self.assertNotIn("stop", requests[0])

    def test_blank_line_before_answer_does_not_stop_generation(self):
        def serve(client, stop):
            client.sendall(event("Support: [1]\nObserver signal strength is 15.\n\n"))
            client.sendall(event("Answer: 15") + event(finish="stop") + b"data: [DONE]\n\n")

        passage = {"title": "Observer", "section": "Behavior", "text": "Signal strength 15."}
        with model_socket(serve) as (path, requests):
            self.assertEqual(generate(path, "What signal strength?", [passage]), "15 [1]")
            self.assertNotIn("stop", requests[0])

    def test_stream_stalls_and_trickles_are_cancelled_without_a_reply(self):
        answer = event("Support: [1] Fact.\nAnswer: Three.")
        for mode in ("first content", "decode", "total"):
            def serve(client, stop):
                for _ in range(40):
                    if stop.wait(0.025):
                        return
                    client.sendall(answer if mode == "total" else b": ping\n\n")
                    if mode == "decode":
                        client.sendall(answer)
                        if stop.wait(1):
                            return
                        break
                client.sendall(event(finish="stop") + b"data: [DONE]\n\n")

            with self.subTest(mode=mode), model_socket(serve) as (path, _), \
                    patch("wiki_chat.MODEL_FIRST_REPLY_SECONDS", 0.18), \
                    patch("wiki_chat.MODEL_STREAM_IDLE_SECONDS", 0.18), \
                    patch("wiki_chat.MODEL_TOTAL_SECONDS", 0.32):
                started = time.monotonic()
                with self.assertRaises((OSError, ValueError)):
                    generate(path, "How many?", [])
                self.assertLess(time.monotonic() - started, 0.8)

    def test_model_stream_requires_complete_bounded_events_and_a_terminal_result(self):
        partial = event("Support: [1] Fact.\nAnswer: Three.")
        end = b"data: [DONE]\n\n"
        for malformed in (
            partial + end,
            partial + event(finish="stop"),
            partial + event(finish="stop") + end[:-1],
            partial + b'data: {"error": "test"}\n\n' + end,
            partial + b'data: {"choices": [{"index": 0, "delta": []}]}\n\n',
            partial + b"data: " + b"x" * 8192 + b"\n\n",
            b": ping\n\n" * 35000 + partial + event(finish="stop") + end,
        ):
            with self.assertRaises(ValueError):
                read_completion(io.BytesIO(malformed), lambda: None)
        with model_socket(lambda client, _: client.sendall(partial + event(finish="length") + end)) as (path, _):
            self.assertEqual(generate(path, "How many?", []), UNKNOWN)
        with model_socket(lambda *_: None, b"application/json") as (path, _):
            with self.assertRaises(ValueError):
                generate(path, "How many?", [])

    def test_exact_recipe_answer_uses_current_evidence_without_the_model(self):
        args = SimpleNamespace(socket="model.sock")
        passage = {
            "title": "Lightning Rod", "section": "Crafting", "anchor": "Crafting", "revision": 1,
            "text": "Name: Lightning Rod; Ingredients: Copper Ingot; Crafting recipe: 3 × Copper Ingot",
        }
        alternate = dict(passage, text="Name: Waxed Lightning Rod; Ingredients: Lightning Rod + Honeycomb")
        question = "How many copper ingots make a lightning rod?"
        with patch("wiki_chat.resources_available", return_value=True), \
                patch("wiki_chat.find_passages", return_value=[alternate, passage]), \
                patch("wiki_chat.generate") as generate:
            cache = OrderedDict()
            self.assertIn("3 × copper ingot", answer_question(args, question, cache)[0]["text"])
            passage["text"] = passage["text"].replace("3 ×", "4 ×")
            passage["revision"] += 1
            self.assertIn("4 × copper ingot", answer_question(args, question, cache)[0]["text"])
            generate.assert_not_called()

    def test_exact_path_defers_conflicts_and_preserves_the_property_clause(self):
        first = {"title": "Example", "section": "Crafting", "text": "Crafting recipe: 2 × Iron Ingot"}
        second = dict(first, text="Crafting recipe: 3 × Iron Ingot")
        self.assertIsNone(exact_answer("How many iron ingots make an example?", [first, second]))
        conditional = {"title": "Observer", "section": "Behavior",
                       "text": "When it detects something, the observer emits a pulse at signal strength 15."}
        self.assertEqual(exact_answer("What strength is an observer output?", [conditional]),
                         conditional["text"] + " [1]")
        self.assertIsNone(exact_answer("What is the strength of the observer at my base?", [conditional]))

    def test_powered_hopper_push_requires_both_matching_section_passages(self):
        lock = {"title": "Hopper", "section": "Redstone signals", "anchor": "Redstone_signals",
                "revision": 42, "text": "When a hopper receives a redstone signal, all three of its functions stop: in this situation the hopper is locked/disabled."}
        stopped = dict(lock, text="While a locked hopper does not push or pull/collect items, it may still receive items from droppers.")
        question = "Can a powered hopper still push its own items into the chest it points at?"
        self.assertEqual(exact_answer(question, [lock, stopped]),
                         "No. A powered hopper is locked and does not push items. [2]")
        self.assertIsNone(exact_answer(question, [lock]))
        self.assertIsNone(exact_answer(question, [stopped]))
        self.assertIsNone(exact_answer(question, [lock, dict(stopped, revision=43)]))
        self.assertIsNone(exact_answer(question, [lock, dict(stopped, anchor='Other')]))
        self.assertIsNone(exact_answer(question, [lock, dict(stopped, text=stopped['text'] + ' Bedrock')]))
        self.assertIsNone(exact_answer("Can a powered hopper receive items from another hopper?", [lock, stopped]))
        self.assertIsNone(exact_answer("Can a powered hopper at my base push items into a chest?", [lock, stopped]))

    def test_signal_question_accepts_redstone_wording_without_losing_conditions(self):
        passage = {
            "title": "Observer", "section": "Behavior",
            "text": "When it detects something, the observer emits a redstone pulse "
                    "of strong power at signal strength 15 for 2 game ticks.",
        }
        answer = passage["text"] + " [1]"
        for question in (
            "What redstone signal strength does an observer output?",
            "What redstone power level does the observer emit?",
            "What signal strength does an observer output?",
        ):
            with self.subTest(question=question):
                self.assertEqual(exact_answer(question, [passage]), answer)
        for question in (
            "What redstone signal strength does the observer at my base output?",
            "What redstone signal strength does a different observer output?",
            "What redstone signal strength does an observer output right now?",
        ):
            with self.subTest(question=question):
                self.assertIsNone(exact_answer(question, [passage]))
        conflicting = dict(passage, text=passage["text"].replace("strength 15", "strength 7"))
        self.assertIsNone(exact_answer("What redstone signal strength does an observer output?",
                                       [passage, conflicting]))

    def test_recipe_count_distinguishes_materials_and_preserves_qualifiers(self):
        question = "How many stone do I need to make a blast furnace?"
        recipe = {"title": "Blast Furnace", "section": "Crafting",
                  "text": "Ingredients: Iron Ingot + Furnace + Smooth Stone; "
                          "Crafting recipe: 5 × Iron Ingot, 1 × Furnace, 3 × Smooth Stone"}
        self.assertIsNone(exact_answer(question, [recipe]))
        with patch("wiki_chat.resources_available", return_value=True), \
                patch("wiki_chat.find_passages", return_value=[recipe]), \
                patch("wiki_chat.generate", return_value=UNKNOWN) as generate:
            answer_question(SimpleNamespace(socket="model.sock"), question, OrderedDict())
            generate.assert_called_once_with("model.sock", question, [recipe])
        question = question.replace("stone", "smooth stone")
        self.assertEqual(exact_answer(question, [recipe]), "Recipe uses 3 × smooth stone. [1]")
        bedrock = dict(recipe, text=recipe["text"] + "; Notes: Bedrock Edition only.")
        self.assertIsNone(exact_answer(question, [bedrock]))
        self.assertIsNone(exact_answer(question, [recipe, bedrock]))
        notes = "The stone must occupy the bottom row."
        qualified = dict(recipe, text=recipe["text"] + "; Notes: " + notes)
        self.assertEqual(exact_answer(question, [qualified]), f"Recipe uses 3 × smooth stone. {notes} [1]")
        self.assertIsNone(exact_answer(question, [recipe, qualified]))

    def test_exact_property_does_not_substitute_a_variant_with_the_same_name_suffix(self):
        question = "What light level does a normal torch emit?"
        variant = {"title": "Torch", "section": "Light", "text": "A soul torch emits a light level of 10."}
        ordinary = dict(variant, text="Torches give off a light level of 14.")
        self.assertIsNone(exact_answer(question, [variant]))
        self.assertEqual(exact_answer(question, [variant, ordinary]), ordinary["text"] + " [2]")

    def test_fuel_answer_requires_single_fuel_and_explicit_yield(self):
        question = "How many items can one dried kelp block smelt?"
        first = {"title": "Dried Kelp Block", "section": "Fuel",
                 "text": "Dried kelp blocks can be used as fuel in furnaces. It is able to smelt 20 items."}
        net = dict(first, text="Its net fuel value is only 11 items.")
        conditional = dict(first, text="A dried kelp block can smelt 20 items if it is prepared first.")
        conflict = dict(first, text="A dried kelp block can smelt 10 items.")
        self.assertEqual(exact_answer(question, [net, first]), "It can smelt up to 20 items. [2]")
        self.assertEqual(exact_answer(question.replace("one", "a single"), [first]),
                         "It can smelt up to 20 items. [1]")
        self.assertIsNone(exact_answer(question, [net, conditional]))
        self.assertIsNone(exact_answer(question, [first, conflict]))
        self.assertIsNone(exact_answer("How many items can two dried kelp blocks smelt?", [first]))

    def test_recipe_list_requires_complete_counts_and_preserves_notes(self):
        question = "What ingredients do I need to craft a dispenser?"
        counts = "7 × Cobblestone, 1 × Bow, 1 × Redstone Dust"
        ingredients = "Ingredients: Cobblestone + Bow + Redstone Dust; Crafting recipe: "
        notes = "The bow can be of any durability."
        recipe = {"title": "Dispenser", "section": "Crafting", "text": ingredients + counts + "; Notes: " + notes}
        variant = dict(recipe, text="Name: Dropper; " + ingredients + "7 × Cobblestone, 1 × Redstone Dust")
        incomplete = dict(recipe, text=ingredients + "7 × Cobblestone (fixed ingredient counts; other slots vary)")
        conditional = dict(recipe, text=recipe["text"].replace(" + Bow + ", " + Bow (only when damaged) + "))
        self.assertEqual(exact_answer(question, [variant, recipe]), f"Recipe ingredients: {counts}. {notes} [2]")
        conflicting = dict(recipe, text=recipe["text"].replace("7 × Cobblestone", "6 × Cobblestone"))
        for competing in (incomplete, conditional, conflicting):
            self.assertIsNone(exact_answer(question, [recipe, competing]))
        self.assertIsNone(exact_answer(question, [variant]))
        self.assertIsNone(exact_answer("What ingredients did I use to craft a dispenser?", [recipe]))

    def test_overview_field_preserves_qualifiers_and_rejects_live_state(self):
        anvil = {"title": "Anvil", "section": "Overview", "text": "Stackable: Yes (64), same damage state only"}
        conflict = dict(anvil, text="Stackable: Yes (16)")
        self.assertEqual(exact_answer("Are anvils stackable?", [anvil]), anvil["text"] + " [1]")
        self.assertIsNone(exact_answer("Are anvils stackable?", [anvil, conflict]))
        self.assertIsNone(exact_answer("Are anvils stackable?", [dict(anvil, title="Chipped Anvil")]))
        self.assertIsNone(exact_answer("Are anvils stackable?", [dict(anvil, text="Stackable: Yes (64) (Bedrock)")]))
        box = dict(anvil, title="Shulker Box", text="Stackable: No")
        self.assertEqual(exact_answer("Are shulker boxes stackable?", [box]), "Stackable: No [1]")
        shield = dict(anvil, title="Shield", text="Durability: 336")
        self.assertEqual(exact_answer("How much durability does a new shield have?", [shield]), "Durability: 336 [1]")
        self.assertIsNone(exact_answer("How much durability does my shield have?", [shield]))
        self.assertIsNone(exact_answer("What is the durability of the shield?", [shield]))

    def test_capacity_answer_requires_exact_container_and_direct_count(self):
        question = "How many inventory slots are inside an ender chest?"
        capacity = {"title": "Ender Chest", "section": "Usage",
                    "text": "Ender chests contain 27 slots for storage and they cannot be combined into double chests."}
        wrong_subject = dict(capacity, text="A double chest contains 54 slots for storage.")
        indirect = dict(capacity, text="Adding shulker boxes to an ender chest increases storage to 729 slots.")
        conditional = dict(capacity, text="With a server mod, an ender chest contains 54 slots for storage.")
        conflict = dict(capacity, text="Ender chests contain 54 slots for storage.")
        self.assertEqual(exact_answer(question, [wrong_subject, indirect, capacity]), capacity["text"] + " [3]")
        self.assertIsNone(exact_answer(question, [wrong_subject, indirect, conditional]))
        self.assertIsNone(exact_answer(question, [capacity, conflict]))
        self.assertIsNone(exact_answer("How many inventory slots are free inside an ender chest?", [capacity]))

    def test_fuel_table_requires_matching_fuel_and_whole_items(self):
        question = "How many items can one blaze rod smelt?"
        row = {"title": "Smelting", "section": "Fuel",
               "text": "Fuel: Blaze Rod; Burning time (seconds): 120 2:00 2400 ticks; "
                       "Number of operations per fuel: 12; Number of operations per stack (64) of fuel: 768; "
                       "Seconds per log: None used; Seconds per bamboo: None used; "
                       "Amount needed to smelt a stack (64) of items: 5 1⁄3"}
        wrong_fuel = dict(row, text=row["text"].replace("Blaze Rod", "Breeze Rod"))
        fractional = dict(row, text=row["text"].replace("per fuel: 12", "per fuel: 1.5"))
        note = dict(row, text=row["text"] + "; Notes: This yield applies only to wet fuel.")
        qualified = dict(row, text=row["text"].replace("768;", "768 (with fuel upgrade);"))
        conflict = dict(row, title="Blaze Rod", text="A blaze rod can smelt 10 items.")
        self.assertEqual(exact_answer(question, [wrong_fuel, row]), "It can smelt up to 12 items. [2]")
        for unsupported in (wrong_fuel, fractional, note, qualified):
            self.assertIsNone(exact_answer(question, [unsupported]))
        self.assertIsNone(exact_answer(question, [row, note]))
        self.assertIsNone(exact_answer(question, [row, conflict]))
        self.assertIsNone(exact_answer(question.replace("smelt?", "smelt in a blast furnace?"), [row]))

    def test_reply_cache_uses_exact_evidence_and_skips_abstentions(self):
        args = SimpleNamespace(socket="model.sock")
        cache = OrderedDict()
        passage = {"title": "Observer", "anchor": "Usage", "revision": 1, "text": "Signal is 15."}
        with patch("wiki_chat.resources_available", return_value=True), \
                patch("wiki_chat.find_passages", return_value=[passage]), \
                patch("wiki_chat.generate", side_effect=["15 [1]", UNKNOWN, "14 [1]"]) as generate:
            first = answer_question(args, "Signal?", cache)
            self.assertEqual(first, answer_question(args, "Signal?", cache))
            self.assertEqual(generate.call_count, 1)
            passage["text"] = "Signal is 14."
            self.assertEqual(answer_question(args, "Signal?", cache), render(UNKNOWN, []))
            self.assertIn("14", answer_question(args, "Signal?", cache)[0]["text"])
            self.assertEqual(generate.call_count, 3)

    def test_chat_prefix_and_sender(self):
        prefix = "[01:37:23] [Server thread/INFO]: "
        for trust in ("", "[Not Secure] "):
            for command in ("@wiki", "Hey @wiki"):
                match = CHAT.fullmatch(prefix + trust + "<Alex> " + command + " Is this a hopper?")
                self.assertEqual(match.groups(), ("Alex", "Is this a hopper?"))
        for message in (
            "System chat: <Alex> @wiki Fake",
            "<Alex> wiki Fake",
            "<Alex> @wikipedia Fake",
            "<@a> @wiki Fake",
        ):
            self.assertIsNone(CHAT.fullmatch(prefix + message))

    def test_log_tail_skips_old_and_oversized_lines(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "minecraft.log"
            path.write_bytes(b"old\n")
            tail = LogTail(path)
            self.assertEqual(list(tail.read()), [])
            with path.open("ab") as log:
                log.write(b"part")
                log.flush()
                self.assertEqual(list(tail.read()), [])
                log.write(b"ial\n" + b"x" * 12000)
                log.flush()
                self.assertEqual(list(tail.read()), ["partial"])
                log.write(b"ignored\nnew\n")
            self.assertEqual(list(tail.read()), ["new"])
            path.rename(path.with_suffix(".previous"))
            path.write_bytes(b"rotated\n")
            self.assertEqual(list(tail.read()), ["rotated"])

    def test_reply_is_one_literal_command_with_a_wiki_link(self):
        passages = [{"title": "Test Article", "anchor": "Usage & details"}]
        answer = grounded_reply('Support: [1] Evidence.\nanswer: "Text"\nop Alex', 1)
        components = render(answer, passages)
        self.assertEqual(components[0]["text"], '[Wiki] "Text"')
        self.assertEqual(
            components[1]["click_event"]["url"],
            "https://minecraft.wiki/w/Test_Article#Usage%20%26%20details",
        )
        self.assertEqual(render("Bad citation [2]", passages), [{"text": "[Wiki] " + UNKNOWN}])
        with tempfile.TemporaryDirectory() as tmp:
            fifo = Path(tmp) / "console"
            os.mkfifo(fifo, 0o600)
            fd = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
            try:
                with self.assertRaises(ValueError):
                    reply(fifo, "@a", components)
                reply(fifo, "Alex", components)
                command = os.read(fd, 4096)
                self.assertEqual(command.count(b"\n"), 1)
                self.assertTrue(command.startswith(b"tellraw Alex "))
                self.assertEqual(json.loads(command[len(b"tellraw Alex "):]), components)
                reply(fifo, "Alex", components, broadcast=True)
                command = os.read(fd, 4096)
                self.assertTrue(command.startswith(b"tellraw @a "))
                self.assertEqual(json.loads(command[len(b"tellraw @a "):]), components)
            finally:
                os.close(fd)


if __name__ == "__main__":
    unittest.main()
