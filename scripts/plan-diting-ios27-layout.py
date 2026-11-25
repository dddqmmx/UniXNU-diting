#!/usr/bin/env python3
"""Plan new restore windows after the live windows, preserving the current Data."""
import argparse
import datetime
import json
from pathlib import Path
import sys
import tomllib

ROOT = Path(__file__).resolve().parents[1]


def align(value, boundary):
    return (value + boundary - 1) // boundary * boundary


def plan(current, images):
    if current.get('device') != 'diting' or current.get('partition') != '/dev/block/sda37':
        raise ValueError('Require live Diting userdata metadata')
    if current.get('directory_format_checksum') != 'PASS' or not current.get('metadata_checked_utc'):
        raise ValueError('Require a checked SHNGROOT directory and live APFS metadata')
    capacity = current['size512'] * 512
    if current['partition_bytes'] != capacity or current['logical_block_bytes'] != 4096:
        raise ValueError('Inconsistent userdata geometry')
    windows = current['windows']
    if {w['name'] for w in windows} != {'OS', 'SystemOS', 'Data'}:
        raise ValueError('Expected exactly the live OS/SystemOS/Data windows')
    by_name = {w['name']: w for w in windows}
    for i, window in enumerate(windows):
        start, size = window['offset'], window['bytes']
        if start < 4096 or start % 4096 or size <= 0 or size % 512 or \
                start + align(size, 4096) > current['image_bytes'] or current['image_bytes'] > capacity:
            raise ValueError('Invalid live window bounds')
        for previous in windows[:i]:
            if start < previous['offset'] + align(previous['bytes'], 4096) and \
                    previous['offset'] < start + align(size, 4096):
                raise ValueError('Overlapping live windows')
        volume_bytes = window['apfs']['container_bytes']
        available = window.get('gpt', {}).get('apfs_bytes', size)
        if volume_bytes <= 0 or volume_bytes > available:
            raise ValueError('Live APFS capacity is outside its window/partition')
    data = by_name['Data']
    if len(data['apfs']['volumes']) != 1 or not data.get('gpt'):
        raise ValueError('Require the actual GPT-backed Data volume')
    # Keep the complete old window set intact. Directory replacement is a
    # separate future operation, after a matching boot package exists.
    cursor = align(current['image_bytes'], 16 * 1024 * 1024)
    planned = []
    for name, image in images.items():
        size = image['bytes']
        if size <= 0 or size % 4096:
            raise ValueError(name + ': restore image is not 4 KiB aligned')
        if cursor + size > capacity:
            raise ValueError('New restore images do not fit after the live windows')
        planned.append(dict(name=name, offset=cursor, bytes=size,
                            volume_uuid=image['volume_uuid'], source=image['source'],
                            operation='new window'))
        cursor = align(cursor + size, 16 * 1024 * 1024)
    planned.append({'name': 'Data', 'offset': data['offset'], 'bytes': data['bytes'],
                    'volume_uuid': data['apfs']['volumes'][0]['uuid'],
                    'operation': 'retain existing window and GPT/APFS metadata'})
    return {'profile': 'ios-27.2.1-24B156', 'device': 'diting',
            'source_metadata_utc': current['metadata_checked_utc'],
            'partition_bytes': capacity, 'image_bytes': cursor,
            'remaining_bytes': capacity - cursor, 'windows': planned,
            'preserved_live_windows': windows,
            'data_container_bytes': data['apfs']['container_bytes'],
            'device_written': False, 'installation_ready': False,
            'before_install': ['Complete strict 24B156 kernel collection and matching loader/payload',
                               'Qualify new userland and its use of the existing Data',
                               'Check any board storage reservations beyond the live windows',
                               'Re-read live geometry/SHNGROOT/APFS metadata from unmounted Recovery',
                               'Retain the current directory for rollback before any directory change']}


def image_info(path, expected_uuid, expected_bytes):
    sys.path.insert(0, str(ROOT / 'tools/apfs'))
    import apfs
    if path.stat().st_size != expected_bytes:
        raise ValueError('Restore image size differs: ' + str(path))
    container = apfs.Container(str(path))
    try:
        volumes = [apfs._uuid(v.vol_uuid).upper() for v in container.volumes()]
        if expected_uuid.upper() not in volumes:
            raise ValueError('Restore APFS UUID differs: ' + str(path))
        if container.nx['block_count'] * container.block_size > expected_bytes:
            raise ValueError('Restore APFS capacity exceeds the image')
    finally:
        container.close()
    return {'source': str(path.resolve()), 'bytes': expected_bytes,
            'volume_uuid': expected_uuid.upper()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--current-layout', type=Path, required=True)
    parser.add_argument('--os-image', type=Path, required=True)
    parser.add_argument('--systemos-image', type=Path, required=True)
    parser.add_argument('--appos-image', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    target = tomllib.loads((ROOT / 'migration/ios27/target.toml').read_text())['images']
    images = {'OS': image_info(args.os_image, target['os_volume_uuid'], target['os_bytes']),
              'SystemOS': image_info(args.systemos_image, target['systemos_volume_uuid'],
                                     target['systemos_bytes'])}
    if args.appos_image:
        images['AppOS'] = image_info(args.appos_image, target['appos_volume_uuid'],
                                    target['appos_bytes'])
    result = plan(json.loads(args.current_layout.read_text()), images)
    result['created_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({key: value for key, value in result.items()
                      if key != 'preserved_live_windows'}, indent=2))


if __name__ == '__main__':
    main()
