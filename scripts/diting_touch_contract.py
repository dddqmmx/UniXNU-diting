"""Diting ST61Y/GT9916S resources from the measured live DT.

The panel selector is read on hardware. A DT listing both variants is not
evidence of which chip is installed. This module only validates local inputs.
"""
import importlib.util
from pathlib import Path
import struct

COMPATIBLE = b'diting,st61y\0diting,goodix-berlin-d\0'
RESOURCES = {
    'spi-base': (8, 0x990000), 'tlmm-base': (8, 0xF000000),
    'gcc-base': (8, 0x100000), 'rsc-base': (8, 0x17A20000),
    'cmd-db-base': (8, 0x80860000), 'gpio-offset': (4, 0x100000),
    'avdd-pin': (4, 71), 'id-pin': (4, 119), 'spi-max-hz': (4, 12000000),
    'max-x': (4, 12200), 'max-y': (4, 27120), 'super-resolution': (4, 1),
    'vdd-mv': (4, 1800), 'vdd-mode': (4, 7),
}
PINS = (16, 17, 18, 19, 21, 20)


def adt_touch_properties():
    return [('compatible', COMPATIBLE),
            *[(name, value.to_bytes(width, 'little'))
              for name, (width, value) in RESOURCES.items()],
            ('pins', struct.pack('<6I', *PINS)), ('vdd-resource', b'ldoc2\0')]


def validate_adt_touch(properties):
    for name, expected in adt_touch_properties():
        if properties.get(name) != expected:
            raise ValueError('Diting touch resource differs from the measured board: ' + name)


def validate_touch_nodes(nodes):
    """Accept the original live tree or the format-2 board's matching resources."""
    spi = '/soc/spi@990000'
    touch = spi + '/xiaomi_touch@0'
    if spi not in nodes or touch not in nodes:
        raise ValueError('Diting QUP0 SE4/touch node is missing')
    bus, panel = nodes[spi], nodes[touch]

    def cells(p, name):
        raw = p.get(name, b'')
        if not raw or len(raw) % 4:
            raise ValueError('Missing/invalid Diting touch cells: ' + name)
        return list(struct.unpack('>' + 'I' * (len(raw) // 4), raw))

    def scalar(p, name, expected):
        if cells(p, name) != [expected]:
            raise ValueError('Unexpected Diting touch value: ' + name)

    def first_range(path, properties):
        parent = nodes.get(path.rsplit('/', 1)[0] or '/', {})
        address_cells = cells(parent, '#address-cells')[0] if '#address-cells' in parent else 2
        size_cells = cells(parent, '#size-cells')[0] if '#size-cells' in parent else 1
        raw = cells(properties, 'reg')
        if address_cells not in (1, 2) or size_cells not in (1, 2) or len(raw) < address_cells + size_cells:
            raise ValueError('Invalid touch provider register encoding')
        address = int.from_bytes(struct.pack('>' + 'I' * address_cells, *raw[:address_cells]), 'big')
        size = int.from_bytes(struct.pack('>' + 'I' * size_cells, *raw[address_cells:address_cells + size_cells]), 'big')
        return address, size

    if (first_range(spi, bus) != (0x990000, 0x4000) or
            bus.get('status', b'okay\0') not in (b'okay\0', b'ok\0') or
            panel.get('status', b'okay\0') not in (b'okay\0', b'ok\0') or
            b'st,spi' not in panel.get('compatible', b'').split(b'\0') or
            b'goodix,gt9916S' not in panel.get('compatible', b'').split(b'\0')):
        raise ValueError('Diting touch requires the real ST/Goodix SPI0 variants')
    for name, value in [('reg', 0), ('spi-max-frequency', 12000000),
                        ('fts,x-max', 12200), ('fts,y-max', 27120),
                        ('fts,support-super-resolution', 1),
                        ('goodix,panel-max-x', 12200), ('goodix,panel-max-y', 27120),
                        ('goodix,ts-id-gpio-match-value', 0)]:
        scalar(panel, name, value)
    if 'spi-cpha' in panel or 'spi-cpol' in panel:
        raise ValueError('Diting touch SPI must use mode 0')
    by_phandle = {}
    for path, properties in nodes.items():
        if 'phandle' in properties:
            values = cells(properties, 'phandle')
            if len(values) != 1 or values[0] in by_phandle:
                raise ValueError('Invalid/duplicate touch provider phandle')
            by_phandle[values[0]] = path

    def provider(handle):
        if len(handle) != 1 or handle[0] not in by_phandle:
            raise ValueError('Diting touch provider is missing')
        path = by_phandle[handle[0]]
        return path, nodes[path]

    for name, pin, flags in [('fts,reset-gpio', 20, 0), ('fts,irq-gpio', 21, 8),
                             ('fts,avdd-gpio', 71, 0), ('goodix,reset-gpio', 20, 0),
                             ('goodix,irq-gpio', 21, 8), ('goodix,avdd-gpio', 71, 0),
                             ('goodix,ts-id-gpio', 119, 1)]:
        gpio = cells(panel, name)
        if len(gpio) != 3 or gpio[1:] != [pin, flags]:
            raise ValueError('Unexpected Diting touch pin: ' + name)
        controller_path, controller = provider(gpio[:1])
        if first_range(controller_path, controller)[0] != 0xF000000:
            raise ValueError('Diting touch GPIO belongs to another controller')
        scalar(controller, '#gpio-cells', 2)
    supply_path, supply = provider(cells(panel, 'vdd-supply'))
    parent = nodes.get(supply_path.rsplit('/', 1)[0], {})
    if (supply.get('regulator-name') != b'pm8350c_l2\0' or
            parent.get('qcom,resource-name') != b'ldoc2\0' or
            parent.get('qcom,regulator-type') != b'pmic5-ldo\0'):
        raise ValueError('Diting touch vdd must use pm8350c_l2 / ldoc2')
    for name, value in [('regulator-min-microvolt', 1800000),
                        ('regulator-max-microvolt', 1980000),
                        ('qcom,init-voltage', 1800000), ('qcom,init-mode', 4)]:
        scalar(supply, name, value)
    return {'result': 'PASS', 'spi': spi, 'panel': touch, 'supply': supply_path,
            'vdd_resource': 'ldoc2', 'vdd_mv': 1800, 'vdd_mode': 7,
            'avdd_pin': 71, 'selector_pin': 119, 'selector_high': 'ST61Y',
            'selector_low': 'GT9916S', 'max_x': 12200, 'max_y': 27120,
            'hardware_tested': False}


def validate_touch_dtb(path):
    source = Path(__file__).resolve().parent / 'prepare-diting-wifi.py'
    spec = importlib.util.spec_from_file_location('diting_touch_fdt', source)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    result = validate_touch_nodes(module.fdt_nodes(Path(path).read_bytes()))
    result['source'] = str(path)
    return result
