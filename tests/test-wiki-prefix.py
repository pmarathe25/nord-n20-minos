#!/usr/bin/env python3
"""Exercise prefix identity and permissions in a disposable root-owned tree."""

import hashlib
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
from contextlib import redirect_stderr, redirect_stdout

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "runtime/usr/local/lib/n20"))
import wiki_prefix


@unittest.skipUnless(getattr(os, "geteuid", lambda: -1)() == 0, "requires root")
class PrefixTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir="/root")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bundle = self.root / "bundle"
        self.android = self.root / "android"
        self.firmware = self.root / "firmware"
        self.backend = self.bundle / "opencl"
        self.impl = self.backend / "bin/libllama-server-impl.so"
        self.model = self.bundle / "model.gguf"
        self.driver = self.backend / "vendor/libOpenCL.so"
        self.boot = self.root / "boot_id"
        self.launch = ["N20_KEEP_SYSTEM_PREFIX=1", "--ctx-size", "1536"]

        for path in (self.model, self.impl, self.driver, self.backend / "cl-compat.so",
                     self.backend / "bin/llama-server", self.android / "linker64",
                     self.boot, *(self.firmware / name for name in wiki_prefix.FIRMWARE)):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"fixture")
        (self.backend / "system").mkdir()
        self.impl.write_bytes(b"fixture\0" + b"".join(wiki_prefix.CAPABILITIES))
        (self.android / "ld-android.so").symlink_to("linker64")

    def key(self):
        return wiki_prefix.fingerprint(self.bundle, self.android, self.launch, self.firmware, (self.boot,))

    def test_identity_changes_with_worker_inputs(self):
        initial = self.key()
        self.assertEqual(len(initial), 64)
        self.assertEqual(self.key(), initial)
        for path in (self.model, self.impl, self.driver, self.android / "linker64",
                     self.firmware / wiki_prefix.FIRMWARE[0], self.boot):
            with self.subTest(path=path.name):
                original = path.read_bytes()
                path.write_bytes(original + b"changed")
                self.assertNotEqual(self.key(), initial)
                path.write_bytes(original)
        self.launch[-1] = "2048"
        self.assertNotEqual(self.key(), initial)

    def test_stock_shadow_and_unsafe_runtime_inputs_disable_disk_cache(self):
        implementation = self.impl.read_bytes()
        self.impl.write_bytes(b"stock")
        with self.assertRaisesRegex(ValueError, "lacks the disk prefix patch"):
            self.key()
        self.impl.write_bytes(implementation)
        shadow = self.driver.with_name(self.impl.name)
        shadow.write_bytes(implementation)
        with self.assertRaisesRegex(ValueError, "expected one Wiki server implementation"):
            self.key()
        shadow.unlink()
        self.driver.chmod(0o664)
        with self.assertRaisesRegex(ValueError, "unsafe Wiki runtime input"):
            self.key()
        self.driver.chmod(0o644)
        self.model.unlink()
        self.model.symlink_to(self.driver)
        with self.assertRaisesRegex(ValueError, "unsafe Wiki runtime input"):
            self.key()

    def test_cache_directory_is_private_and_existing_links_are_rejected(self):
        cache = self.root / "prefix"
        wiki_prefix.prepare(cache, 61124)
        wiki_prefix.prepare(cache, 61124)
        self.assertEqual(cache.stat().st_uid, 61124)
        self.assertEqual(cache.stat().st_mode & 0o777, 0o700)
        link = self.root / "link"
        link.symlink_to(cache, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "unsafe Wiki prefix cache directory"):
            wiki_prefix.prepare(link, 61124)
        cache.chmod(0o750)
        with self.assertRaisesRegex(ValueError, "unsafe Wiki prefix cache directory"):
            wiki_prefix.prepare(cache, 61124)

    def test_tuned_sources_are_trusted_and_bound_to_the_tuned_key(self):
        marker = self.root / "tuned"
        marker.touch()
        library = self.impl.with_name("libggml-opencl.so")
        library.write_bytes(b"stock")
        with self.assertRaisesRegex(ValueError, "lacks the tuning patch"):
            wiki_prefix.check_tuned(self.bundle, self.android, marker)
        options = (*wiki_prefix.TUNED_SOURCES, *wiki_prefix.TUNED_FLAGS)
        library.write_bytes(b"\0".join(option.encode() for option in options) + b"\0")
        kernels = self.bundle / "kernels"
        kernels.mkdir()
        for name in wiki_prefix.TUNED_SOURCES.values():
            (kernels / name).write_text("test source")
        wiki_prefix.check_tuned(self.bundle, self.android, marker)

        stock = self.key()
        option, name = next(iter(wiki_prefix.TUNED_SOURCES.items()))
        source = kernels / name
        self.launch.append(f"{option}={source}")
        initial = self.key()
        source.write_text("different source")
        self.assertNotEqual(self.key(), initial)
        self.launch.pop()
        self.assertEqual(self.key(), stock)

        source.chmod(0o664)
        with self.assertRaisesRegex(ValueError, "unsafe Wiki runtime input"):
            wiki_prefix.check_tuned(self.bundle, self.android, marker)
        source.chmod(0o644)
        shadow = self.driver.with_name(library.name)
        shadow.symlink_to(library)
        with self.assertRaisesRegex(ValueError, "shadowed Wiki OpenCL backend"):
            wiki_prefix.check_tuned(self.bundle, self.android, marker)

    def test_texture_requires_the_tested_checkpoint_and_fingerprints_both_sources(self):
        marker = self.root / "texture"
        marker.touch()
        shaders = self.root / "shaders-texture"
        library = self.impl.with_name("libggml-opencl.so")
        library.write_bytes(b"N20_Q2_TEXTURE_SOURCE\0")
        kernels = self.bundle / "kernels"
        kernels.mkdir()
        tuned = kernels / wiki_prefix.TUNED_SOURCES["N20_Q2_PREFILL_SOURCE"]
        texture = kernels / wiki_prefix.TEXTURE_SOURCES["N20_Q2_TEXTURE_SOURCE"]
        for source in (tuned, texture):
            source.write_text("original source")
        with self.assertRaisesRegex(ValueError, "tested 4B Q2_0 checkpoint"):
            wiki_prefix.check_texture(self.bundle, self.android, marker, shaders, 61124)
        self.assertFalse(shaders.exists())

        digest = hashlib.sha256(self.model.read_bytes()).hexdigest()
        with patch.object(wiki_prefix, "TEXTURE_MODEL_SHA256", digest):
            wiki_prefix.check_texture(self.bundle, self.android, marker, shaders, 61124)
            self.assertEqual(shaders.stat().st_uid, 61124)
            self.assertEqual(shaders.stat().st_mode & 0o777, 0o700)

        self.launch.append(f"N20_Q2_PREFILL_SOURCE={tuned}")
        tuned_key = self.key()
        self.launch.append(f"N20_Q2_TEXTURE_SOURCE={texture}")
        texture_key = self.key()
        self.assertNotEqual(tuned_key, texture_key)
        for source in (tuned, texture):
            source.write_text("different source")
            self.assertNotEqual(self.key(), texture_key)
            source.write_text("original source")
        self.launch[-1] = f"N20_Q2_TEXTURE_SOURCE={tuned}"
        with self.assertRaisesRegex(ValueError, "unexpected Wiki OpenCL source"):
            self.key()

    def test_combined_texture_prefix_reuses_one_verified_model_hash(self):
        marker = self.root / "texture"
        marker.touch()
        shaders = self.root / "shaders-texture"
        prefix = self.root / "prefix"
        library = self.impl.with_name("libggml-opencl.so")
        library.write_bytes(b"N20_Q2_TEXTURE_SOURCE\0")
        kernels = self.bundle / "kernels"
        kernels.mkdir()
        texture = kernels / wiki_prefix.TEXTURE_SOURCES["N20_Q2_TEXTURE_SOURCE"]
        texture.write_text("texture source")
        self.launch.append(f"N20_Q2_TEXTURE_SOURCE={texture}")
        expected = self.key()
        firmware_path = self.root / "firmware-path"
        firmware_path.write_text("/newroot/usr/lib/firmware\n")
        actual_fingerprint = wiki_prefix.fingerprint
        actual_file_digest = hashlib.file_digest
        model_hashes = []

        def fingerprint(bundle, android, launch, *, verified_model=None):
            return actual_fingerprint(bundle, android, launch, self.firmware, (self.boot,), verified_model)

        def file_digest(stream, algorithm):
            if Path(stream.name) == self.model:
                model_hashes.append(stream.name)
            return actual_file_digest(stream, algorithm)

        args = [str(self.bundle), str(self.android), str(marker), str(shaders), "61124",
                str(prefix), "--", *self.launch]
        model_hash = hashlib.sha256(self.model.read_bytes()).hexdigest()
        with patch.object(wiki_prefix, "TEXTURE_MODEL_SHA256", model_hash), \
                patch.object(wiki_prefix, "FIRMWARE_PATH", firmware_path), \
                patch.object(wiki_prefix, "fingerprint", side_effect=fingerprint), \
                patch.object(hashlib, "file_digest", side_effect=file_digest):
            for _ in range(2):
                output = io.StringIO()
                with redirect_stdout(output):
                    wiki_prefix.prepare_texture(args)
                self.assertEqual(output.getvalue().strip(), expected)
            self.assertEqual(len(model_hashes), 2)
            output = io.StringIO()
            with redirect_stdout(output):
                wiki_prefix.prepare_texture(args[:5])
            self.assertEqual(output.getvalue(), "")
            self.assertEqual(len(model_hashes), 3)
            verified = wiki_prefix.check_texture(self.bundle, self.android, marker, shaders, 61124)
            self.model.write_bytes(b"different model")
            with self.assertRaisesRegex(ValueError, "model changed after texture validation"):
                fingerprint(self.bundle, self.android, self.launch, verified_model=verified)
            with self.assertRaisesRegex(ValueError, "tested 4B Q2_0 checkpoint"):
                wiki_prefix.prepare_texture(args)
            self.model.write_bytes(b"fixture")
            self.impl.write_bytes(b"unpatched")
            output, error = io.StringIO(), io.StringIO()
            with redirect_stdout(output), redirect_stderr(error):
                wiki_prefix.prepare_texture(args)
            self.assertEqual(output.getvalue(), "")
            self.assertIn("disk prefix disabled", error.getvalue())

    def test_gdn_profile_requires_capable_backend_and_separate_identity(self):
        marker = self.root / "gdn"
        marker.touch()
        shaders = self.root / "shaders-gdn"
        library = self.impl.with_name("libggml-opencl.so")
        library.write_bytes(b"unpatched")
        with self.assertRaisesRegex(ValueError, "lacks the tuning patch"):
            wiki_prefix.check_gdn(self.bundle, self.android, marker, shaders, 61124)
        self.assertFalse(shaders.exists())

        library.write_bytes(b"N20_GDN_CPL\0")
        wiki_prefix.check_gdn(self.bundle, self.android, marker, shaders, 61124)
        self.assertEqual(shaders.stat().st_uid, 61124)
        self.assertEqual(shaders.stat().st_mode & 0o777, 0o700)
        original_key = self.key()
        self.launch.append("N20_GDN_CPL=2")
        self.assertNotEqual(self.key(), original_key)
        self.launch.pop()
        self.assertEqual(self.key(), original_key)

        marker.unlink()
        marker.symlink_to(self.model)
        with self.assertRaisesRegex(ValueError, "unsafe Wiki runtime input"):
            wiki_prefix.check_gdn(self.bundle, self.android, marker, shaders, 61124)


if __name__ == "__main__":
    unittest.main()
