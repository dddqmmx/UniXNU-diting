"""Host staging and rejection tests, without GPU work or image verification."""
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('gpu_runtime18', ROOT / 'userland/gpu_runtime18.py')
gpu = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gpu)


class GpuRuntimeTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name) / 'os'
        self.runtime = Path(self.directory.name) / 'runtime'
        version = self.root / 'System/Library/CoreServices/SystemVersion.plist'
        version.parent.mkdir(parents=True)
        version.write_bytes(plistlib.dumps(dict(ProductBuildVersion='22H374')))
        bundle = self.runtime / gpu.BUNDLE
        bundle.mkdir(parents=True)
        info = dict(CFBundleIdentifier='dev.unixnu.diting.Metal',
                    CFBundleExecutable='DitingAdrenoMetal',
                    NSPrincipalClass='DitingAdrenoMetalDevice', MinimumOSVersion='18.0')
        (bundle / 'Info.plist').write_bytes(plistlib.dumps(info))
        for name in gpu.BINARIES:
            (bundle / name).write_bytes(('host staging fixture: ' + name).encode())
        (self.runtime / gpu.POLICY[0]).write_text('*\n')
        (self.runtime / gpu.POLICY[1]).write_text('1\n')
        self.report = dict(runtime_format='diting-native-gpu-runtime-v1', guest='22H374',
                           native_abi=1, chip_id='0x07030002', device_extent=600,
                           performance=dict(frontend_optimization=3, turnip_optimization=3,
                                            shader_cache=True, host_import_batch=64))
        self.save_report()
        self.chown = patch.object(gpu.os, 'chown')
        self.chown.start()
        self.addCleanup(self.chown.stop)

    def save_report(self):
        (self.runtime / 'build-report.json').write_text(json.dumps(self.report))

    def test_complete_runtime_precedes_publication_and_preserves_data(self):
        data = self.root / 'private/var/mobile/Library/Caches'
        data.mkdir(parents=True)
        cache = data / 'existing-shader-cache'
        cache.write_bytes(b'existing cache')
        real_replace = os.replace
        def replace(source, target):
            for name in gpu.BINARIES:
                path = self.root / 'System/Library/Extensions' / gpu.BUNDLE / name
                self.assertEqual(path.read_bytes(), (self.runtime / gpu.BUNDLE / name).read_bytes())
            real_replace(source, target)
        with patch.object(gpu.os, 'replace', side_effect=replace):
            result = gpu.stage(self.root, self.runtime)
        self.assertEqual(cache.read_bytes(), b'existing cache')
        self.assertFalse(result['data_modified'])
        self.assertFalse(result['hardware_acceleration_confirmed'])
        extension = self.root / 'System/Library/Extensions'
        self.assertEqual((extension / gpu.POLICY[0]).read_text(), '*\n')
        self.assertEqual((extension / gpu.BUNDLE / 'Info.plist').stat().st_mode & 0o777, 0o644)
        self.assertEqual((extension / gpu.BUNDLE / gpu.BINARIES[0]).stat().st_mode & 0o777, 0o755)

    def test_missing_turnip_cannot_publish_an_incomplete_runtime(self):
        (self.runtime / gpu.BUNDLE / gpu.BINARIES[2]).unlink()
        with self.assertRaises(RuntimeError):
            gpu.stage(self.root, self.runtime)
        self.assertFalse((self.root / 'System/Library/Extensions').exists())

    def test_wrong_guest_abi_or_cache_policy_is_rejected_before_writes(self):
        for key, value in [('guest', '20G81'), ('native_abi', 2), ('chip_id', '0x07040000')]:
            with self.subTest(key=key):
                old = self.report[key]
                self.report[key] = value
                self.save_report()
                with self.assertRaises(RuntimeError):
                    gpu.stage(self.root, self.runtime)
                self.report[key] = old
        self.report['performance']['shader_cache'] = False
        self.save_report()
        with self.assertRaises(RuntimeError):
            gpu.stage(self.root, self.runtime)
        self.assertFalse((self.root / 'System/Library/Extensions').exists())

    def test_foreign_bundle_and_aliased_paths_are_preserved(self):
        extension = self.root / 'System/Library/Extensions'
        target = extension / gpu.BUNDLE
        target.mkdir(parents=True)
        old = target / 'Info.plist'
        old.write_bytes(plistlib.dumps(dict(CFBundleIdentifier='foreign.gpu')))
        with self.assertRaises(RuntimeError):
            gpu.stage(self.root, self.runtime)
        self.assertEqual(plistlib.loads(old.read_bytes())['CFBundleIdentifier'], 'foreign.gpu')
        old.unlink()
        target.rmdir()
        target.symlink_to(self.runtime / gpu.BUNDLE, target_is_directory=True)
        with self.assertRaises(RuntimeError):
            gpu.stage(self.root, self.runtime)
        self.assertTrue(target.is_symlink())


if __name__ == '__main__':
    unittest.main()
