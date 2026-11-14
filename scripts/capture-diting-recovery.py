#!/usr/bin/env python3
"""Capture a complete OEM minidump from root Recovery, without partition writes.

adb shell -T separates remote stderr and preserves the remote exit status.
Binary reads require BOTH a zero exit status and the exact advertised length.
An optional --reader temporarily boots an already supplied Recovery image.
"""
import argparse
import datetime
import json
from pathlib import Path
import re
import shlex
import subprocess
import time


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--serial', required=True)
    p.add_argument('--output-dir', type=Path, required=True)
    p.add_argument('--reader', type=Path)
    p.add_argument('--adb', default='adb')
    p.add_argument('--fastboot', default='fastboot')
    a = p.parse_args()
    out = a.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    receipt = {'serial': a.serial, 'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
               'commands': [], 'partitions_written': False, 'userdata_mounted': False,
               'status': 'RUNNING', 'transport': 'adb shell -T; separate stdout/stderr'}

    def save():
        (out / 'capture.json').write_text(json.dumps(receipt, indent=2) + '\n')

    def command(argv, name=None, expected=None, required=True, timeout=15):
        item = {'argv': argv, 'file': name, 'expected_bytes': expected}
        receipt['commands'].append(item)
        save()
        binary = expected is not None
        path = out / (name + '.partial') if binary else None
        try:
            if binary:
                with path.open('xb') as dest:
                    result = subprocess.run(argv, stdout=dest, stderr=subprocess.PIPE, timeout=timeout)
                data = None
                size = path.stat().st_size
            else:
                result = subprocess.run(argv, capture_output=True, timeout=timeout)
                data, size = result.stdout, len(result.stdout)
                if name:
                    (out / name).write_bytes(data)
            item.update(exit=result.returncode, bytes=size,
                        stderr=result.stderr.decode(errors='replace'))
            item['success'] = result.returncode == 0 and (expected is None or size == expected)
            if binary:
                if item['success']:
                    path.rename(out / name)
                else:
                    item['partial_file'] = path.name
            save()
            if required and not item['success']:
                raise RuntimeError('Capture failed: ' + ' '.join(argv))
            return (data or b'').decode(errors='replace')
        except subprocess.TimeoutExpired as error:
            item.update(exit=None, success=False, timeout=timeout)
            if binary:
                item.update(bytes=path.stat().st_size, partial_file=path.name)
            else:
                if name:
                    (out / (name + '.partial')).write_bytes(error.stdout or b'')
            item['stderr'] = (error.stderr or b'').decode(errors='replace')
            save()
            if required:
                raise RuntimeError('Capture command timed out') from error
            return ''

    adb = [a.adb, '-s', a.serial, 'shell', '-T']
    try:
        if a.reader:
            if not a.reader.is_file():
                raise RuntimeError('Supplied Recovery image is missing')
            fb = [a.fastboot, '-s', a.serial]
            for key, value in [('product', 'diting'), ('unlocked', 'yes'), ('current-slot', 'b')]:
                text = command(fb + ['getvar', key])
                # fastboot writes getvar responses to stderr on this host.
                text += receipt['commands'][-1]['stderr']
                if not re.search(r'(?m)^' + re.escape(key) + r':\s*' + value + r'\s*$', text):
                    raise RuntimeError('Fastboot prerequisite mismatch: ' + key)
            command(fb + ['boot', str(a.reader.resolve())], name='recovery-boot.stdout', timeout=30)
            deadline = time.monotonic() + 45
            while time.monotonic() < deadline:
                state = command([a.adb, '-s', a.serial, 'get-state'], required=False, timeout=4).strip()
                if state in ('device', 'recovery'):
                    break
                time.sleep(1)
            else:
                raise RuntimeError('Root Recovery did not appear')
        identity = command(adb + ['id'], name='id.txt')
        if not re.search(r'\buid=0\(', identity):
            raise RuntimeError('Recovery must be root')
        if command(adb + ['getprop ro.product.device'], name='device.txt').strip() != 'diting':
            raise RuntimeError('Recovery device identity mismatch')
        mounts = command(adb + ['cat /proc/mounts'], name='mounts.txt')
        if re.search(r' (?:/data|/metadata|/cache) ', mounts):
            receipt['userdata_mounted'] = True
            raise RuntimeError('Unexpected guest filesystem mount')
        for name, cmd in [('cmdline.txt', 'cat /proc/cmdline'), ('dmesg.txt', 'dmesg'),
                          ('bootreason.txt', 'getprop ro.boot.bootreason'),
                          ('bootreason-history.txt', 'getprop sys.boot.reason'),
                          ('pstore-list.txt', 'ls -la /sys/fs/pstore')]:
            command(adb + [cmd], name=name, required=False)
        device = '/dev/block/by-name/minidump'
        size = int(command(adb + ['blockdev --getsize64 ' + shlex.quote(device)]).strip())
        if not 0 < size <= 256 * 1024 * 1024 or size % (1024 * 1024):
            raise RuntimeError('Unexpected minidump geometry; no partial-prefix capture')
        command(adb + [f'dd if={shlex.quote(device)} bs=1048576 count={size // 1048576}'],
                name='minidump.bin', expected=size, timeout=60)
        names = command(adb + ['ls /sys/bus/nvmem/devices'], name='nvmem-list.txt', required=False)
        # Qualcomm SDAM's nvmem driver forbids reads before offset 0x40.
        for name in sorted(names.split()):
            if not re.fullmatch(r'spmi_sdam\d+', name):
                continue
            device = '/sys/bus/nvmem/devices/' + name + '/nvmem'
            command(adb + [f'dd if={shlex.quote(device)} bs=1 skip=64 count=64'],
                    name=name + '.offset64.bin', expected=64, required=False)
        mounts = command(adb + ['cat /proc/mounts'], name='mounts-after.txt')
        if re.search(r' (?:/data|/metadata|/cache) ', mounts):
            receipt['userdata_mounted'] = True
            raise RuntimeError('Guest filesystem mounted during capture')
        receipt['status'] = 'PASS'
        receipt['optional_failures'] = sum(not item.get('success', False) for item in receipt['commands'])
        print(out)
    except Exception as error:
        receipt.update(status='FAIL', error=str(error))
        raise
    finally:
        receipt['finished_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        save()


if __name__ == '__main__':
    main()
