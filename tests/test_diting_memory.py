"""Exercise command-db corruption and LLCC ownership/readback/timeouts."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
class MemoryPolicyTests(unittest.TestCase):
    def test_bcm_units_and_shared_llcc_slice_ownership(self):
        with tempfile.TemporaryDirectory(prefix='diting-memory-') as tmp:
            binary = Path(tmp) / 'memory'
            result = subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++17', '-O1', '-g',
                '-Wall', '-Wextra', '-Werror', '-pedantic', '-fsanitize=address,undefined',
                '-I' + str(ROOT / 'drivers/DitingPower'), str(ROOT / 'tests/diting_memory.cpp'),
                '-o', str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout.strip(), 'PASS 13 memory policy cases')
if __name__ == '__main__':
    unittest.main()
