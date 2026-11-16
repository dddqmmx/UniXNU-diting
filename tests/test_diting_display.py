"""Exercise the production read-only SM8475 reader, ADT and CDC length bound."""
import os
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from diting_adt_contract import parse_adt
spec = importlib.util.spec_from_file_location('display_decoder', ROOT / 'scripts/decode-diting-display.py')
decoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decoder)


class DisplayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='diting-display-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.root = Path(cls.directory.name)
        cls.binary = cls.root / 'display'
        subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++17', '-O1', '-g',
                        '-Wall', '-Wextra', '-Werror', '-pedantic', '-fsanitize=address,undefined',
                        '-I' + str(ROOT / 'drivers/DitingPower'), '-I' + str(ROOT / 'drivers/DitingUsb'),
                        str(ROOT / 'tests/diting_display.cpp'), '-o', str(cls.binary)], check=True, capture_output=True)

    def test_gates_bounds_clock_changes_and_timestamp_coherence(self):
        for case in ('board', 'clock-off', 'dsi-off', 'stable', 'fast', 'torn', 'disabled', 'changed', 'io', 'bounds', 'copy-snapshot'):
            with self.subTest(case=case):
                result = subprocess.run([str(self.binary), case], check=True, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.stdout.strip(), 'PASS ' + case)

    def test_full_reply_and_truncation_always_preserve_the_end_frame(self):
        for case in ('format', 'trailer'):
            result = subprocess.run([str(self.binary), case], check=True, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.stdout.strip(), 'PASS ' + case)

    def test_actual_adt_has_the_exact_40_byte_board_contract(self):
        output = self.root / 'display.adt'
        env = dict(os.environ)
        env.pop('DITING_HARDWARE_DTB', None)
        subprocess.run([sys.executable, str(ROOT / 'scripts/make-diting-adt-ipados18.py'),
                        '--cpus', '8', '--dram-size', '0x200000000', '--skip-image-verification',
                        '--output', str(output)], env=env, check=True, capture_output=True, timeout=20)
        wire = self.root / 'display-board.bin'
        wire.write_bytes(parse_adt(output.read_bytes())['/chosen']['diting-dpu-timing'])
        subprocess.run([str(self.binary), 'board-file', str(wire)], check=True, capture_output=True, timeout=10)

    def test_decoder_requires_a_complete_reply_and_preserves_the_physical_limit(self):
        reply = subprocess.check_output([str(self.binary), 'reply'], text=True, timeout=10)
        report = decoder.decode(reply)
        self.assertFalse(report['hardware_performance_pass'])
        self.assertIsNone(report['physical_presented_frame_count'])
        self.assertEqual(report['timestamp_units'], 'RAW')
        self.assertEqual(len(report['interfaces']), 2)
        for bad in (reply.replace('DitingUSB END display', ''),
                    '\n'.join(line for line in reply.splitlines() if not line.startswith('CTL index=5')),
                    reply.replace('timestamp_units=RAW', 'timestamp_units=NS')):
            with self.assertRaises(ValueError):
                decoder.decode(bad)


if __name__ == '__main__':
    unittest.main()
