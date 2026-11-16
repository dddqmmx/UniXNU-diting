"""Check refresh ordering, restoration and read-only colour register contracts."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class RefreshTests(unittest.TestCase):
    def test_production_transition_and_colour_reader(self):
        with tempfile.TemporaryDirectory(prefix='diting-refresh-') as directory:
            binary = Path(directory) / 'display-refresh'
            subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++17', '-O1', '-g',
                '-Wall', '-Wextra', '-Werror', '-pedantic', '-fsanitize=address,undefined',
                '-I' + str(ROOT / 'drivers/DitingPower'), '-I' + str(ROOT / 'drivers/DitingUsb'),
                str(ROOT / 'tests/diting_display_refresh.cpp'), '-o', str(binary)], check=True, capture_output=True)
            for case in ('transition', 'write-failures', 'timeout', 'power-loss', 'gates', 'colour'):
                with self.subTest(case=case):
                    result = subprocess.run([str(binary), case], check=True, capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.stdout.strip(), 'PASS ' + case)

if __name__ == '__main__':
    unittest.main()
