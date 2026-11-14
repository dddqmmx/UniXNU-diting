#!/usr/bin/env python3
"""Teach the pinned generic ADT encoder to keep RAM banks and capacity distinct."""
import argparse
from pathlib import Path

OLD = '''    if len(memory) != 1 or len(registers(memory[0])) != 1:
        raise Error('ADT handoff requires one contiguous DRAM range')
    dram_base, dram_size = registers(memory[0])[0]
    if payload.get('dram_size') is not None and int(str(payload['dram_size']), 0) != dram_size:
        raise Error('payload.dram_size differs from the hardware DTS')
'''

NEW = '''    # Diting: validated banks; explicit nominal capacity for discontiguous RAM.
    banks = sorted(bank for entry in memory for bank in registers(entry))
    if not banks:
        raise Error('ADT handoff requires physical RAM banks')
    previous_end = 0
    for base, size in banks:
        if (size <= 0 or base < previous_end or base > (1 << 64) - 1 - size or
                (base | size) & 4095):
            raise Error('Invalid or overlapping hardware RAM banks')
        previous_end = base + size
    dram_base = banks[0][0]
    physical = payload.get('dram_size')
    if len(banks) == 1:
        dram_size = banks[0][1]
        if physical is not None and int(str(physical), 0) != dram_size:
            raise Error('payload.dram_size differs from the hardware DTS')
    else:
        if physical is None:
            raise Error('Discontiguous RAM requires explicit nominal payload.dram_size')
        dram_size = int(str(physical), 0)
        if dram_size < sum(size for _, size in banks):
            raise Error('Nominal DRAM capacity is smaller than the reported bank sum')
    if dram_size <= 0 or dram_base > (1 << 64) - 1 - dram_size:
        raise Error('Invalid nominal DRAM capacity')
'''


def install(unixnu):
    path = Path(unixnu) / 'scripts/unixnu/devicetree.py'
    text = path.read_text()
    if text.count(NEW) == 1 and OLD not in text:
        return False
    if text.count(OLD) != 1 or 'Diting: validated banks;' in text:
        raise RuntimeError(f'{path}: ADT RAM logic changed; review before applying the compatibility patch')
    path.write_text(text.replace(OLD, NEW, 1))
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--unixnu', required=True, type=Path)
    args = parser.parse_args()
    print('banked RAM compatibility patch applied' if install(args.unixnu) else 'banked RAM compatibility patch already present')


if __name__ == '__main__':
    main()
