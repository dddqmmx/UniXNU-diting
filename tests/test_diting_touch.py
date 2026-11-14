"""Host protocol/ownership tests; no phone I/O or generated image verification."""
import importlib.util
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

DEVICE = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('touch_contract', DEVICE / 'scripts/diting_touch_contract.py')
contract = importlib.util.module_from_spec(spec)
spec.loader.exec_module(contract)
spec = importlib.util.spec_from_file_location('touch_fdt', DEVICE / 'scripts/prepare-diting-wifi.py')
fdt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fdt)


class TouchProtocolTests(unittest.TestCase):
    def compile_and_run(self, name):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / name
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                            '-Werror', '-pedantic', '-fsanitize=address,undefined', '-g', '-pthread',
                            '-I' + str(DEVICE / 'drivers/DitingTouch'),
                            '-I' + str(DEVICE / 'drivers/DitingPower'),
                            str(DEVICE / 'tests' / (name + '.c')), '-o', str(output)],
                           check=True, capture_output=True, text=True)
            result = subprocess.run([str(output)], check=True, capture_output=True, text=True, timeout=30)
            self.assertIn('PASS', result.stdout)

    def test_st61y_wire_initialization_and_contacts(self):
        self.compile_and_run('diting_st61y_protocol')

    def test_geni_fifo_ownership_and_recovery(self):
        self.compile_and_run('diting_geni_spi')

    def test_shared_gcc_votes(self):
        self.compile_and_run('diting_gcc_votes')


class TouchResourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        output = Path(cls.directory.name) / 'hardware.dtb'
        subprocess.run(['dtc', '-I', 'dts', '-O', 'dtb', '-i',
                        str(DEVICE / 'migration/format2/soc-seed/dts'), '-o', str(output),
                        str(DEVICE / 'dts/diting.dts')],
                       check=True, capture_output=True, text=True)
        cls.nodes = fdt.fdt_nodes(output.read_bytes())

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def test_board_touch_resources(self):
        result = contract.validate_touch_nodes(self.nodes)
        self.assertEqual(result['vdd_resource'], 'ldoc2')
        self.assertEqual((result['max_x'], result['max_y']), (12200, 27120))
        self.assertFalse(result['hardware_tested'])

    def test_adt_properties_are_exact(self):
        properties = dict(contract.adt_touch_properties())
        contract.validate_adt_touch(properties)
        for name in properties:
            with self.subTest(property=name):
                bad = dict(properties)
                bad[name] = properties[name] + b'\0'
                with self.assertRaises(ValueError):
                    contract.validate_adt_touch(bad)

    def test_spi_parent_cell_widths(self):
        nodes = {path: dict(properties) for path, properties in self.nodes.items()}
        nodes['/soc']['#address-cells'] = struct.pack('>I', 1)
        nodes['/soc']['#size-cells'] = struct.pack('>I', 1)
        nodes['/soc/spi@990000']['reg'] = struct.pack('>II', 0x990000, 0x4000)
        contract.validate_touch_nodes(nodes)
        nodes['/soc']['#address-cells'] = struct.pack('>I', 2)
        with self.assertRaises(ValueError):
            contract.validate_touch_nodes(nodes)

    def test_wrong_pins_supply_and_protocol_are_rejected(self):
        panel = '/soc/spi@990000/xiaomi_touch@0'
        changes = [('fts,avdd-gpio', [0, 70, 0]), ('goodix,ts-id-gpio', [0, 118, 1]),
                   ('fts,x-max', [2711]), ('fts,support-super-resolution', [0]),
                   ('spi-max-frequency', [24000000]), ('vdd-supply', [0xdeadbeef])]
        for name, values in changes:
            with self.subTest(property=name):
                nodes = {path: dict(properties) for path, properties in self.nodes.items()}
                if 'gpio' in name:
                    values = [struct.unpack_from('>I', nodes[panel][name])[0], *values[1:]]
                nodes[panel][name] = struct.pack('>' + 'I' * len(values), *values)
                with self.assertRaises(ValueError):
                    contract.validate_touch_nodes(nodes)
        nodes = {path: dict(properties) for path, properties in self.nodes.items()}
        nodes[panel]['spi-cpha'] = b''
        with self.assertRaises(ValueError):
            contract.validate_touch_nodes(nodes)


if __name__ == '__main__':
    unittest.main()
