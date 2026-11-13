"""Test swap startup checks without activating swap or touching system state."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(getattr(os, "geteuid", lambda: -1)() == 0, "requires root-owned test files")
class SwapStartupTests(unittest.TestCase):
    def test_activation_requires_a_safe_preexisting_file(self):
        with tempfile.TemporaryDirectory(prefix="n20-swap-test-") as tmp:
            root = Path(tmp)
            directory = root / "n20"
            directory.mkdir(mode=0o700)
            swap = directory / "swapfile"
            swaps = root / "swaps"
            swaps.write_text("Filename Type Size Used Priority\n")
            swappiness = root / "swappiness"
            swappiness.write_text("60\n")
            called = root / "called"
            swapon = root / "swapon"
            swapon.write_text(f"#!/bin/sh\nprintf '%s' \"$1\" > '{called}'\n")
            swapon.chmod(0o755)
            script = (ROOT / "runtime/usr/local/sbin/n20-swap-start").read_text()
            script = script.replace("/var/lib/n20", str(directory))
            script = script.replace("/proc/swaps", str(swaps))
            script = script.replace("/proc/sys/vm/swappiness", str(swappiness))
            script = script.replace("/usr/sbin/swapon", str(swapon))

            def run():
                return subprocess.run(["sh", "-c", script], check=False).returncode

            self.assertEqual(run(), 0)  # No file: leave swap disabled.
            self.assertFalse(called.exists())
            swap.symlink_to(root / "other")
            self.assertNotEqual(run(), 0)
            swap.unlink()
            with swap.open("wb") as out:
                out.truncate(6 * 1024**3)
            swap.chmod(0o644)
            self.assertNotEqual(run(), 0)
            swap.chmod(0o600)
            self.assertEqual(run(), 0)
            self.assertEqual(called.read_text(), str(swap))
            self.assertEqual(swappiness.read_text(), "10\n")
            called.unlink()
            swaps.write_text(f"Filename Type Size Used Priority\n{swap} file 1 0 -2\n")
            self.assertEqual(run(), 0)
            self.assertFalse(called.exists())


if __name__ == "__main__":
    unittest.main()
