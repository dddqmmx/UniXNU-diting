"""Exercise the actual keyboard host and XNU injection compatibility policies."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

class KeyboardHostTests(unittest.TestCase):
    def test_native_host_coverage_and_legacy_injection(self):
        patch=ROOT/'patches/xnu-ipados18/zzzzzzzzzzzzzzzzzzzzzzzzz-diting-keyboard-host.patch'
        additions='\n'.join(line[1:] for line in patch.read_text().splitlines()
                            if line.startswith('+') and not line.startswith('+++'))
        functions=re.findall(r'static (?:bool|void)\nexec_(?:keyboard_host_alias|inject_basename)\(.*?\n\}',additions,re.S)
        self.assertEqual(len(functions),2)
        with tempfile.TemporaryDirectory(prefix='diting-keyboard-host-') as directory:
            out=Path(directory)
            (out/'keyboard-injection-fixture.h').write_text('\n'.join(functions)+'\n')
            binary=out/'keyboard-host'
            subprocess.run([os.environ.get('CC','clang'),'-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
                            '-fsanitize=address,undefined','-I'+str(out),str(ROOT/'tests/diting_keyboard_host.c'),
                            '-o',str(binary)],check=True,capture_output=True,text=True)
            result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertIn('PASS keyboard host',result.stdout)

if __name__=='__main__':
    unittest.main()
