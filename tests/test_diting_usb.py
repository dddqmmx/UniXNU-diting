"""Exercise inherited DMA retirement and diagnostic command boundaries."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

DEVICE = Path(__file__).resolve().parents[1]


class UsbProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='diting-usb-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / 'usb-protocol'
        subprocess.run([
            os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-Wall', '-Wextra',
            '-Werror', '-pedantic', '-fsanitize=address,undefined', '-g',
            '-I' + str(DEVICE / 'drivers/DitingUsb'), str(DEVICE / 'tests/diting_usb_protocol.cpp'),
            '-o', str(cls.binary),
        ], check=True, capture_output=True, text=True)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], check=True, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.stdout.strip(), 'PASS ' + name)

    def test_real_resources_retire_before_stop_and_event_buffers_clear_after_halt(self):
        self.run_case('retire')

    def test_already_halted_owner_needs_no_endpoint_cancellation(self):
        self.run_case('halted')

    def test_wrong_id_role_or_ring_count_never_reprograms_controller(self):
        self.run_case('prerequisites')

    def test_timeout_active_command_and_bad_event_count_retain_old_dma(self):
        self.run_case('timeouts')

    def test_memory_command_overflow_bounds_and_malformed_input(self):
        self.run_case('commands')


if __name__ == '__main__':
    unittest.main()
