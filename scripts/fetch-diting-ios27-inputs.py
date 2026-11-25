#!/usr/bin/env python3
"""Fetch the exact 24B156 customer restore members into a private directory."""
import argparse
import json
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import time
import tomllib
from types import SimpleNamespace

from diting_ios27 import inspect_kernel_collection, restore_identity

ROOT = Path(__file__).resolve().parents[1]
PROFILE = 'ios-27.2.1-24B156'


def extract(source, component, out, jobs):
    member, size, compressed = source.info(component)
    path = out / Path(member).name
    if path.exists() and path.stat().st_size == size:
        print(f'{component}: preserving completed local member', flush=True)
        return path
    partial = path.with_suffix(path.suffix + '.partial')
    print(f'{component}: extracting {size} bytes from {member}', flush=True)
    with source.zip.open(member) as stream, partial.open('wb') as destination:
        from unixnu.ipsw import RangeFile
        import contextlib
        window = (source.stream.prefetch(source.stream.tell(), compressed, jobs)
                  if isinstance(source.stream, RangeFile) else contextlib.nullcontext())
        done, reported = 0, time.monotonic()
        with window:
            while chunk := stream.read(8 << 20):
                destination.write(chunk)
                done += len(chunk)
                if time.monotonic() - reported >= 10:
                    print(f'{component}: {done}/{size} bytes', flush=True)
                    reported = time.monotonic()
    if partial.stat().st_size != size:
        raise RuntimeError(f'Incomplete restore member: {member}')
    partial.replace(path)
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--unixnu', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True, help='private member cache outside the source repository')
    parser.add_argument('--ipsw', help='optional local copy of the same Apple restore')
    parser.add_argument('--components', nargs='+', help='specific BuildIdentity components')
    parser.add_argument('--userland', action='store_true', help='include OS, SystemOS and AppOS images')
    parser.add_argument('--lzfse', type=Path, help='decoder; also saves the checked decompressed kernelcache')
    parser.add_argument('--decrypt-with', type=Path, help='ipsw tool with native AEA decryption; keys stay inside the tool')
    parser.add_argument('--jobs', type=int, default=8, help='bounded parallel HTTP ranges')
    args = parser.parse_args()
    out = args.out.resolve()
    if out == ROOT or ROOT in out.parents:
        parser.error('--out must be outside the source repository')
    if not 1 <= args.jobs <= 64:
        parser.error('--jobs must be in 1..64')
    out.mkdir(parents=True, exist_ok=True)
    profile = tomllib.loads((ROOT / 'os-profiles' / PROFILE / 'profile.toml').read_text())
    target = tomllib.loads((ROOT / 'configs/diting-ios27-inputs.toml').read_text())['target']
    sys.path.insert(0, str(args.unixnu.resolve() / 'scripts'))
    from unixnu.ipsw import Ipsw, im4p_payload
    ctx = SimpleNamespace(board={'ipsw': profile['ipsw']}, out_root=out,
                          option_text=lambda *unused: args.ipsw)
    source = Ipsw(ctx)
    source.identity = restore_identity(source.manifest, profile['ipsw'])
    selected = list(args.components or profile['ipsw']['fetch'])
    if args.userland:
        selected += ['OS', 'Cryptex1,SystemOS', 'Cryptex1,AppOS']
    selected = list(dict.fromkeys(selected))
    missing = [name for name in selected if name not in source.identity['Manifest']]
    if missing:
        parser.error('Components missing from the customer identity: ' + ', '.join(missing))
    report = {'profile': PROFILE, 'version': target['version'], 'build': target['build'],
              'variant': source.identity['Info']['Variant'], 'device_class': target['device_class'],
              'chip_id': target['chip_id'], 'board_id': target['board_id'],
              'image_verification': 'skipped by user request', 'device_access': False, 'components': {}}
    record = out / 'restore-inputs.json'
    (out / 'BuildManifest.plist').write_bytes(plistlib.dumps(source.manifest))
    for component in selected:
        path = extract(source, component, out, args.jobs)
        report['components'][component] = {'member': source.member(component),
                                           'bytes': path.stat().st_size, 'path': str(path)}
        record.write_text(json.dumps(report, indent=2) + '\n')
        if component == 'KernelCache' and args.lzfse:
            packed = out / 'kernelcache.lzfse'
            decoded = out / 'kernelcache.decompressed'
            packed.write_bytes(im4p_payload(path.read_bytes()))
            subprocess.run([str(args.lzfse), '-decode', '-i', str(packed), '-o', str(decoded)], check=True)
            info = inspect_kernel_collection(decoded.read_bytes(), target)
            report['kernel'] = {key: value for key, value in info.items() if key != 'entries'}
            (out / 'kernel-inventory.json').write_text(json.dumps(info, indent=2) + '\n')
        if path.suffix == '.aea' and args.decrypt_with:
            destination = out / 'decrypted'
            destination.mkdir(exist_ok=True)
            print(f'{component}: decrypting AEA locally', flush=True)
            subprocess.run([str(args.decrypt_with), 'fw', 'aea', str(path), '--output', str(destination)], check=True)
            decoded = destination / path.with_suffix('').name
            if not decoded.is_file() or decoded.stat().st_size < 4096:
                raise RuntimeError('The AEA tool did not leave a complete disk image')
            report['components'][component]['decrypted'] = str(decoded)
        record.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
