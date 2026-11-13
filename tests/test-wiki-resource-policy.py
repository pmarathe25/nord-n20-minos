"""Exercise the launcher's memory and battery guards with isolated fixtures."""
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class WikiResourcePolicyTests(unittest.TestCase):
    def test_start_and_stop_boundaries_fail_closed(self):
        launcher = (ROOT / "runtime/usr/local/sbin/n20-wiki-start").read_text()
        functions = launcher[launcher.index("memory_at_least() {"):launcher.index("configured=0")]
        with tempfile.TemporaryDirectory() as tmp:
            memory = Path(tmp) / "meminfo"
            temperature = Path(tmp) / "temperature"
            functions = functions.replace("/proc/meminfo", str(memory)).replace(
                "/sys/class/power_supply/battery/temp", str(temperature))

            def allowed(kib, temp, threshold, swap=0):
                memory.write_text((f"MemAvailable: {kib} kB\n" if kib is not None else "MemFree: 1 kB\n") +
                                  f"SwapFree: {swap} kB\n")
                temperature.write_text(str(temp) + "\n")
                return subprocess.run(["sh", "-c", functions +
                    f"\nmemory_at_least {threshold} && temperature_ok"],
                    check=False).returncode == 0

            self.assertTrue(allowed(3670016, 399, 3670016))
            self.assertFalse(allowed(3670015, 399, 3670016))
            self.assertTrue(allowed(921600, 399, 921600))
            self.assertFalse(allowed(921599, 399, 921600))
            self.assertFalse(allowed(4000000, 400, 3670016))
            self.assertFalse(allowed(4000000, "bad", 3670016))
            self.assertFalse(allowed(None, 200, 3670016))
            memory.write_text("MemAvailable: 262144 kB\nSwapFree: 2097152 kB\n")
            self.assertEqual(subprocess.run(["sh", "-c", functions + "\nmodel_start_memory_ok"],
                                            check=False).returncode, 0)
            memory.write_text("MemAvailable: 262143 kB\nSwapFree: 2097152 kB\n")
            self.assertNotEqual(subprocess.run(["sh", "-c", functions + "\nmodel_start_memory_ok"],
                                               check=False).returncode, 0)
            memory.write_text("MemAvailable: 131072 kB\nSwapFree: 524288 kB\n")
            self.assertEqual(subprocess.run(["sh", "-c", functions + "\nmodel_running_memory_ok"],
                                            check=False).returncode, 0)
            memory.write_text("MemAvailable: 131071 kB\nSwapFree: 524288 kB\n")
            self.assertNotEqual(subprocess.run(["sh", "-c", functions + "\nmodel_running_memory_ok"],
                                               check=False).returncode, 0)


if __name__ == "__main__":
    unittest.main()
