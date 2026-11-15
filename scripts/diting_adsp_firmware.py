"""Build an explicitly selected, signed Diting PAS1 image without filesystem writes.

Only build artifacts contain firmware bytes. The file reader and PAS verifier
remain the consumers; this module neither hashes images nor touches the phone.
"""
import json
import os
from pathlib import Path
import struct
import subprocess
import zlib

from diting_memory import DEFAULT_MEMORY_MAP, load_memory_map

LIMIT = 64 * 1024 * 1024
TYPE_MASK, TYPE_HASH, RELOCATABLE = 7 << 24, 2 << 24, 1 << 27


def pack(data):
    if not 0 < len(data) <= LIMIT:
        raise ValueError('ADSP decoded image exceeds its bounded kernel storage')
    compressed = zlib.compress(data, 9)
    if len(compressed) > LIMIT or zlib.decompress(compressed) != data:
        raise ValueError('ADSP kernel storage did not preserve the signed image')
    return struct.pack('<8sIIQQ', b'DTADSP01', 1, 0, len(data), len(compressed)) + compressed


def board_contract(out, live=None):
    profile, _, _ = load_memory_map(DEFAULT_MEMORY_MAP, live)
    regions = [r for r in profile['reserved'] if r['name'].startswith('adsp_region@')]
    if len(regions) != 1:
        raise ValueError('Exactly one recorded Diting ADSP reservation is required')
    base, size = (int(regions[0][key], 0) for key in ('base', 'size'))
    record = dict(base=base, size=size, pas=1, remote_pid=2,
                  profile=str(DEFAULT_MEMORY_MAP), live=str(live) if live else None,
                  live_check='SKIP' if not live else 'PASS', hardware_tested=False)
    if live:
        def get(node, prop, kind='x'):
            return subprocess.check_output(['fdtget', '-t', kind, str(live), node, prop],
                                           text=True, stderr=subprocess.DEVNULL, timeout=10).strip()
        node = '/soc/remoteproc-adsp@03000000'
        if 'qcom,cape-adsp-pas' not in get(node, 'compatible', 's').split():
            raise ValueError('Selected live DT is not the Diting cape PAS1 endpoint')
        phandle = get(node, 'memory-region')
        children = subprocess.check_output(['fdtget', '-l', str(live), '/reserved-memory'],
                                           text=True, timeout=10).splitlines()
        matches = []
        for name in children:
            path = '/reserved-memory/' + name
            try:
                if get(path, 'phandle') == phandle:
                    matches.append(path)
            except subprocess.CalledProcessError:
                continue
        if len(matches) != 1:
            raise ValueError('ADSP memory-region must name one actual reservation')
        parent = '/reserved-memory'
        if get(parent, '#address-cells') != '2' or get(parent, '#size-cells') != '2':
            raise ValueError('Unsupported ADSP reservation address cells')
        cells = [int(v, 16) for v in get(matches[0], 'reg').split()]
        if len(cells) != 4 or ((cells[0] << 32) | cells[1], (cells[2] << 32) | cells[3]) != (base, size):
            raise ValueError('Live ADSP carveout differs from the loader RAM profile')
        get(matches[0], 'no-map')
        edge = node + '/glink-edge'
        if get(edge, 'qcom,remote-pid') != '2' or get(edge, 'qcom,glink-label', 's') != 'lpass':
            raise ValueError('Selected ADSP GLINK is not the Diting LPASS edge')
        pmic = edge + '/qcom,pmic_glink_rpmsg'
        if get(pmic, 'qcom,glink-channels', 's') != 'PMIC_RTR_ADSP_APPS':
            raise ValueError('Selected live DT lacks the real PMIC ADSP channel')
        record['node'], record['reservation'] = node, matches[0]
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    (out / 'DitingAdspBoard.h').write_text(
        '/* Generated from the recorded loader RAM profile and selected live DT. */\n'
        f'#define DITING_ADSP_BASE UINT64_C({base:#x})\n'
        f'#define DITING_ADSP_SIZE UINT64_C({size:#x})\n')
    (out / 'adsp-board.json').write_text(json.dumps(record, indent=2) + '\n')
    return record


def headers(data):
    if len(data) < 52 or data[:4] != b'\x7fELF' or data[5:7] != b'\x01\x01':
        raise ValueError('ADSP input must be a little-endian ELF image')
    elf64 = data[4] == 2
    if data[4] not in (1, 2) or len(data) < (64 if elf64 else 52):
        raise ValueError('Truncated or unsupported ADSP ELF header')
    if struct.unpack_from('<H', data, 18)[0] != 164:
        raise ValueError('ADSP firmware must target Hexagon, not an ARM kernel')
    if elf64:
        phoff = struct.unpack_from('<Q', data, 32)[0]
        ehsize, entsize, count = struct.unpack_from('<3H', data, 52)
    else:
        phoff = struct.unpack_from('<I', data, 28)[0]
        ehsize, entsize, count = struct.unpack_from('<3H', data, 40)
    if ehsize != (64 if elf64 else 52) or entsize != (56 if elf64 else 32) or not 1 <= count <= 64:
        raise ValueError('Invalid ADSP program header geometry')
    if phoff < ehsize or phoff + entsize * count > len(data):
        raise ValueError('ADSP program headers lie outside the supplied bytes')
    pieces = []
    for i in range(count):
        values = struct.unpack_from('<IIQQQQQQ' if elf64 else '<8I', data, phoff + i * entsize)
        if elf64:
            kind, flags, offset, _, address, length, memory, _ = values
        else:
            kind, offset, _, address, length, memory, flags, _ = values
        if length > LIMIT or (length and offset > LIMIT - length) or address + memory > (1 << 64) - 1:
            raise ValueError('ADSP segment exceeds the bounded firmware address space')
        if kind == 1 and flags & TYPE_MASK != TYPE_HASH and length > memory:
            raise ValueError('ADSP loadable segment is larger than its memory extent')
        pieces.append(dict(index=i, kind=kind, offset=offset, bytes=length,
                           memory=memory, address=address, flags=flags))
    first = pieces[0]
    if first['offset'] or first['kind'] != 0 or first['bytes'] < phoff + count * entsize or first['bytes'] > len(data):
        raise ValueError('ADSP first segment must preserve the ELF and program headers')
    hashed = [p for p in pieces if p['flags'] & TYPE_MASK == TYPE_HASH]
    if len(hashed) != 1 or not hashed[0]['bytes']:
        raise ValueError('One actual PAS signature/hash segment is required')
    used = sorted((p['offset'], p['offset'] + p['bytes']) for p in pieces if p['bytes'])
    if any(high > other for (_, high), (other, _) in zip(used, used[1:])):
        raise ValueError('ADSP file segments overlap')
    return pieces


