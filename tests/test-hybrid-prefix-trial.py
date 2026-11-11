#!/usr/bin/env python3
"""Isolated checks for the scratch hybrid prefix-state experiment."""
import importlib.util
import json
import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("hybrid_prefix_trial", ROOT / "wiki/experiments/n20-hybrid-prefix-trial.py")
trial = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trial)


class PrefixTrialTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.runtime = self.root / "runtime"
        self.runtime.mkdir()
        self.dsp = self.runtime / "dsp"
        self.dsp.mkdir()
        self.files = {}
        for role in trial.ROLES:
            path = (self.dsp / "libn20_projection_skel.so" if role == "dsp-module" else
                    self.dsp / "fastrpc_shell_unsigned_3" if role == "dsp-shell" else
                    self.runtime / "libggml-opencl.so" if role == "backend" else self.root / role)
            path.write_bytes(role.encode())
            self.files[role] = path
        self.files["hybrid-runtime"].write_bytes(b"file:///libn20_projection_skel.so?\x00")
        for name in ("model", "binary", "runner", "reference", "prompt"):
            path = (self.runtime if name == "binary" else self.root) / name
            path.write_bytes(name.encode())
            self.files[name] = path
        (self.runtime / "dependency").write_bytes(b"library")
        self.work = self.root / "work"
        self.work.mkdir(mode=0o700)
        self.args = SimpleNamespace(
            artifact=[f"{role}={self.files[role]}" for role in sorted(trial.ROLES)],
            runtime_tree=[self.runtime], ubatch=256, model=self.files["model"],
            binary=self.files["binary"], runner=self.files["runner"],
            reference=self.files["reference"], prompt=self.files["prompt"], timeout=10,
            work_dir=self.work)
        self.env = {"LD_LIBRARY_PATH": str(self.runtime),
                    "ADSP_LIBRARY_PATH": str(self.dsp),
                    "N20_HYBRID_RUNTIME": str(self.files["hybrid-runtime"]),
                    "N20_HYBRID_ROW_SOURCE": str(self.files["row-shader"]),
                    "N20_HYBRID_AUX_SOURCE": str(self.files["aux-shader"]),
                    "N20_HYBRID_PAIR": "1"}

    def test_identity_tracks_firmware_kernels_libraries_environment_and_reference(self):
        baseline = trial.profile(self.args, self.env)
        for role in ("model", "backend", "hybrid-runtime", "dsp-module", "dsp-shell", "dsp-firmware", "gpu-firmware",
                     "gpu-driver", "row-shader", "aux-shader", "device-identity", "reference"):
            with self.subTest(role=role):
                path = self.files[role]
                original = path.read_bytes()
                path.write_bytes(original + b"-changed")
                self.assertNotEqual(trial.profile(self.args, self.env), baseline)
                path.write_bytes(original)
        path = self.runtime / "dependency"
        path.write_bytes(b"changed library")
        self.assertNotEqual(trial.profile(self.args, self.env), baseline)

        path.write_bytes(b"library")
        altered = dict(self.env, N20_HYBRID_PAIR="0")
        self.assertNotEqual(trial.profile(self.args, altered), baseline)
        self.args.ubatch = 128
        self.assertNotEqual(trial.profile(self.args, self.env), baseline)

    def test_profile_survives_manifest_json_round_trip(self):
        profile = trial.profile(self.args, self.env)
        self.assertEqual(json.loads(json.dumps(profile)), profile)

    def test_rejects_wrong_selected_module_and_uncovered_library_path(self):
        with self.assertRaisesRegex(ValueError, "DSP module"):
            trial.profile(self.args, dict(self.env, ADSP_LIBRARY_PATH=str(self.runtime)))
        with self.assertRaisesRegex(ValueError, "DSP module"):
            trial.profile(self.args, dict(self.env, DSP_LIBRARY_PATH=str(self.runtime)))
        with self.assertRaisesRegex(ValueError, "LD_LIBRARY_PATH"):
            trial.profile(self.args, dict(self.env, LD_LIBRARY_PATH=str(self.root)))
        with self.assertRaisesRegex(ValueError, "matching fingerprinted artifact"):
            trial.profile(self.args, dict(self.env, N20_HYBRID_RUNTIME=str(self.files["backend"])))
        escaped = str(self.runtime / "..")
        with self.assertRaisesRegex(ValueError, "LD_LIBRARY_PATH"):
            trial.profile(self.args, dict(self.env, LD_LIBRARY_PATH=escaped))
        with self.assertRaisesRegex(ValueError, "ADSP_LIBRARY_PATH"):
            trial.profile(self.args, dict(self.env, ADSP_LIBRARY_PATH=str(self.root)))
        with self.assertRaisesRegex(ValueError, "GGML_BACKEND_PATH"):
            trial.profile(self.args, dict(self.env, GGML_BACKEND_PATH=str(self.files["backend"])))
        shadow = self.runtime / "shadow"
        shadow.mkdir()
        (shadow / "libggml-opencl.so").write_bytes(b"different backend")
        with self.assertRaisesRegex(ValueError, "OpenCL backend"):
            trial.profile(self.args, dict(self.env, LD_LIBRARY_PATH=os.pathsep.join((str(shadow), str(self.runtime)))))

    @unittest.skipUnless(os.name == "posix", "requires POSIX symlinks")
    def test_digest_rejects_symlinked_parent(self):
        alias = self.root / "alias"
        alias.symlink_to(self.runtime, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "absolute regular non-symlink"):
            trial.digest_file(alias / "libggml-opencl.so")

    def test_module_uri_selects_the_matching_named_artifact(self):
        runtime = self.files["hybrid-runtime"]
        runtime.write_bytes(b"file:///libn20_projection_block_major_skel.so?\x00")
        with self.assertRaisesRegex(ValueError, "DSP module"):
            trial.profile(self.args, self.env)
        module = self.dsp / "libn20_projection_block_major_skel.so"
        module.write_bytes(b"block-major")
        self.args.artifact = [x for x in self.args.artifact if not x.startswith("dsp-module=")]
        self.args.artifact.append("dsp-module=" + str(module))
        trial.profile(self.args, self.env)
        runtime.write_bytes(b"file:///libn20_projection_skel.so?\x00file:///libn20_projection_block_major_skel.so?\x00")
        with self.assertRaisesRegex(ValueError, "ambiguous"):
            trial.profile(self.args, self.env)

    def test_changed_input_during_seed_is_not_published(self):
        cache = self.root / "cache"
        cache.mkdir(mode=0o700)
        expected = trial.profile(self.args, self.env)
        def changed(_args, _env, _operation, state):
            state.write_bytes(b"state")
            self.files["model"].write_bytes(b"changed")
            return {"prefix_tokens": 2}
        with patch.object(trial, "run", side_effect=changed):
            with self.assertRaisesRegex(ValueError, "changed while seeding"):
                trial.seed(self.args, self.env, cache, expected)
        self.assertFalse((cache / trial.STATE_NAME).exists())
        self.assertFalse((cache / trial.MANIFEST_NAME).exists())

    @unittest.skipUnless(os.name == "posix", "requires POSIX ownership and modes")
    def test_seeded_real_profile_can_be_reused_and_invalidates_on_change(self):
        cache = self.root / "cache"
        cache.mkdir(mode=0o700)
        expected = trial.profile(self.args, self.env)

        def save(_args, _env, operation, state):
            self.assertEqual(operation, "save")
            state.write_bytes(b"private model state")
            return {"prefix_tokens": 2}

        with patch.object(trial, "run", side_effect=save):
            trial.seed(self.args, self.env, cache, expected)
        self.assertTrue(trial.validate_cache(cache, trial.profile(self.args, self.env)))
        (self.runtime / "dependency").write_bytes(b"updated library")
        with self.assertRaisesRegex(ValueError, "stale prefix cache profile"):
            trial.validate_cache(cache, trial.profile(self.args, self.env))

    def test_changed_input_after_fresh_run_prevents_seed_and_restore(self):
        cache = self.root / "cache"
        cache.mkdir()
        with patch.object(trial, "profile", side_effect=({"version": 1}, {"version": 2})), \
             patch.object(trial, "validate_cache", return_value=False), \
             patch.object(trial, "run", return_value={"token_ids": "1", "logits_hash": "a"}) as worker, \
             patch.object(trial, "seed") as seed:
            with self.assertRaisesRegex(ValueError, "changed during fresh run"):
                trial.execute_trial(self.args, self.env, cache)
        worker.assert_called_once()
        seed.assert_not_called()

    def test_missing_or_duplicate_artifact_is_rejected(self):
        with self.assertRaises(ValueError):
            trial.artifacts(self.args.artifact[:-1])
        with self.assertRaises(ValueError):
            trial.artifacts(self.args.artifact + [self.args.artifact[0]])

    def test_partial_cache_is_rejected(self):
        cache = self.root / "cache"
        cache.mkdir()
        self.assertFalse(trial.validate_cache(cache, {}))
        (cache / trial.STATE_NAME).write_bytes(b"incomplete")
        with self.assertRaisesRegex(ValueError, "incomplete"):
            trial.validate_cache(cache, {})

    @unittest.skipUnless(os.name == "posix", "requires POSIX ownership and modes")
    def test_stale_corrupt_and_nonprivate_cache_are_rejected(self):
        cache = self.root / "cache"
        cache.mkdir(mode=0o700)
        state = cache / trial.STATE_NAME
        state.write_bytes(b"state")
        state.chmod(0o600)
        manifest = cache / trial.MANIFEST_NAME
        manifest.write_text(json.dumps({"version": 1, "profile": {"key": 1},
                                        "state_sha256": trial.digest_file(state)}))
        manifest.chmod(0o600)
        self.assertTrue(trial.validate_cache(cache, {"key": 1}))
        with self.assertRaisesRegex(ValueError, "stale"):
            trial.validate_cache(cache, {"key": 2})
        state.write_bytes(b"different")
        with self.assertRaisesRegex(ValueError, "corrupt"):
            trial.validate_cache(cache, {"key": 1})
        state.chmod(0o644)
        with self.assertRaisesRegex(ValueError, "unsafe"):
            trial.validate_cache(cache, {"key": 1})

    @unittest.skipUnless(os.name == "posix", "requires POSIX ownership and modes")
    def test_benchmark_result_requires_real_hybrid_dispatch(self):
        stdout = ("load_and_context_s=1.0 prompt_tokens=20 ubatch=256 sampling=host\n"
                  "run=1 first_token_s=2.0 decode_s=0 token_ids=42 logits_hash=abcd\n")
        def invoke(*_args, **kwargs):
            self.assertEqual(kwargs["cwd"], self.work)
            kwargs["stderr"].write("n20 hybrid: slots_attempted=2 projections_attempted=2 dispatched=2 fallbacks=0\n")
            kwargs["stderr"].write("n20 hybrid pair: attempted=1 reused=1\n")
            return SimpleNamespace(returncode=0, stdout=stdout)
        with patch.object(trial.subprocess, "run", side_effect=invoke):
            result = trial.run(self.args, self.env)
        self.assertEqual(result["hybrid_dispatched"], 2)
        self.assertEqual(result["pair_reused"], 1)
        self.assertEqual(result["logits_hash"], "abcd")
        def no_dispatch(*_args, **kwargs):
            kwargs["stderr"].write("n20 hybrid: slots_attempted=2 projections_attempted=2 dispatched=0 fallbacks=2\n")
            return SimpleNamespace(returncode=0, stdout=stdout)
        with patch.object(trial.subprocess, "run", side_effect=no_dispatch):
            with self.assertRaisesRegex(ValueError, "absent or fell back"):
                trial.run(self.args, self.env)
        (self.work / "libggml-opencl.so").write_bytes(b"unexpected")
        with self.assertRaisesRegex(ValueError, "not empty"):
            trial.run(self.args, self.env)


if __name__ == "__main__":
    unittest.main()
