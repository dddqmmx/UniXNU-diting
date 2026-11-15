"""Bounded, read-only production clock capture with real prerequisite gates."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ClockTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='diting-clock-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / 'clock'
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1',
                        '-Wall', '-Wextra', '-Werror', '-pedantic', '-g',
                        '-fsanitize=address,undefined', '-I' + str(ROOT / 'drivers/DitingAdreno'),
                        str(ROOT / 'tests/diting_gpu_clock.cpp'), '-o', str(cls.binary)],
                       capture_output=True, text=True, check=True)

    def test_no_unqualified_or_unbounded_hardware_reads(self):
        for case in ['prerequisites', 'off', 'stable', 'changed', 'gate', 'powerlost', 'io', 'arguments']:
            with self.subTest(case=case):
                result = subprocess.run([str(self.binary), case], capture_output=True,
                                        text=True, check=True, timeout=10)
                self.assertEqual(result.stdout.strip(), 'PASS ' + case)


if __name__ == '__main__':
    unittest.main()