def describe(data, region):
    pieces = headers(data)
    if len(data) > LIMIT or any(p['bytes'] and p['offset'] + p['bytes'] > len(data) for p in pieces):
        raise ValueError('ADSP input is truncated at a firmware segment')
    loads = [p for p in pieces if p['kind'] == 1 and p['memory'] and p['flags'] & TYPE_MASK != TYPE_HASH]
    if not loads:
        raise ValueError('ADSP firmware has no loadable segments')
    low = min(p['address'] for p in loads)
    high = max(p['address'] + p['memory'] for p in loads)
    span = ((high + 4095) & ~4095) - low
    relocated = any(p['flags'] & RELOCATABLE for p in loads)
    if low & 4095 or span > region['size']:
        raise ValueError('Signed ADSP image does not fit the actual Diting carveout')
    for p in loads:
        offset = p['address'] - (low if relocated else region['base'])
        if offset < 0 or offset + p['memory'] > region['size']:
            raise ValueError('ADSP segment lies outside its actual reserved memory')
    return dict(bytes=len(data), span=span, relocatable=relocated, pieces=pieces,
                source_address=low, destination=region['base'], pas_authentication='required on device')


def join_split(directory, region):
    directory = Path(directory)
    first = directory / 'adsp.b00'
    if first.is_symlink() or not first.is_file():
        raise ValueError('Actual adsp.b00 is required for a split image')
    head = first.read_bytes()
    pieces = headers(head)
    if len(head) != pieces[0]['bytes']:
        raise ValueError('adsp.b00 size differs from its signed program header')
    size = max(p['offset'] + p['bytes'] for p in pieces if p['bytes'])
    joined = bytearray(size)
    for p in pieces:
        if not p['bytes']:
            continue
        file = directory / f"adsp.b{p['index']:02d}"
        if file.is_symlink() or not file.is_file() or file.stat().st_size != p['bytes']:
            raise ValueError(f'Actual signed ADSP segment missing or wrong size: {file.name}')
        joined[p['offset']:p['offset'] + p['bytes']] = file.read_bytes()
    mdt = directory / 'adsp.mdt'
    if mdt.exists():
        hashed = next(p for p in pieces if p['flags'] & TYPE_MASK == TYPE_HASH)
        signature = joined[hashed['offset']:hashed['offset'] + hashed['bytes']]
        if mdt.is_symlink() or mdt.read_bytes() != head + signature:
            raise ValueError('MDT metadata differs from the preserved signed header/hash pieces')
    result = bytes(joined)
    return result, describe(result, region)


def prepare(out, env):
    out = Path(out)
    region = board_contract(out, env.get('DITING_ADSP_LIVE_DTB') or env.get('DITING_LIVE_DTB'))
    source = env.get('DITING_ADSP_FIRMWARE')
    report = dict(board=region, builtin=False, hardware_tested=False)
    if not source:
        (out / 'adsp-firmware.json').write_text(json.dumps(report, indent=2) + '\n')
        return report, None
    path = Path(source).expanduser().resolve()
    if path.is_dir():
        data, description = join_split(path, region)
        receipt = path / 'source.json'
        if receipt.is_file():
            report['source_receipt'] = json.loads(receipt.read_text())
    elif path.is_file() and 0 < path.stat().st_size <= LIMIT:
        data = path.read_bytes()
        description = describe(data, region)
    else:
        raise ValueError('Explicit DITING_ADSP_FIRMWARE input is missing or too large')
    blob = out / 'adsp.mbn'
    blob.write_bytes(data)
    archive = out / 'adsp.pack'
    archive.write_bytes(pack(data))
    assembly = out / 'DitingAdspFirmware.s'
    # Quoting here is assembler quoting, never shell interpolation.
    quoted = str(archive.resolve()).replace('\\', '\\\\').replace('"', '\\"')
    assembly.write_text('.section __TEXT,__const\n.p2align 12\n'
                        '.globl _diting_adsp_firmware_begin\n.globl _diting_adsp_firmware_end\n'
                        '_diting_adsp_firmware_begin:\n.incbin "' + quoted + '"\n'
                        '_diting_adsp_firmware_end:\n')
    report.update(builtin=True, source=str(path), image=description,
                  storage=dict(format='DTADSP01/zlib', raw_bytes=len(data), packed_bytes=archive.stat().st_size,
                               signed_bytes_preserved=True, decoded_memory_released_on_close=True),
                  selection='diting_adsp_builtin=1; only when the selected OS file is absent')
    (out / 'adsp-firmware.json').write_text(json.dumps(report, indent=2) + '\n')
    return report, assembly
