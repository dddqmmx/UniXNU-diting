"""Identity checks for the Diting iOS 27 restore and native build inputs."""
import re
import struct


def require_complete_prelink(report):
    """Reject diagnostic or --allow-missing candidates before recording a strict kernel collection."""
    missing = sum(sum(targets.values()) for targets in report.get('missing', {}).values())
    stubbed = sum(sum(targets.values()) for targets in report.get('stubbed', {}).values())
    if (report.get('format') != 'MH_FILESET' or report.get('mapping_complete') is not True
            or report.get('allow_missing') is not False or report.get('candidate_written') is not True
            or missing or stubbed):
        raise ValueError(f'Incomplete iOS 27 prelink: {missing} unresolved references, '
                         f'{stubbed} stubbed references; diagnostic candidate is not loadable')


def restore_identity(manifest, target):
    """Distinguish the customer OS from the separate recovery Erase identity."""
    if (manifest.get('ProductVersion'), manifest.get('ProductBuildVersion')) != (target['version'], target['build']):
        raise ValueError('Restore version/build differs from the selected iOS profile')
    if target['product'] not in manifest.get('SupportedProductTypes', []):
        raise ValueError('Restore product differs from the selected iOS profile')
    matches = []
    for identity in manifest.get('BuildIdentities', []):
        info = identity.get('Info', {})
        if (info.get('DeviceClass') == target['device_class'] and info.get('RestoreBehavior') == 'Erase'
                and info.get('Variant') == target['variant']
                and int(identity.get('ApChipID', '-1'), 16) == target['chip_id']
                and int(identity.get('ApBoardID', '-1'), 16) == target['board_id']):
            matches.append(identity)
    if len(matches) != 1:
        raise ValueError(f'Expected one {target["variant"]} identity, found {len(matches)}')
    return matches[0]


def inspect_kernel_collection(data, target):
    if len(data) < 32:
        raise ValueError('Truncated kernel collection')
    magic, cpu, subtype, kind, commands, size = struct.unpack_from('<6I', data)
    if (magic, cpu, subtype & 0xff, kind) != (0xfeedfacf, 0x100000c, 2, 12):
        raise ValueError('Expected an arm64e MH_FILESET kernel collection')
    if size > len(data) - 32 or commands > size // 8:
        raise ValueError('Invalid kernel collection load commands')
    pattern = rb'Darwin Kernel Version ([0-9.]+):[^\0\n]*root:xnu-([0-9.]+)[^\0\n]*/RELEASE_ARM64_T([0-9A-Fa-f]+)'
    versions = set(re.findall(pattern, data))
    if len(versions) != 1:
        raise ValueError('Kernel collection has no unique release version')
    darwin, source, chip = (value.decode() for value in versions.pop())
    if (darwin, source, int(chip, 16)) != (target['kernel_darwin'], target['kernel_xnu'], target['chip_id']):
        raise ValueError(f'Kernel release {darwin}/xnu-{source}/T{chip} differs from the selected restore')
    at, entries = 32, {}
    for _ in range(commands):
        if at + 8 > 32 + size:
            raise ValueError('Truncated load command')
        command, length = struct.unpack_from('<II', data, at)
        if length < 8 or length & 7 or at + length > 32 + size:
            raise ValueError('Invalid load command size')
        if command == 0x80000035:
            if length < 32:
                raise ValueError('Truncated fileset entry')
            va, fileoff, name_at = struct.unpack_from('<QQI', data, at + 8)
            if name_at < 32 or name_at >= length or fileoff > len(data) - 32:
                raise ValueError('Invalid fileset entry range')
            end = data.find(b'\0', at + name_at, at + length)
            if end < 0:
                raise ValueError('Unterminated fileset entry name')
            name = data[at + name_at:end].decode('ascii')
            if name in entries:
                raise ValueError('Duplicate fileset entry')
            entries[name] = {'address': hex(va), 'offset': fileoff}
        at += length
    if at != 32 + size or 'com.apple.kernel' not in entries:
        raise ValueError('Incomplete fileset kernel index')
    return {'darwin': darwin, 'xnu': source, 'chip_id': hex(int(chip, 16)),
            'arch': 'arm64e', 'format': 'MH_FILESET', 'bytes': len(data), 'entries': entries}
