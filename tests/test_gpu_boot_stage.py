"""Exercise the boot installer's failure boundaries against actual host files."""
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]


def archive(bodies=None, install_id=bytes(range(16))):
    macho = struct.pack('<II', 0xfeedfacf, 0x0100000c) + bytes(24)
    bodies = bodies or [b'<plist>Diting fixture</plist>', macho, macho, macho,
                        b'*\n', b'1\n', macho, b'<plist>capture fixture</plist>']
    records = bytearray()
    for data in bodies:
        packed = zlib.compress(data)
        records += struct.pack('<QQ', len(data), len(packed))
        records += packed + bytes(-len(packed) % 8)
    return struct.pack('<8sIIIIQ', b'DTGPU001', 1, 0x07030002, 8, 0, 48 + len(records)) + install_id + records


@unittest.skipUnless(shutil.which('cc'), 'native C compiler required')
class BootStageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='diting-gpu-boot-test-')
        cls.directory = Path(cls.build.name)
        headers = cls.directory / 'libkern'
        headers.mkdir()
        (headers / 'zlib.h').write_text('#include <zlib.h>\n')
        cls.obj = cls.directory / 'boot.o'
        cls.compiler = shutil.which('cc')
        subprocess.run([cls.compiler, '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                        '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation',
                        '-I' + str(cls.directory), '-I' + str(ROOT / 'kits/DitingPlatform24'),
                        '-c', str(ROOT / 'tests/gpu_runtime_boot.c'), '-o', str(cls.obj)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.work = tempfile.TemporaryDirectory(prefix='diting-gpu-os-test-')
        self.root = Path(self.work.name) / 'os'
        for name in ('System/Library/Extensions', 'System/Library/LaunchDaemons', 'diting',
                     'private/var', 'private/preboot'):
            (self.root / name).mkdir(parents=True, exist_ok=True)
        for name in ('private/var/keep', 'private/preboot/keep'):
            (self.root / name).write_bytes(b'guest data remains current')
        self.exe = Path(self.work.name) / 'boot'
        self.make_executable(archive())

    def tearDown(self):
        for name in ('private/var/keep', 'private/preboot/keep'):
            self.assertEqual((self.root / name).read_bytes(), b'guest data remains current')
        self.work.cleanup()

    def make_executable(self, data):
        package = Path(self.work.name) / 'runtime.pack'
        package.write_bytes(data)
        assembly = Path(self.work.name) / 'runtime.S'
        assembly.write_text('.section .rodata\n.globl DitingGpuRuntimeData\nDitingGpuRuntimeData:\n'
                            '.incbin ' + json.dumps(str(package)) + '\n'
                            '.globl DitingGpuRuntimeEnd\nDitingGpuRuntimeEnd:\n'
                            '.section .note.GNU-stack,"",@progbits\n')
        subprocess.run([self.compiler, '-fsanitize=address,undefined', str(self.obj), str(assembly),
                        '-lz', '-o', str(self.exe)], check=True)

    def run_boot(self, scenario='normal'):
        output = subprocess.check_output([str(self.exe), str(self.root), scenario], text=True)
        return json.loads(output.splitlines()[-1])

    def assert_no_writes(self, result):
        self.assertNotEqual(result['error'], 0)
        self.assertEqual((result['mounts'], result['writes']), (0, 0))
        self.assertFalse((self.root / 'System/Library/Extensions/DitingAdrenoMetal.publish').exists())

    def test_first_install_publishes_complete_bundle_and_reboot_skips_inflation(self):
        result = self.run_boot()
        self.assertEqual((result['error'], result['stage']), (0, 6))
        self.assertEqual(result['mounts'], 1)
        self.assertEqual((self.root / 'System/Library/Extensions/DitingAdrenoMetal.publish').read_bytes(), b'1\n')
        self.assertTrue((self.root / 'diting/gpu-capture18').is_file())
        result = self.run_boot()
        self.assertEqual((result['error'], result['writes'], result['mounts'], result['allocations']), (0, 0, 0, 0))

    def test_incomplete_write_keeps_publication_absent_and_retries(self):
        result = self.run_boot('fail-write')
        self.assertNotEqual(result['error'], 0)
        self.assertFalse((self.root / 'System/Library/Extensions/DitingAdrenoMetal.publish').exists())
        stage = self.root / 'System/Library/Extensions/DitingAdrenoMetal.bundle.diting-stage'
        # Represents a torn staging file left by power loss, with our durable ID.
        (stage / 'DitingMetalBridge.dylib').write_bytes(b'torn')
        (stage / 'DitingMetalBridge.dylib').chmod(0o755)
        result = self.run_boot()
        self.assertEqual((result['error'], result['stage']), (0, 6))

    def test_existing_foreign_bundle_or_symlink_is_preserved(self):
        bundle = self.root / 'System/Library/Extensions/DitingAdrenoMetal.bundle'
        bundle.mkdir()
        (bundle / 'owner').write_bytes(b'foreign')
        self.assert_no_writes(self.run_boot())
        self.assertEqual((bundle / 'owner').read_bytes(), b'foreign')
        shutil.rmtree(bundle)
        extensions = self.root / 'System/Library/Extensions'
        extensions.rmdir()
        extensions.symlink_to(self.root / 'private/var', target_is_directory=True)
        self.assert_no_writes(self.run_boot())

    def test_wrong_root_prerequisites_never_mount_or_write(self):
        for scenario in ('rootful-off', 'wrong-fs', 'wrong-device'):
            with self.subTest(scenario=scenario):
                self.assert_no_writes(self.run_boot(scenario))

    def test_malformed_header_and_stream_are_refused_before_writes(self):
        for offset in (0, 8, 12, 16, 20, 24):
            damaged = bytearray(archive())
            damaged[offset] ^= 0xff
            self.make_executable(damaged)
            self.assert_no_writes(self.run_boot())
        damaged = bytearray(archive())
        damaged[64] ^= 0xff  # first zlib stream, after the 48-byte header and sizes
        self.make_executable(damaged)
        self.assert_no_writes(self.run_boot())

    def test_installed_marker_cannot_hide_a_missing_runtime_library(self):
        self.assertEqual(self.run_boot()['error'], 0)
        (self.root / 'System/Library/Extensions/DitingAdrenoMetal.bundle/libvulkan_freedreno.dylib').unlink()
        result = self.run_boot()
        self.assertNotEqual(result['error'], 0)
        self.assertEqual((result['writes'], result['mounts'], result['allocations']), (0, 0, 0))

    def test_existing_diting_bundle_with_older_id_is_upgraded_atomically(self):
        self.assertEqual(self.run_boot()['error'], 0)
        macho = struct.pack('<II', 0xfeedfacf, 0x0100000c) + bytes(24)
        updated_plugin = macho + b'updated-plugin-v2'
        new_id = bytes(range(16, 32))
        self.make_executable(archive([
            b'<plist>Diting fixture</plist>', updated_plugin, macho, macho,
            b'*\n', b'1\n', macho, b'<plist>capture fixture</plist>'
        ], install_id=new_id))
        result = self.run_boot()
        self.assertEqual((result['error'], result['stage']), (0, 6))
        self.assertEqual(
            (self.root / 'System/Library/Extensions/DitingAdrenoMetal.bundle/DitingAdrenoMetal').read_bytes(),
            updated_plugin)
        self.assertEqual(
            (self.root / 'System/Library/Extensions/DitingAdrenoMetal.bundle/.diting-native-gpu-runtime-v1').read_bytes(),
            new_id)
        result = self.run_boot()
        self.assertEqual((result['error'], result['writes'], result['mounts'], result['allocations']), (0, 0, 0, 0))


if __name__ == '__main__':
    unittest.main()
