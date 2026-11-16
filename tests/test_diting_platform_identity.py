"""Check software platform identity encoding and the kernel's bounded parser."""
import importlib.util
import contextlib
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('diting_platform_identity', ROOT/'scripts/diting_platform_identity.py')
identity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(identity)
sys.path.insert(0, str(ROOT/'scripts'))


class PlatformIdentityTests(unittest.TestCase):
    def test_stable_encoding_preserves_the_uuid_and_exact_adt_extent(self):
        value = 'a1234567-b123-5123-8123-c123456789ab'
        self.assertEqual(identity.platform_identity(value), value.encode()+b'\0')
        self.assertEqual(identity.platform_identity(value.upper()), value.encode()+b'\0')
        self.assertEqual(len(identity.platform_identity(value)), 37)

    def test_invalid_private_configuration_is_refused(self):
        values = [None, False, 1, {}, b'a1234567-b123-5123-8123-c123456789ab',
            '', '00000000-0000-0000-0000-000000000000',
            'a1234567b12351238123c123456789ab', '{a1234567-b123-5123-8123-c123456789ab}',
            'a1234567-b123-5123-8123-c123456789ab\0', 'a1234567-b123-5123-8123-c123456789ab\n',
            'a1234567-b123-5123-8123-c123456789ag']
        for value in values:
            with self.subTest(value=value), self.assertRaises(ValueError):
                identity.platform_identity(value)

    def test_kernel_parser_rejects_wrong_extent_null_and_unterminated_values(self):
        compiler = shutil.which('clang++')
        if not compiler: self.skipTest('Host clang++ required')
        task_root = os.environ.get('DITING_PLATFORM_TEST_TMP')
        with tempfile.TemporaryDirectory(prefix='diting-platform-', dir=task_root) as directory:
            binary = Path(directory)/'identity-test'
            subprocess.run([compiler,'-std=c++17','-O1','-g','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-fno-omit-frame-pointer',
                str(ROOT/'tests/diting_platform_identity.cpp'),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True,timeout=10)

    def test_actual_adt_generator_uses_a_private_property_and_keeps_apple_identity_absent(self):
        spec = importlib.util.spec_from_file_location('diting_identity_adt',ROOT/'scripts/make-diting-adt-ipados18.py')
        adt = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(adt)
        from diting_adt_contract import parse_adt
        value = 'a1234567-b123-5123-8123-c123456789ab'
        environment = dict(os.environ)
        environment.pop('DITING_HARDWARE_DTB',None)
        with tempfile.TemporaryDirectory(prefix='diting-identity-adt-',dir=os.environ.get('DITING_PLATFORM_TEST_TMP')) as directory:
            output = Path(directory)/'tree.adt'
            argv = ['make-diting-adt-ipados18.py','--cpus','8','--dram-size','0x200000000',
                '--output',str(output),'--skip-image-verification']
            for settings in ({'platform_uuid':value},{}):
                with patch.object(adt,'options',return_value=settings), patch.object(sys,'argv',argv), \
                        patch.dict(os.environ,environment,clear=True), contextlib.redirect_stdout(io.StringIO()):
                    adt.main()
                chosen = parse_adt(output.read_bytes())['/chosen']
                self.assertEqual(chosen.get('diting-platform-uuid'),identity.platform_identity(value) if settings else None)
                self.assertNotIn('unique-chip-id',chosen)
                self.assertNotIn('chip-id',chosen)
            with patch.object(adt,'options',return_value={'platform_uuid':'invalid'}),patch.object(sys,'argv',argv), \
                    contextlib.redirect_stderr(io.StringIO()),self.assertRaises(SystemExit):
                adt.main()


if __name__ == '__main__': unittest.main()
