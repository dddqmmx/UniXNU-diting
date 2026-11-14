"""Validated RAM inputs for the selected Diting hardware variant."""
import json
from pathlib import Path
import subprocess

DEFAULT_MEMORY_MAP = Path(__file__).resolve().parents[1] / 'configs/diting-memory-sm8475.json'
MAX_ADDRESS = (1 << 64) - 1


def ranges(entries, label):
    result = []
    for entry in entries:
        base = int(str(entry['base']), 0)
        size = int(str(entry['size']), 0)
        if size <= 0 or base < 0 or base > MAX_ADDRESS - size or (base | size) & 4095:
            raise ValueError(f'{label}: invalid page-aligned range {base:#x}+{size:#x}')
        result.append((base, size))
    result.sort()
    for (base, size), (other, _) in zip(result, result[1:]):
        if base + size > other:
            raise ValueError(f'{label}: overlapping ranges at {base:#x} and {other:#x}')
    return result


def covered(base, size, banks):
    end = base + size
    for start, length in banks:
        if start <= base < start + length:
            base = min(end, start + length)
            if base == end:
                return True
    return False


def hardware_banks(dtb):
    def get(node, prop, kind='x'):
        return subprocess.check_output(['fdtget', '-t', kind, str(dtb), node, prop],
                                       text=True, stderr=subprocess.DEVNULL).strip()
    if get('/', '#address-cells') != '2' or get('/', '#size-cells') != '2':
        raise ValueError('Diting RAM requires two address and size cells')
    entries = []
    nodes = subprocess.check_output(['fdtget', '-l', str(dtb), '/'], text=True).splitlines()
    for name in nodes:
        node = '/' + name
        try:
            if get(node, 'device_type', 's') != 'memory':
                continue
        except subprocess.CalledProcessError:
            continue
        try:
            if get(node, 'status', 's') not in ('ok', 'okay'):
                continue
        except subprocess.CalledProcessError:
            pass
        cells = [int(value, 16) for value in get(node, 'reg').split()]
        if not cells or len(cells) % 4:
            raise ValueError(f'{node}: RAM reg must contain complete 64-bit pairs')
        for index in range(0, len(cells), 4):
            a, b, c, d = cells[index:index + 4]
            entries.append({'base': hex((a << 32) | b), 'size': hex((c << 32) | d)})
    return ranges(entries, 'hardware RAM')


def load_memory_map(path=DEFAULT_MEMORY_MAP, hardware_dtb=None):
    data = json.loads(Path(path).read_text())
    if data.get('format') != 'diting-sm8475-dram-v1':
        raise ValueError('unsupported Diting RAM profile')
    dram = ranges(data['dram'], 'DRAM')
    allowed = ranges(data['allocatable'], 'allocatable RAM')
    reserved = ranges(data['reserved'], 'reserved RAM')
    firmware = ranges(data.get('firmware_reserved', []), 'firmware reservations')
    physical = int(str(data['physical_bytes']), 0)
    if not dram or not allowed or max(len(dram), len(allowed)) > 64:
        raise ValueError('Diting RAM requires 1..64 physical and allocatable ranges')
    if physical < sum(size for _, size in dram) or physical > MAX_ADDRESS - dram[0][0]:
        raise ValueError('nominal physical DRAM size is inconsistent with its banks')
    data['physical_bytes'] = physical
    if 'reported_bank_bytes' in data and data['reported_bank_bytes'] != sum(size for _, size in dram):
        raise ValueError('recorded RAM bank sum is inconsistent with its ranges')
    for base, size in allowed:
        if not covered(base, size, dram):
            raise ValueError(f'allocatable RAM {base:#x}+{size:#x} exceeds physical banks')
        for other, length in reserved + firmware:
            if base < other + length and other < base + size:
                raise ValueError(f'allocatable RAM {base:#x}+{size:#x} overlaps a reservation')
    if hardware_dtb is not None:
        try:
            actual = hardware_banks(hardware_dtb)
        except (OSError, subprocess.CalledProcessError) as error:
            raise ValueError(f'{hardware_dtb}: cannot read the hardware RAM banks') from error
        if actual != dram:
            raise ValueError(f'{hardware_dtb}: RAM banks differ from {path}; select the matching hardware profile')
    return data, dram, allowed
