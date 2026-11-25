"""Run the actual L12 codec/FIFO owner against register events; no phone I/O."""
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

DEVICE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(DEVICE / 'scripts'))
from diting_adt_contract import parse_adt


class BacklightTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='diting-backlight-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.root = Path(cls.directory.name)
        cls.binary = cls.root / 'backlight'
        subprocess.run([
            os.environ.get('CXX', 'clang++'), '-std=c++17', '-O1', '-g',
            '-Wall', '-Wextra', '-Werror', '-pedantic', '-fsanitize=address,undefined',
            '-fno-omit-frame-pointer', '-I' + str(DEVICE / 'drivers/DitingPower'),
            str(DEVICE / 'tests/diting_backlight_protocol.cpp'), '-o', str(cls.binary),
        ], check=True, capture_output=True, text=True)

    def run_case(self, case, *args):
        result = subprocess.run([str(self.binary), case, *map(str, args)], check=True,
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.stdout.strip(), f'PASS {case}')

    def test_signed_levels_and_fade_endpoints(self):
        self.run_case('ranges')

    def test_all_l12_levels_keep_wire_byte_order_and_read_units(self):
        self.run_case('wire')

    def test_fifo_packets_padding_hs_lp_and_register_restoration(self):
        self.run_case('packets')

    def test_busy_display_never_clears_readback_or_touches_fifo(self):
        self.run_case('busy')

    def test_timeout_and_idle_without_completion_retain_owner(self):
        self.run_case('retained')

    def test_bta_must_retire_before_readback_fifo_reuse(self):
        self.run_case('bta')

    def test_retired_ack_errors_preserve_unrelated_display_faults(self):
        self.run_case('errors')

    def test_latched_ack_error_requires_zero_write_clear(self):
        self.run_case('latched-ack')

    def test_wake_policy_restores_cached_brightness_and_honors_enable(self):
        self.run_case('wake-policy')

    def test_dsi_command_error_retries_once_when_published(self):
        self.run_case('retry-error')

    def test_display_power_and_timing_changes_survive_command(self):
        self.run_case('display-owner')

    def test_corebrightness_guard_handles_late_service_and_rejects_foreign_adt(self):
        source = (DEVICE / 'userland/ui_compat18.c').read_text()
        start = source.index('static bool brightness18_diting_board(void)')
        end = source.index('static void brightness18_image(', start)
        (self.root / 'brightness18_guard.inc').write_text(source[start:end])
        binary = self.root / 'backlight-compat'
        subprocess.run([
            os.environ.get('CC', 'clang'), '-std=gnu11', '-O1', '-g',
            '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
            '-I' + str(self.root), str(DEVICE / 'tests/diting_backlight_compat.c'), '-o', str(binary),
        ], check=True, capture_output=True, text=True)
        result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=20)
        self.assertEqual(result.stdout.strip(), 'PASS compatibility')

    def test_actual_adt_producer_matches_driver_board_guard(self):
        output = self.root / 'brightness.adt'
        env = dict(os.environ)
        env.pop('DITING_HARDWARE_DTB', None)
        subprocess.run([
            sys.executable, str(DEVICE / 'scripts/make-diting-adt-ipados18.py'),
            '--cpus', '8', '--dram-size', '0x200000000',
            '--skip-image-verification', '--output', str(output),
        ], env=env, check=True, capture_output=True, text=True, timeout=20)
        wire = self.root / 'backlight-adt.bin'
        wire.write_bytes(parse_adt(output.read_bytes())['/chosen']['diting-dsi-backlight'])
        self.run_case('board-file', wire)

    def test_initial_saved_zero_migrates_once_without_overriding_later_user_choices(self):
        source = (DEVICE / 'userland/ui_compat18.c').read_text()
        start = source.index('static void brightness18_initial_preference(void)')
        end = source.index('\n/* This build', start)
        (self.root / 'brightness18_migration.inc').write_text(source[start:end])
        binary = self.root / 'backlight-migration'
        subprocess.run([
            os.environ.get('CC', 'clang'), '-std=gnu11', '-O1', '-g',
            '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
            '-I' + str(self.root), str(DEVICE / 'tests/diting_backlight_migration.c'), '-o', str(binary),
        ], check=True, capture_output=True, text=True)
        result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=20)
        self.assertEqual(result.stdout.strip(), 'PASS migration')

    def test_both_diting_panel_variants_match_configured_brightness(self):
        dtb = self.root / 'panels.dtb'
        subprocess.run([
            'dtc', '-I', 'dts', '-O', 'dtb', '-i', str(DEVICE / 'migration/format2/soc-seed/dts'),
            '-o', str(dtb), str(DEVICE / 'dts/diting.dts'),
        ], check=True, capture_output=True, text=True, timeout=20)
        spec = importlib.util.spec_from_file_location('backlight_fdt', DEVICE / 'scripts/prepare-diting-wifi.py')
        fdt = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(fdt)
        nodes = fdt.fdt_nodes(dtb.read_bytes())
        config = json.loads((DEVICE / 'configs/diting-display-sm8475.json').read_text())
        brightness = config['brightness']
        panels = [props for path, props in nodes.items() if path.endswith((
            '/qcom,mdss_dsi_l12_36_02_0b_dsc_cmd', '/qcom,mdss_dsi_l12_42_02_0a_dsc_cmd'))]
        self.assertEqual(len(panels), 2)
        for panel in panels:
            self.assertEqual(panel['qcom,mdss-dsi-bl-pmic-control-type'], b'bl_ctrl_dcs\0')
            self.assertEqual(struct.unpack('>I', panel['qcom,mdss-dsi-bl-min-level'])[0], brightness['minimum_nonzero'])
            self.assertEqual(struct.unpack('>I', panel['qcom,mdss-dsi-bl-max-level'])[0], brightness['maximum'])
            self.assertIn('qcom,mdss-dsi-bl-inverted-dbv', panel)
            self.assertEqual(struct.unpack('>I', panel['qcom,mdss-dsi-virtual-channel-id'])[0], brightness['channel'])


if __name__ == '__main__':
    unittest.main()
