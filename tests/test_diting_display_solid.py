"""Exercise the production solid-fill controller, including ownership and restore failures."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

class SolidTests(unittest.TestCase):
    def test_production_controller_and_bounded_command_parser(self):
        with tempfile.TemporaryDirectory(prefix='diting-solid-') as directory:
            binary=Path(directory)/'solid'
            subprocess.run([os.environ.get('CXX','clang++'),'-std=c++17','-O1','-g',
                '-Wall','-Wextra','-Werror','-pedantic','-fsanitize=address,undefined',
                '-I'+str(ROOT/'drivers/DitingPower'),str(ROOT/'tests/diting_display_solid.cpp'),'-o',str(binary)],
                check=True,capture_output=True)
            for case in ('normal','fetch-order','write-failures','cancel-busy','power-loss','profile-gates','flush-timeout','parser'):
                with self.subTest(case=case):
                    result=subprocess.run([str(binary),case],check=True,capture_output=True,text=True,timeout=10)
                    self.assertEqual(result.stdout.strip(),'PASS '+case)

if __name__=='__main__':unittest.main()
