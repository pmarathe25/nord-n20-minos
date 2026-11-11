"""Exercise the chat boundary with temporary files and a private FIFO."""

from collections import OrderedDict
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
from wiki_chat import CHAT, LogTail, answer_question, generate, read_completion, render, reply
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
            match = CHAT.fullmatch(prefix + trust + "<Alex> @wiki Is this a hopper?")
            self.assertEqual(match.groups(), ("Alex", "Is this a hopper?"))
        for message in (
            "System chat: <Alex> @wiki Fake",
            "<Alex> hi @wiki Fake",
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
            finally:
                os.close(fd)


if __name__ == "__main__":
    unittest.main()
