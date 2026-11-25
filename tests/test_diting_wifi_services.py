"""Check Wi-Fi-only native jobs, code signing, and the actual boot transaction.

No phone, firmware, credentials, image hashes or shared build cache are used.
"""
from copy import deepcopy
import hashlib
import os
from pathlib import Path
import plistlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from diting_wifi_services import (BOOTSTRAP_JOB, BOOTSTRAP_LABEL, DESTINATIONS,
    ENTITLEMENTS, HEADER_BYTES, WIFID, native_jobs, pack_runtime,
    sign_bootstrap, verify_bootstrap_signature)


def cache_fixture():
    return dict(VersionNumber=7, LaunchDaemons={
        WIFID: dict(Label='com.apple.wifid', Program='/usr/sbin/wifid',
            RunAtLoad=True, KeepAlive=True,
            EnvironmentVariables={'existing': 'retained'},
            MachServices={'com.apple.wifi.xpc': True, 'com.apple.CoreWiFi': {'ResetAtClose': True}}),
        '/System/Library/LaunchDaemons/dev.gpu.plist': dict(Label='dev.gpu',
            Program='/diting/diting-gpu-auto', MachServices={'gpu': True}),
        '/System/Library/LaunchDaemons/dev.ui.plist': dict(Label='dev.ui',
            ProgramArguments=['/diting/ui'], EnvironmentVariables={'existing': 'UI'}),
        '/System/Library/LaunchDaemons/dev.usb.plist': dict(Label='dev.usb',
            Program='/diting/usb', KeepAlive={'SuccessfulExit': False})},
        LaunchAgents={'/System/Library/LaunchAgents/test.plist': {'Label': 'original'}},
        JobEnvironment={'original': [1, 'retained', True]},
        CacheTables={'nested': {'binary': b'\0\xff', 'version': 9}})


def encode(value):
    return plistlib.dumps(value, fmt=plistlib.FMT_BINARY)


def executable_fixture():
    # A bounded Mach-O format fixture, not a runnable or hardware provider.
    commands = struct.pack('<2I16s4Q4I', 0x19, 72, b'__TEXT',
        0x100000000, 16384, 0, 16384, 5, 5, 0, 0)
    commands += struct.pack('<2I16s4Q4I', 0x19, 72, b'__LINKEDIT',
        0x100004000, 32768, 16384, 16384+12, 1, 1, 0, 0)
    commands += struct.pack('<4I', 0x1d, 16, 32768, 12)
    header = struct.pack('<8I', 0xfeedfacf, 0x100000c, 0, 2, 3, len(commands), 0, 0)
    data = bytearray(header+commands)
    data.extend(bytes((i % 251 for i in range(32768-len(data)))))
    return bytes(data) + struct.pack('>3I', 0xfade0cc0, 12, 0)


