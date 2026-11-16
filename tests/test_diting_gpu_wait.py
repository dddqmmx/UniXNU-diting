"""Verify bounded fence waiting permits independent work and coherent panic records."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class WaitTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='diting-gpu-wait-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.root = Path(cls.directory.name)
        (cls.root / 'IOKit').mkdir()
        (cls.root / 'IOKit/IOTypes.h').write_text(
            '#pragma once\ntypedef int IOReturn;\n'
            'enum { kIOReturnSuccess=0,kIOReturnTimeout=1,kIOReturnBadArgument=2,kIOReturnNotReady=3 };\n')
        cls.flags = [os.environ.get('CXX', 'clang++'), '-std=c++17', '-O1', '-g', '-Wall', '-Wextra',
                     '-Werror', '-pedantic', '-fsanitize=address,undefined', '-pthread']
        cls.binary = cls.root / 'wait'
        subprocess.run([*cls.flags, '-I' + str(cls.root), '-I' + str(ROOT / 'drivers/DitingAdreno/submit'),
                        str(ROOT / 'tests/diting_gpu_wait.cpp'), '-o', str(cls.binary)],
                       check=True, capture_output=True)

    def test_completion_timeout_close_and_units(self):
        for case in ('poll', 'progress', 'timeout', 'close', 'units'):
            with self.subTest(case=case):
                result = subprocess.run([str(self.binary), case], check=True, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.stdout.strip(), 'PASS ' + case)

    def test_panic_records_remain_bounded_and_coherent_during_updates(self):
        binary = self.root / 'probes'
        subprocess.run([*self.flags, '-I' + str(ROOT / 'drivers'), str(ROOT / 'tests/diting_probe.cpp'),
                        str(ROOT / 'drivers/DitingProbe.cpp'), '-o', str(binary)], check=True, capture_output=True)
        result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=20)
        self.assertEqual(result.stdout.strip(), 'PASS bounded coherent panic snapshots')


if __name__ == '__main__':
    unittest.main()
