"""Keep signed Hexagon segments intact and outside the loader's managed RAM."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import diting_adsp_firmware as firmware

REGION = dict(base=0x9fd00000, size=0x3100000)


def split_files(path, elf64=False):
    size, phsize = (64, 56) if elf64 else (52, 32)
    headsize = size + 4 * phsize
    ident = b'\x7fELF' + bytes([2 if elf64 else 1, 1, 1]) + bytes(9)
    values = (ident, 2, 164, 1, 0, size, 0, 0, size, phsize, 4, 0, 0, 0)
    head = struct.pack('<16sHHIQQQIHHHHHH' if elf64 else '<16sHHIIIIIHHHHHH', *values)
    pieces = [(0, 0, 0, headsize, 0, 7 << 24),
              (1, 0x1000, 0x86300000, 37, 4096, firmware.RELOCATABLE | 5),
              (1, 0x2000, 0x86301000, 8, 4096, firmware.RELOCATABLE | 6),
              (0, 0x7000, 0, 16, 16, firmware.TYPE_HASH)]
    for kind, offset, address, length, memory, flags in pieces:
        ph = (kind, flags, offset, 0, address, length, memory, 4096) if elf64 else (
            kind, offset, 0, address, length, memory, flags, 4096)
        head += struct.pack('<IIQQQQQQ' if elf64 else '<8I', *ph)
    for i, data in enumerate((head, bytes(range(37)), bytes(range(8)), b'signed-test-hash')):
        (path / f'adsp.b{i:02d}').write_bytes(data)
    (path / 'adsp.mdt').write_bytes(head + b'signed-test-hash')
    return head


class AdspFirmwareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix='diting-adsp-')
        cls.addClassCleanup(cls.work.cleanup)
        cls.native = Path(cls.work.name) / 'native-mdt'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-g', '-Wall', '-Wextra',
                        '-Werror', '-fsanitize=address,undefined', '-I' + str(ROOT / 'drivers/DitingPower'),
                        str(ROOT / 'tests/diting_adsp_firmware.c'), str(ROOT / 'drivers/DitingPower/QcomMdt.c'),
                        '-o', str(cls.native)], check=True, capture_output=True, text=True)
        cls.decoder = Path(cls.work.name) / 'native-adsp-archive'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-g', '-Wall', '-Wextra',
                        '-Werror', '-fsanitize=address,undefined', '-I' + str(ROOT / 'drivers/DitingPower'),
                        str(ROOT / 'tests/diting_adsp_archive.c'), '-lz', '-o', str(cls.decoder)],
                       check=True, capture_output=True, text=True)

    def directory(self):
        work = tempfile.TemporaryDirectory(dir=self.work.name)
        self.addCleanup(work.cleanup)
        return Path(work.name)

    def test_joined_elf32_and_elf64_work_with_the_actual_native_pas_parser(self):
        for elf64 in (False, True):
            directory = self.directory()
            split_files(directory, elf64)
            data, report = firmware.join_split(directory, REGION)
            self.assertEqual(report['span'], 8192)
            self.assertEqual(report['destination'], 0x9fd00000)
            self.assertEqual(data[0x1000:0x1025], bytes(range(37)))
            self.assertEqual(data[0x3000:0x3020], bytes(32))
            joined = directory / 'adsp.mbn'; joined.write_bytes(data)
            result = subprocess.run([str(self.native), str(joined)], check=True,
                                    capture_output=True, text=True, timeout=10)
            self.assertIn('PASS native MDT', result.stdout)

    def test_missing_signature_or_load_piece_is_rejected(self):
        for piece in ('adsp.b03', 'adsp.b01'):
            directory = self.directory(); split_files(directory)
            (directory / piece).unlink()
            with self.assertRaises(ValueError):
                firmware.join_split(directory, REGION)

    def test_compressed_storage_preserves_signed_bytes_for_the_native_pas_parser(self):
        for elf64 in (False, True):
            directory = self.directory(); split_files(directory, elf64)
            data, _ = firmware.join_split(directory, REGION)
            archive = directory / 'adsp.pack'; archive.write_bytes(firmware.pack(data))
            decoded = directory / 'decoded.mbn'
            subprocess.run([str(self.decoder), str(archive), str(decoded)], check=True,
                           capture_output=True, text=True, timeout=10)
            self.assertEqual(decoded.read_bytes(), data)
            result = subprocess.run([str(self.native), str(decoded)], check=True,
                                    capture_output=True, text=True, timeout=10)
            self.assertIn('PASS native MDT', result.stdout)

    def test_compressed_truncation_checksum_trailing_bytes_and_expansion_limit_are_rejected(self):
        directory = self.directory(); split_files(directory)
        data, _ = firmware.join_split(directory, REGION)
        archive = firmware.pack(data)
        truncated = bytearray(archive[:-1]); struct.pack_into('<Q', truncated, 24, len(truncated) - 32)
        trailing = bytearray(archive + b'!'); struct.pack_into('<Q', trailing, 24, len(trailing) - 32)
        corrupt = bytearray(archive); corrupt[-1] ^= 1
        excessive = bytearray(archive); struct.pack_into('<Q', excessive, 16, firmware.LIMIT + 1)
        for blob in (truncated, trailing, corrupt, excessive):
            path = directory / 'bad.pack'; path.write_bytes(blob)
            result = subprocess.run([str(self.decoder), str(path), str(directory / 'bad.mbn')],
                                    capture_output=True, text=True, timeout=10)
            self.assertIn(result.returncode, (3, 4), result.stderr)

    def test_decoder_workspace_allocation_failure_is_clean(self):
        directory = self.directory(); split_files(directory)
        data, _ = firmware.join_split(directory, REGION)
        path = directory / 'adsp.pack'; path.write_bytes(firmware.pack(data))
        result = subprocess.run([str(self.decoder), str(path), str(directory / 'bad.mbn'), 'failalloc'],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 4, result.stderr)

    def test_truncation_arm_image_and_reservation_overflow_are_rejected(self):
        directory = self.directory(); split_files(directory)
        data, _ = firmware.join_split(directory, REGION)
        arm = bytearray(data); struct.pack_into('<H', arm, 18, 183)
        for blob, region in ((data[:-1], REGION), (arm, REGION), (data, dict(base=REGION['base'], size=4096))):
            with self.assertRaises(ValueError):
                firmware.describe(blob, region)

    def test_original_mdt_metadata_must_match_the_signed_pieces(self):
        directory = self.directory(); split_files(directory)
        mdt = directory / 'adsp.mdt'; mdt.write_bytes(mdt.read_bytes()[:-1] + b'?')
        with self.assertRaises(ValueError):
            firmware.join_split(directory, REGION)

    def test_overlapping_file_segments_and_excess_program_headers_are_rejected(self):
        directory = self.directory(); original = split_files(directory)
        overlap = bytearray(original); struct.pack_into('<I', overlap, 52 + 2 * 32 + 4, 0x1000)
        too_many = bytearray(original); struct.pack_into('<H', too_many, 44, 65)
        for head in (overlap, too_many):
            with self.assertRaises(ValueError):
                firmware.headers(head)


if __name__ == '__main__':
    unittest.main()
