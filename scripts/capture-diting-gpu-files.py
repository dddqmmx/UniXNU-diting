#!/usr/bin/env python3
"""Read GPU logs and installation metadata from unmounted root Recovery.

Reads the live APFS checkpoints and requested file extents. No mount, partition
write, image hash, or image comparison. Capture success is not a GPU PASS.
"""
import argparse
import datetime
import importlib.util
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/apfs'))
import apfs
from dmg import read_gpt, APFS_PART_TYPE

BLOCK = 4096
TARGET = '/dev/block/by-name/userdata'
BUNDLE = '/System/Library/Extensions/DitingAdrenoMetal.bundle'
OS_FILES = {
    BUNDLE + '/.diting-native-gpu-runtime-v1': 16,
    BUNDLE + '/Info.plist': 65536,
    '/System/Library/Extensions/DitingAdrenoMetal.clients': 4096,
    '/System/Library/Extensions/DitingAdrenoMetal.publish': 4096,
    '/System/Library/LaunchDaemons/dev.unixnu.diting.gpu-capture18.plist': 65536,
}
OS_METADATA = [BUNDLE + '/' + name for name in
               ('DitingAdrenoMetal', 'DitingMetalBridge.dylib', 'libvulkan_freedreno.dylib')]
DATA_FILES = {'/logs/diting/gpu-capture18.log': 1 << 20}
DATA_FILES.update({f'/logs/diting/gpu-{seconds}.trace.bin': 16 << 20 for seconds in (60, 180, 300)})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--volume-layout', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--adb', default='adb')
    parser.add_argument('--usb-only', action='store_true', help='read only the bounded kernel USB boot snapshot from Data')
    args = parser.parse_args()
    data_files = {'/logs/diting/usb-boot.log': 262144} if args.usb_only else DATA_FILES
    layout = json.loads(args.volume_layout.read_text())
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    receipt = dict(serial=args.serial, started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                   status='RUNNING', partitions_written=False, guest_volumes_mounted=False,
                   image_verification='skipped by user request', files=[], block_reads=0,
                   transport='adb shell -T; exact-length stdout, separate stderr')
    commands = (out / 'commands.jsonl').open('x')

    def save():
        (out / 'capture.json').write_text(json.dumps(receipt, indent=2) + '\n')

    def run(command, expected=None):
        argv = [args.adb, '-s', args.serial, 'shell', '-T', command]
        result = subprocess.run(argv, capture_output=True, timeout=20)
        entry = dict(argv=argv, exit=result.returncode, bytes=len(result.stdout),
                     expected_bytes=expected, stderr=result.stderr.decode(errors='replace'))
        commands.write(json.dumps(entry) + '\n')
        commands.flush()
        if result.returncode or (expected is not None and len(result.stdout) != expected):
            raise IOError('Recovery command failed or returned a short read: ' + command)
        return result.stdout

    def mounts(name):
        data = run('cat /proc/mounts')
        (out / name).write_bytes(data)
        if re.search(rb' (?:/data|/metadata|/cache) ', data):
            receipt['guest_volumes_mounted'] = True
            raise RuntimeError('Guest volumes must remain unmounted')

    def blocks(absolute, count):
        if absolute % BLOCK or not 0 < count <= 128:
            raise ValueError('Invalid block read')
        data = run(f'dd if={TARGET} bs={BLOCK} skip={absolute // BLOCK} count={count}', count * BLOCK)
        receipt['block_reads'] += count
        return data

    class Window:
        def __init__(self, name, base, size):
            self.path, self.base, self.size = name, base, size
            self.cache = {}

        def read(self, offset, length):
            if offset < 0 or length < 0 or offset + length > self.size:
                raise IOError('APFS read outside its live window')
            if not length:
                return b''
            start = offset // BLOCK * BLOCK
            stop = (offset + length + BLOCK - 1) // BLOCK * BLOCK
            if stop > self.size:
                raise IOError('APFS block read outside its live window')
            data = bytearray()
            cursor = start
            while cursor < stop:
                if cursor not in self.cache:
                    count = min(128, (stop - cursor) // BLOCK)
                    raw = blocks(self.base + cursor, count)
                    if len(self.cache) + count > 4096:
                        self.cache.clear()
                    for index in range(count):
                        self.cache[cursor + index * BLOCK] = raw[index * BLOCK:(index + 1) * BLOCK]
                data.extend(self.cache[cursor])
                cursor += BLOCK
            return bytes(data[offset - start:offset - start + length])

        def close(self):
            self.cache.clear()

    try:
        if not re.search(rb'\buid=0\(', run('id')) or run('getprop ro.product.device').strip() != b'diting':
            raise RuntimeError('Root Diting Recovery required')
        mounts('mounts-before.txt')
        capacity = int(run(f'blockdev --getsize64 {TARGET}').strip())
        if capacity != 0x37E69FB000:
            raise RuntimeError('Unexpected userdata geometry')
        raw = blocks(0, 1)
        if raw[:8] != b'SHNGROOT':
            raise RuntimeError('Live userdata has no SHNGROOT window directory')
        version, count, total = struct.unpack_from('<IIQ', raw, 8)
        if version != 1 or count != 3 or not 0 < total <= capacity:
            raise RuntimeError('Unexpected SHNGROOT directory geometry')
        windows = {}
        for i in range(count):
            name, offset, size = struct.unpack_from('<32sQQ', raw, 24 + 128 * i)
            name = name.split(b'\0', 1)[0].decode('ascii')
            if name in windows or offset < BLOCK or offset % BLOCK or size % BLOCK or not size or offset + size > total:
                raise RuntimeError('Invalid live window bounds')
            windows[name] = (offset, size)
        if set(windows) != {'OS', 'SystemOS', 'Data'}:
            raise RuntimeError('Unexpected live window names')
        ranges = sorted((base, base + size) for base, size in windows.values())
        if any(left[1] > right[0] for left, right in zip(ranges, ranges[1:])):
            raise RuntimeError('Overlapping live windows')
        receipt['windows'] = windows
        requested = [('Data', data_files, [])] if args.usb_only else [('OS', OS_FILES, OS_METADATA), ('Data', data_files, [])]
        for name, files, metadata in requested:
            window = Window(name, *windows[name])
            source = window
            if window.read(0, 1024)[512:520] == b'EFI PART':
                parts = [part for part in read_gpt(window) if part.type_guid == APFS_PART_TYPE]
                if len(parts) != 1:
                    raise RuntimeError('Expected one APFS partition in ' + name)
                source = apfs.PartitionImage(window, parts[0])
            original = apfs.open_image
            apfs.open_image = lambda path: source
            container = None
            try:
                container = apfs.Container(name)
                wanted = layout[name.lower()]['uuid'].upper()
                volumes = [v for v in container.volumes() if apfs._uuid(v.vol_uuid).upper() == wanted]
                if len(volumes) != 1:
                    raise RuntimeError('Installed volume identity mismatch: ' + name)
                volume = volumes[0]
                receipt.setdefault('volume_checkpoints', {})[name] = dict(uuid=wanted, xid=container.xid)
                for path in [*files, *metadata]:
                    entry = dict(window=name, path=path)
                    receipt['files'].append(entry)
                    try:
                        hit = volume.lookup(path)
                        if hit is None:
                            entry['status'] = 'MISSING'
                            continue
                        inode = volume.inode(hit[0])
                        if inode is None:
                            raise RuntimeError('Requested inode is unavailable')
                        entry.update(bytes=inode.size, mode=oct(inode.mode), uid=inode.owner, gid=inode.group,
                                     inode=inode.obj_id, parent=inode.parent_id,
                                     type='directory' if inode.is_dir else 'regular' if inode.is_reg else 'other')
                        if path in files:
                            if not inode.is_reg:
                                raise RuntimeError('Requested contents are not a regular file')
                            if not 0 <= inode.size <= files[path]:
                                raise RuntimeError('Requested log or marker exceeds its size bound')
                            body = inode.contents()
                            if len(body) != inode.size:
                                raise IOError('APFS file returned a short read')
                            dest = out / (name.lower() + '-' + Path(path).name)
                            dest.write_bytes(body)
                            entry['output'] = dest.name
                        entry['status'] = 'READ'
                    except Exception as error:
                        entry.update(status='UNREADABLE', error=str(error))
            finally:
                apfs.open_image = original
                if container is not None:
                    container.close()
                window.close()
        mounts('mounts-after.txt')
        receipt['status'] = 'PASS'
        receipt['missing_files'] = sum(e['status'] == 'MISSING' for e in receipt['files'])
        receipt['unreadable_files'] = sum(e['status'] == 'UNREADABLE' for e in receipt['files'])
        print(json.dumps(receipt, indent=2))
    except Exception as error:
        receipt.update(status='FAIL', error=str(error))
        raise
    finally:
        receipt['finished_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        save()
        commands.close()


if __name__ == '__main__':
    main()
