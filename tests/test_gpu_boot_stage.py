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
        (cls.directory / 'sys').mkdir()
        (cls.directory / 'sys/stdio.h').write_text('/* Host rename flags are defined by the VFS adapter. */\n')
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

    def crash_boot(self, point):
        result = subprocess.run([str(self.exe), str(self.root), 'crash-' + point], capture_output=True)
        self.assertEqual(result.returncode, 99, result.stderr.decode(errors='replace'))

    def runtime_update(self):
        macho = struct.pack('<II', 0xfeedfacf, 0x0100000c) + bytes(24)
        bodies = [b'<plist>updated fixture</plist>', macho + b'plugin-v2', macho + b'bridge-v2',
                  macho + b'turnip-v2', b'*\n', b'1\n', macho + b'capture-v2', b'<plist>updated job</plist>']
        self.make_executable(archive(bodies, install_id=bytes(range(16, 32))))
        return bodies

    def reset_runtime(self):
        for entry in (self.root / 'System/Library/Extensions').iterdir():
            if entry.is_dir():
                shutil.rmtree(entry)
            else:
                entry.unlink()
        (self.root / 'diting/gpu-capture18').unlink(missing_ok=True)
        (self.root / 'System/Library/LaunchDaemons/dev.unixnu.diting.gpu-capture18.plist').unlink(missing_ok=True)

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

    def test_foreign_staging_directory_is_refused_before_any_write_mount(self):
        stage = self.root / 'System/Library/Extensions/DitingAdrenoMetal.bundle.diting-stage'
        stage.mkdir()
        (stage / 'keep').write_bytes(b'foreign stage')
        self.assert_no_writes(self.run_boot())
        self.assertEqual((stage / 'keep').read_bytes(), b'foreign stage')

    def test_foreign_helper_is_refused_before_bundle_creation(self):
        helper = self.root / 'diting/gpu-capture18'
        helper.write_bytes(b'foreign helper')
        helper.chmod(0o755)
        self.assert_no_writes(self.run_boot())
        self.assertEqual(helper.read_bytes(), b'foreign helper')

    def test_foreign_staged_helper_is_refused_before_write_mount(self):
        helper = self.root / 'diting/gpu-capture18.diting-gpu-stage'
        helper.write_bytes(b'foreign staged helper')
        helper.chmod(0o755)
        self.assert_no_writes(self.run_boot())
        self.assertEqual(helper.read_bytes(), b'foreign staged helper')

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

    def test_power_loss_after_bundle_exchange_retains_one_complete_version_and_recovers(self):
        self.assertEqual(self.run_boot()['error'], 0)
        bundle = self.root / 'System/Library/Extensions/DitingAdrenoMetal.bundle'
        stage = bundle.with_name(bundle.name + '.diting-stage')
        names = ['Info.plist', 'DitingAdrenoMetal', 'DitingMetalBridge.dylib', 'libvulkan_freedreno.dylib']
        before = [(bundle / name).read_bytes() for name in names]
        bodies = self.runtime_update()
        self.crash_boot('after-swap')
        self.assertEqual([(bundle / name).read_bytes() for name in names], bodies[:4])
        self.assertEqual([(stage / name).read_bytes() for name in names], before)
        self.assertEqual((bundle.parent / 'DitingAdrenoMetal.publish').read_bytes(), b'0\n')
        self.assertEqual(self.run_boot()['error'], 0)
        self.assertEqual((bundle.parent / 'DitingAdrenoMetal.publish').read_bytes(), b'1\n')
        self.assertEqual((bundle.parent / 'DitingAdrenoMetal.installed').read_bytes(), bytes(range(16, 32)))

    def test_each_late_commit_boundary_resumes_without_loading_partial_runtime(self):
        for point in ('after-disable', 'after-helper', 'after-ready'):
            with self.subTest(point=point):
                # Each point starts from the original complete installed ID.
                extensions = self.root / 'System/Library/Extensions'
                self.reset_runtime()
                self.make_executable(archive())
                self.assertEqual(self.run_boot()['error'], 0)
                self.runtime_update()
                self.crash_boot(point)
                self.assertEqual((extensions / 'DitingAdrenoMetal.publish').read_bytes(), b'0\n')
                self.assertEqual(self.run_boot()['error'], 0)
                self.assertEqual((extensions / 'DitingAdrenoMetal.publish').read_bytes(), b'1\n')

    def test_failed_final_publication_keeps_discovery_disabled_and_can_retry(self):
        self.assertEqual(self.run_boot()['error'], 0)
        self.runtime_update()
        result = self.run_boot('fail-publish')
        self.assertNotEqual(result['error'], 0)
        self.assertEqual(result['step'], 16)
        extensions = self.root / 'System/Library/Extensions'
        self.assertEqual((extensions / 'DitingAdrenoMetal.publish').read_bytes(), b'0\n')
        self.assertEqual(self.run_boot()['error'], 0)

    def test_first_install_recovers_at_each_persistent_boundary(self):
        for point in ('after-intent', 'after-stage', 'after-disable', 'after-bundle', 'after-helper', 'after-ready'):
            with self.subTest(point=point):
                self.reset_runtime()
                self.make_executable(archive())
                self.crash_boot(point)
                self.assertEqual(self.run_boot()['error'], 0)
                self.assertEqual((self.root / 'System/Library/Extensions/DitingAdrenoMetal.publish').read_bytes(), b'1\n')

    def test_publish_directory_sync_failure_revokes_discovery_and_recovers(self):
        self.assertEqual(self.run_boot()['error'], 0)
        self.runtime_update()
        result = self.run_boot('fail-publish-sync')
        self.assertNotEqual(result['error'], 0)
        self.assertEqual(result['step'], 16)
        self.assertEqual((self.root / 'System/Library/Extensions/DitingAdrenoMetal.publish').read_bytes(), b'0\n')
        self.assertEqual(self.run_boot()['error'], 0)

    def test_new_candidate_recovers_an_older_torn_stage_without_an_internal_id(self):
        result = self.run_boot('fail-write')
        self.assertNotEqual(result['error'], 0)
        stage = self.root / 'System/Library/Extensions/DitingAdrenoMetal.bundle.diting-stage'
        self.assertFalse((stage / '.diting-native-gpu-runtime-v1').exists())
        self.runtime_update()
        self.assertEqual(self.run_boot()['error'], 0)

    def test_new_candidate_recovers_an_interrupted_older_candidate(self):
        self.assertEqual(self.run_boot()['error'], 0)
        self.runtime_update()
        self.crash_boot('after-swap')
        final_id = bytes(range(32, 48))
        self.make_executable(archive(install_id=final_id))
        self.assertEqual(self.run_boot()['error'], 0)
        extensions = self.root / 'System/Library/Extensions'
        self.assertEqual((extensions / 'DitingAdrenoMetal.installed').read_bytes(), final_id)
        self.assertEqual((extensions / 'DitingAdrenoMetal.publish').read_bytes(), b'1\n')


if __name__ == '__main__':
    unittest.main()
