"""Restore selection and malformed-fileset refusal for the iOS 27 port."""
import copy
import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import tomllib
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from diting_ios27 import inspect_kernel_collection, require_complete_prelink, restore_identity


TARGET = {'version': '27.2.1', 'build': '24B156', 'product': 'iPhone18,2',
          'device_class': 'v54ap', 'variant': 'Customer Erase Install (IPSW)',
          'chip_id': 0x8150, 'board_id': 14, 'kernel_darwin': '27.2.0', 'kernel_xnu': '13432.42.3'}


def manifest():
    identities = []
    for variant, path in [('Recovery Customer Install', 'recovery.dmg.aea'),
                          ('Customer Erase Install (IPSW)', 'customer.dmg.aea')]:
        identities.append({'Info': {'DeviceClass': 'v54ap', 'RestoreBehavior': 'Erase', 'Variant': variant},
                           'ApChipID': '0x8150', 'ApBoardID': '0x0e',
                           'Manifest': {'OS': {'Info': {'Path': path}}}})
    return {'ProductVersion': '27.2.1', 'ProductBuildVersion': '24B156',
            'SupportedProductTypes': ['iPhone18,2'], 'BuildIdentities': identities}


def fileset():
    name = b'com.apple.kernel\0'
    length = (32 + len(name) + 7) & ~7
    command = struct.pack('<IIQQII', 0x80000035, length, 0xfffffe000700c000, 256, 32, 0)
    header = struct.pack('<8I', 0xfeedfacf, 0x100000c, 0xc0000002, 12, 1, length, 0, 0)
    data = bytearray(header + command + name + bytes(length - 32 - len(name)))
    data += bytes(256 - len(data)) + header
    data += b'Darwin Kernel Version 27.2.0: date; root:xnu-13432.42.3~28/RELEASE_ARM64_T8150\0'
    return data


class IdentityTests(unittest.TestCase):
    def test_recovery_first_selects_customer_os(self):
        chosen = restore_identity(manifest(), TARGET)
        self.assertEqual(chosen['Manifest']['OS']['Info']['Path'], 'customer.dmg.aea')

    def test_wrong_product_build_and_chip_are_refused(self):
        for key, value in [('build', '24A446'), ('version', '27.0.1'), ('product', 'iPad17,3'),
                           ('chip_id', 0x8010), ('board_id', 28), ('device_class', 'j171ap')]:
            with self.subTest(key=key), self.assertRaises(ValueError):
                restore_identity(manifest(), {**TARGET, key: value})

    def test_ambiguous_customer_identity_is_refused(self):
        image = manifest()
        image['BuildIdentities'].append(copy.deepcopy(image['BuildIdentities'][1]))
        with self.assertRaises(ValueError):
            restore_identity(image, TARGET)

    def test_no_customer_identity_is_refused(self):
        image = manifest()
        image['BuildIdentities'].pop()
        with self.assertRaises(ValueError):
            restore_identity(image, TARGET)


