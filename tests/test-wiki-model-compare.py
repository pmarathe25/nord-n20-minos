"""Isolated checks for matched Wiki-model benchmark scoring and evidence identity."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("wiki_model_compare", ROOT / "wiki/experiments/n20-wiki-model-compare.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class PairedEvaluation(unittest.TestCase):
    def test_matched_answers_scores_and_evidence_rejection(self):
        passage = {"title": "Observer", "section": "Behavior", "anchor": "Behavior", "text": "Strength 15."}
        fixture = {"version": 1, "items": [
            {"case": {"id": "fact", "question": "How strong?", "titles": ["Observer"], "expect": ["15"]},
             "passages": [passage]},
            {"case": {"id": "private", "question": "What is my signal now?"}, "passages": [passage]},
        ]}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture_file = root / "fixture.json"
            fixture_file.write_text(json.dumps(fixture), encoding="utf-8")
            before, after = root / "before.json", root / "after.json"
            answers = iter(["15 [1]", module.UNKNOWN, "12 [1]", module.UNKNOWN])
            def completed(_path, _question, _passages):
                answer = next(answers)
                event = {"choices": [{"index": 0, "delta": {"content": "Answer: " + answer}, "finish_reason": None}]}
                end = {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]}
                stream = ("data: " + json.dumps(event) + "\n\ndata: " + json.dumps(end) + "\n\ndata: [DONE]\n\n").encode()
                module.wiki_chat.read_completion(io.BytesIO(stream), lambda: None)
                return answer
            with patch.object(module, "exact_answer", return_value=None), \
                    patch.object(module, "generate", side_effect=completed), \
                    contextlib.redirect_stdout(io.StringIO()):
                module.run(types.SimpleNamespace(fixture=fixture_file, output=before, socket=root / "sock", arm="baseline"))
                module.run(types.SimpleNamespace(fixture=fixture_file, output=after, socket=root / "sock", arm="candidate"))
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                module.compare(types.SimpleNamespace(baseline=before, candidate=after))
            summary = json.loads(out.getvalue())
            self.assertEqual(summary["baseline_correct"], 2)
            self.assertEqual(summary["candidate_correct"], 1)
            self.assertEqual(summary["regressions"], ["fact"])
            modified = json.loads(after.read_text(encoding="utf-8"))
            modified["results"][0]["passages_sha256"] = "changed"
            after.write_text(json.dumps(modified), encoding="utf-8")
            with self.assertRaises(ValueError):
                module.compare(types.SimpleNamespace(baseline=before, candidate=after))

    def test_unavailable_private_answer_is_not_an_abstention(self):
        result = module.grade({"id": "private", "question": "Where am I?"}, module.UNAVAILABLE, [])
        self.assertFalse(result["correct"])
        self.assertFalse(result["abstention"])

    def test_stream_capture_distinguishes_explicit_unknown_from_truncation(self):
        passage = {"title": "Observer", "section": "Behavior", "anchor": "Behavior", "text": "Strength 15."}
        fixture = {"version": 1, "items": [
            {"case": {"id": "private", "question": "Where am I?"}, "passages": [passage]},
        ]}
        event = {"choices": [{"index": 0, "delta": {"content": "Answer: " + module.UNKNOWN},
                              "finish_reason": None}]}
        end = {"choices": [{"index": 0, "delta": {}, "finish_reason": "length"}]}
        stream = ("data: " + json.dumps(event) + "\n\ndata: " + json.dumps(end) + "\n\ndata: [DONE]\n\n").encode()

        def truncated(_path, _question, _passages):
            _raw, finish = module.wiki_chat.read_completion(io.BytesIO(stream), lambda: None)
            self.assertEqual(finish, "length")
            return module.UNKNOWN

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, output = root / "fixture.json", root / "result.json"
            source.write_text(json.dumps(fixture), encoding="utf-8")
            with patch.object(module, "exact_answer", return_value=None), \
                    patch.object(module, "generate", side_effect=truncated), \
                    contextlib.redirect_stdout(io.StringIO()):
                module.run(types.SimpleNamespace(fixture=source, output=output, socket=root / "sock", arm="test"))
            item = json.loads(output.read_text(encoding="utf-8"))["results"][0]
            self.assertFalse(item["correct"])
            self.assertTrue(item["explicit_unknown"])
            self.assertEqual(item["finish_reason"], "length")


if __name__ == "__main__":
    unittest.main()
