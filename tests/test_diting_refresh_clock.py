"""Compile and exercise the actual new XNU header from its incremental patch."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

class RefreshClockTests(unittest.TestCase):
    def test_xnu_clock_skips_old_periods_and_preserves_phase(self):
        patch=ROOT/'patches/xnu-ipados18/zzzzzzzzzzzzzzzzzzzzzzzz-diting-refresh-deadlines.patch'
        with tempfile.TemporaryDirectory(prefix='diting-clock-') as directory:
            work=Path(directory)
            subprocess.run(['git','apply','--include=iokit/Kernel/MiniDisplayRefreshClock.h',str(patch)],
                           cwd=work,check=True,capture_output=True)
            binary=work/'clock'
            subprocess.run([os.environ.get('CXX','clang++'),'-std=c++17','-O1','-g',
                '-Wall','-Wextra','-Werror','-pedantic','-fsanitize=address,undefined',
                '-I'+str(work/'iokit/Kernel'),str(ROOT/'tests/diting_refresh_clock.cpp'),'-o',str(binary)],
                check=True,capture_output=True)
            result=subprocess.run([str(binary)],check=True,capture_output=True,text=True,timeout=10)
            self.assertIn('PASS refresh phase',result.stdout)

if __name__=='__main__':unittest.main()
