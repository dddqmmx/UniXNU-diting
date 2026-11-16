"""Check real monitor ownership, quantization and bounded restoration faults."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
class BandwidthProbeTests(unittest.TestCase):
    def test_monitor_contract(self):
        with tempfile.TemporaryDirectory(prefix='diting-bandwidth-') as directory:
            binary=Path(directory)/'bandwidth'
            built=subprocess.run([os.environ.get('CXX','clang++'),'-std=c++17','-O1','-g',
                '-Wall','-Wextra','-Werror','-pedantic','-fsanitize=address,undefined',
                '-I'+str(ROOT/'drivers/DitingPower'),str(ROOT/'tests/diting_bandwidth_probe.cpp'),
                '-o',str(binary)],capture_output=True,text=True)
            self.assertEqual(built.returncode,0,built.stdout+built.stderr)
            for case in ('measured-restored','owned-prerequisites','owned-diagnostics','no-data','failed-restore','failed-setup'):
                with self.subTest(case=case):
                    ran=subprocess.run([str(binary),case],capture_output=True,text=True,timeout=10)
                    self.assertEqual(ran.returncode,0,ran.stdout+ran.stderr)
                    self.assertEqual(ran.stdout.strip(),'PASS '+case)
if __name__=='__main__':unittest.main()