class WifiServiceTests(unittest.TestCase):
    def test_only_two_wifi_jobs_change_in_the_complete_native_cache(self):
        original = cache_fixture()
        job_bytes, wifid_bytes, cache_bytes, record = native_jobs(encode(original), True)
        updated = plistlib.loads(cache_bytes)
        expected = deepcopy(original)
        expected['LaunchDaemons'][WIFID]['MachServices']['com.apple.wifi.manager'] = {'HideUntilCheckIn': True}
        expected['LaunchDaemons'][WIFID]['EnvironmentVariables']['OS_ACTIVITY_DT_MODE'] = 'YES'
        expected['LaunchDaemons'][WIFID]['StandardOutPath'] = '/private/var/logs/DitingService-wifid-wifi.log'
        expected['LaunchDaemons'][WIFID]['StandardErrorPath'] = '/private/var/logs/DitingService-wifid-wifi.log'
        expected['LaunchDaemons'][BOOTSTRAP_JOB] = plistlib.loads(job_bytes)
        self.assertEqual(updated, expected)
        self.assertEqual(plistlib.loads(wifid_bytes), updated['LaunchDaemons'][WIFID])
        self.assertEqual(record['changed_jobs'], [WIFID, BOOTSTRAP_JOB])
        bootstrap = plistlib.loads(job_bytes)
        self.assertEqual(bootstrap['ProgramArguments'], ['/diting/wifi-bootstrap'])
        self.assertEqual(bootstrap['Nice'], 19)
        self.assertTrue(bootstrap['LowPriorityIO'])
        self.assertTrue(bootstrap['LaunchOnlyOnce'])
        self.assertNotIn('AllowEnable', bootstrap)
        self.assertEqual(original, cache_fixture())

    def test_native_policy_is_preserved_without_optional_logs(self):
        original = cache_fixture()
        original['LaunchDaemons'][WIFID]['MachServices']['com.apple.wifi.manager'] = {'ResetAtClose': True}
        _, _, updated, _ = native_jobs(encode(original))
        wifid = plistlib.loads(updated)['LaunchDaemons'][WIFID]
        self.assertEqual(wifid['MachServices']['com.apple.wifi.manager'],
                         {'ResetAtClose': True, 'HideUntilCheckIn': True})
        self.assertEqual(wifid['EnvironmentVariables'], {'existing': 'retained'})
        self.assertNotIn('StandardOutPath', wifid)
        self.assertTrue(wifid['KeepAlive'])

    def test_wrong_native_inputs_and_foreign_bootstrap_are_rejected(self):
        cases = ['wrong cache type', {'VersionNumber': 6}, cache_fixture(),
                 cache_fixture(), cache_fixture(), cache_fixture(), cache_fixture()]
        cases[2]['LaunchDaemons'][WIFID]['Program'] = '/diting/fake-wifid'
        cases[3]['LaunchDaemons'][WIFID]['Disabled'] = True
        cases[4]['LaunchDaemons'][WIFID]['MachServices'] = []
        cases[5]['LaunchDaemons'][BOOTSTRAP_JOB] = {'Label': 'foreign', 'ProgramArguments': ['/foreign']}
        cases[6]['LaunchDaemons'][WIFID]['EnvironmentVariables'] = []
        for original in cases:
            with self.subTest(original=original):
                with self.assertRaises(ValueError): native_jobs(encode(original), True)
        for data in (b'', b'xml is not a captured binary cache', b'bplist00'+b'\0'*(2<<20)):
            with self.assertRaises(ValueError): native_jobs(data)

    def test_code_signing_pages_and_entitlement_slots(self):
        signed = sign_bootstrap(executable_fixture())
        record = verify_bootstrap_signature(signed)
        self.assertEqual(record['code_slots'], 8)
        limit, sig_size = struct.unpack_from('<2I', signed, 32+72+72+8)
        blob = signed[limit:]
        self.assertEqual(struct.unpack_from('>3I', blob), (0xfade0cc0, sig_size, 2))
        cd = blob[28:struct.unpack_from('>I', blob, 24)[0]]
        hash_offset = struct.unpack_from('>I', cd, 16)[0]
        ent = blob[struct.unpack_from('>I', blob, 24)[0]:]
        self.assertEqual(plistlib.loads(ent[8:]), ENTITLEMENTS)
        self.assertEqual(cd[hash_offset-160:hash_offset-128], hashlib.sha256(ent).digest())
        for page in range(8):
            self.assertEqual(cd[hash_offset+32*page:hash_offset+32*(page+1)],
                             hashlib.sha256(signed[4096*page:4096*(page+1)]).digest())
        # Mutate a real signing page, the directory, its slot index, XML, or
        # the signature size. None may enter the runtime archive unnoticed.
        for offset in (256, 4096, 32767, limit+12, limit+32, len(signed)-16):
            data = bytearray(signed); data[offset] ^= 1
            with self.subTest(offset=offset):
                with self.assertRaises((ValueError, plistlib.InvalidFileException)):
                    pack_runtime(data, encode(cache_fixture()))
        for cut in (0, 31, 33, len(signed)-1):
            with self.assertRaises(ValueError): verify_bootstrap_signature(signed[:cut])
        with self.assertRaises(ValueError): pack_runtime(executable_fixture(), encode(cache_fixture()))

    def test_fixed_order_archive_preserves_the_exact_cache_preimage(self):
        original = encode(cache_fixture())
        executable = sign_bootstrap(executable_fixture())
        packed, report = pack_runtime(executable, original, identifier=bytes(range(16)))
        self.assertEqual(packed[:8], b'DTWIFI01')
        self.assertEqual(struct.unpack_from('<4IQ16s', packed, 8),
                         (1, 0x8475, 5, 0, len(packed), bytes(range(16))))
        cursor, contents = HEADER_BYTES, []
        for _ in range(5):
            raw, size = struct.unpack_from('<2Q', packed, cursor); cursor += 16
            inflater = zlib.decompressobj()
            data = inflater.decompress(packed[cursor:cursor+size])+inflater.flush()
            self.assertTrue(inflater.eof)
            self.assertFalse(inflater.unused_data)
            self.assertEqual(len(data), raw)
            contents.append(data)
            cursor += (size+7) & ~7
        self.assertEqual(cursor, len(packed))
        self.assertEqual(contents[0], executable)
        self.assertEqual(contents[4], original)
        self.assertEqual(report['destinations'], list(DESTINATIONS))
        self.assertFalse(report['password_or_network_preferences_included'])
        with self.assertRaises(ValueError): pack_runtime(executable, original, identifier=b'\0'*16)

    def test_actual_archive_inflater_and_install_transaction(self):
        compiler = os.environ.get('DITING_WIFI_HOST_CXX') or shutil.which('clang++')
        if not compiler: self.skipTest('Host clang++ is required')
        with tempfile.TemporaryDirectory(prefix='diting-wifi-services-') as directory:
            output = Path(directory)
            packed, _ = pack_runtime(sign_bootstrap(executable_fixture()), encode(cache_fixture()),
                                     identifier=bytes(range(16)))
            archive = output / 'runtime.pack'; archive.write_bytes(packed)
            binary = output / 'transaction-test'
            prefix = []
            for name, arguments in [('chrt', ['--idle', '0']), ('nice', ['-n', '19']), ('ionice', ['-c', '3'])]:
                if shutil.which(name): prefix += [name, *arguments]
            result = subprocess.run([*prefix, compiler, '-std=gnu++17', '-O1', '-g', '-Wall', '-Wextra',
                '-Werror', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                str(ROOT / 'tests/diting_wifi_runtime.cpp'), '-lz', '-o', str(binary)],
                capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            result = subprocess.run([*prefix, str(binary), str(archive)],
                capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            self.assertIn('PASS:', result.stdout)
            print(result.stdout.strip())


if __name__ == '__main__': unittest.main()
