"""Exercise the real PMIC codec, publisher policy and inbox; no phone I/O."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

DEVICE = Path(__file__).resolve().parents[1]


class BatteryProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='diting-battery-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / 'battery-protocol'
        subprocess.run([
            os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-Wall', '-Wextra',
            '-Werror', '-pedantic', '-fsanitize=address,undefined',
            '-fno-omit-frame-pointer', '-g',
            '-I' + str(DEVICE / 'drivers/DitingPower'),
            str(DEVICE / 'tests/diting_battery_protocol.c'),
            str(DEVICE / 'drivers/DitingPower/DitingBattmgr.c'),
            str(DEVICE / 'drivers/DitingPower/DitingUcsi.c'),
            '-o', str(cls.binary),
        ], check=True, capture_output=True, text=True)

    def run_case(self, case):
        result = subprocess.run([str(self.binary), case], check=True,
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.stdout.strip(), f'PASS {case}')

    def test_xiaomi_wire_ids_units_and_core_publication(self):
        self.run_case('wire')

    def test_response_owner_type_opcode_id_and_exact_lengths(self):
        self.run_case('frames')

    def test_signed_measurements_ranges_and_sentinels(self):
        self.run_case('ranges')

    def test_request_timeout_queue_refusal_and_late_reply(self):
        self.run_case('timeouts')

    def test_optional_errors_do_not_hide_capacity_or_repeat_slow_reads(self):
        self.run_case('optional')

    def test_notification_burst_keeps_pending_slow_refresh(self):
        self.run_case('notifications')

    def test_committed_values_survive_partial_next_cycle(self):
        self.run_case('immutable')

    def test_source_debounce_full_state_and_missing_charger_fields(self):
        self.run_case('source')

    def test_real_zero_unknown_capacity_and_full_pack_vbus(self):
        self.run_case('empty')

    def test_coalesced_reconnect_discards_old_inbox_and_bounds_frames(self):
        self.run_case('inbox')

    def test_malformed_frame_sanitizer_coverage(self):
        self.run_case('fuzz')

    def test_usb_data_role_classification_and_ucsi_resolution(self):
        self.run_case('usb_data_role')

    def test_pmic_glink_multi_owner_routing_and_epoch(self):
        self.run_case('pmic_glink')

    def test_ucsi_glink_state_machine_and_connector_status(self):
        self.run_case('ucsi')

    def test_battmgr_safe_whitelisted_writes_and_thermal_hysteresis(self):
        self.run_case('writes')

    def test_weak_usb_net_drain_critical_alert(self):
        self.run_case('weak_drain')


if __name__ == '__main__':
    unittest.main()
