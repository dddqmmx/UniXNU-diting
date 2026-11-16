"""Exercise DDIC ownership/rollback, TE prerequisites and bounded USB pixels."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class PanelTests(unittest.TestCase):
    def build_and_run(self, source, cases):
        with tempfile.TemporaryDirectory(prefix='diting-panel-') as directory:
            binary = Path(directory) / 'panel'
            result = subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++17', '-O1', '-g',
                '-Wall', '-Wextra', '-Werror', '-pedantic', '-fsanitize=address,undefined',
                '-I' + str(ROOT / 'drivers/DitingPower'), '-I' + str(ROOT / 'drivers/DitingUsb'),
                '-I' + str(ROOT / 'drivers/DitingAdreno'), str(ROOT / 'tests' / source), '-o', str(binary)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for case in cases:
                with self.subTest(case=case):
                    result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual(result.stdout.strip(), 'PASS ' + case)
    def test_mode_and_command_ownership(self):
        self.build_and_run('diting_panel_mode.cpp', ('all-plans', 'unknown', 'metadata', 'malformed',
            'optional-colour', 'colour-guards', 'colour-correction', 'colour-rollback',
            'colour-restore-failed', 'colour-retained', 'readback-rollback', 'tear-rollback', 'retained', 'busy'))
    def test_tear_and_pixel_reader(self):
        self.build_and_run('diting_panel_output.cpp', ('te-transition', 'te-failures',
            'te-prerequisites', 'pixel-ranges', 'pixel-reply'))

if __name__ == '__main__':
    unittest.main()