class KernelTests(unittest.TestCase):
    def test_release_identity_and_index(self):
        result = inspect_kernel_collection(fileset(), TARGET)
        self.assertEqual(result['entries']['com.apple.kernel']['offset'], 256)
        self.assertEqual(result['xnu'], '13432.42.3')

    def test_wrong_architecture_kind_and_extent_are_refused(self):
        for offset, value in [(8, 0), (12, 2), (20, 0xffffffff), (36, 7), (56, 31)]:
            broken = fileset()
            struct.pack_into('<I', broken, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                inspect_kernel_collection(broken, TARGET)

    def test_old_release_identity_is_refused(self):
        data = fileset().replace(b'13432.42.3', b'12377.121.6')
        with self.assertRaises(ValueError):
            inspect_kernel_collection(data, TARGET)


class PrelinkCompletionTests(unittest.TestCase):
    def test_complete_collection_is_accepted(self):
        require_complete_prelink({'format': 'MH_FILESET', 'mapping_complete': True,
                                  'allow_missing': False, 'loadable': False,
                                  'candidate_written': True,
                                  'missing': {}, 'stubbed': {}})

    def test_emitted_experimental_image_is_refused(self):
        report = {'format': 'MH_FILESET', 'mapping_complete': False,
                  'allow_missing': True, 'loadable': False, 'candidate_written': True,
                  'missing': {}, 'stubbed': {'com.apple.filesystems.apfs': {'0x1000': 2}}}
        with self.assertRaisesRegex(ValueError, '2 stubbed references'):
            require_complete_prelink(report)

    def test_gaps_and_incomplete_reports_are_refused(self):
        complete = {'format': 'MH_FILESET', 'mapping_complete': True,
                    'allow_missing': False, 'loadable': False,
                    'candidate_written': True,
                    'missing': {}, 'stubbed': {}}
        for key in ('format', 'mapping_complete', 'allow_missing', 'candidate_written'):
            report = complete.copy()
            del report[key]
            with self.subTest(key=key), self.assertRaises(ValueError):
                require_complete_prelink(report)
        with self.subTest(allow_missing=True), self.assertRaises(ValueError):
            require_complete_prelink({**complete, 'allow_missing': True})
        for kind in ('missing', 'stubbed'):
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                require_complete_prelink({**complete, kind: {'owner': {'0x1000': 1}}})

    def test_default_board_and_profile_use_ios27_strict_prelink(self):
        root = Path(__file__).resolve().parents[1]
        board = tomllib.loads((root / 'board.toml').read_text())
        profile = tomllib.loads((root / 'os-profiles/ios-27.2.1-24B156/profile.toml').read_text())
        porting = tomllib.loads((root / 'porting.toml').read_text())
        self.assertEqual(board['board']['os_profile'], 'ios-27.2.1-24B156')
        self.assertEqual(porting['device']['os_profile'], 'ios-27.2.1-24B156')
        self.assertEqual(board['kernel']['defconfig'], 'diting_ios27_defconfig')
        self.assertEqual(board['ipsw']['build'], '24B156')
        self.assertEqual(board['payload']['adt']['chip_id'], '0x8150')
        for spec in (profile['kernel']['prelink'], board['kernel']['prelink']):
            self.assertNotIn('--allow-missing', spec['extra_args'])
            self.assertIn('--fileset-no-reference-order', spec['extra_args'])
        legacy = board['os_overrides']['ipados-18.7.10-22H374']
        self.assertEqual(legacy['kernel']['defconfig'], 'diting_ipados18_defconfig')
        self.assertEqual(legacy['ipsw']['build'], '22H374')
        self.assertEqual(legacy['payload']['adt']['chip_id'], '0x8010')
        self.assertEqual(set(legacy['remove']),
                         {'ipsw.variant', 'kernel.prelink.early_security_init', 'kernel.prelink.extra_args'})


class ComponentPatchTests(unittest.TestCase):
    def test_private_patch_reuse_and_independent_edit_refusal(self):
        script = Path(__file__).resolve().parents[1] / 'scripts/build-diting-ios27.py'
        spec = importlib.util.spec_from_file_location('diting_ios27_builder_test', script)
        builder = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(builder)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            clean = root / 'clean'
            clean.mkdir()
            def git(*arguments):
                return subprocess.check_output(['git', '-C', str(clean), *arguments],
                                               text=True, stderr=subprocess.DEVNULL).strip()
            git('init', '-q')
            (clean / 'source.txt').write_text('original\n')
            git('add', 'source.txt')
            git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
                'commit', '-qm', 'isolated source fixture')
            revision = git('rev-parse', 'HEAD')
            patch_file = root / 'target.patch'
            patch_file.write_text('diff --git a/source.txt b/source.txt\n'
                                  '--- a/source.txt\n+++ b/source.txt\n@@ -1 +1 @@\n'
                                  '-original\n+patched\n'
                                  'diff --git a/owned.json b/owned.json\nnew file mode 100644\n'
                                  '--- /dev/null\n+++ b/owned.json\n@@ -0,0 +1 @@\n+{}\n')
            out = root / 'out'
            out.mkdir()
            entry = {'name': 'Fixture', 'revision': revision, 'patches': ['target.patch']}
            with patch.object(builder, 'ROOT', root):
                destination = builder.component_overrides(entry, clean, out)
                self.assertEqual((destination / 'source.txt').read_text(), 'patched\n')
                self.assertTrue((destination / 'owned.json').is_file())
                builder.component_overrides(entry, clean, out)
                (destination / 'source.txt').write_text('independent edit\n')
                with self.assertRaisesRegex(RuntimeError, 'Independent component edits retained'):
                    builder.component_overrides(entry, clean, out)
                self.assertEqual((destination / 'source.txt').read_text(), 'independent edit\n')
                self.assertEqual((destination / 'owned.json').read_text(), '{}\n')
                self.assertEqual((clean / 'source.txt').read_text(), 'original\n')


if __name__ == '__main__':
    unittest.main()
