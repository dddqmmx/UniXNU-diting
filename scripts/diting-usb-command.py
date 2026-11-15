#!/usr/bin/env python3
"""Run one acknowledged command on the actual Diting CDC interface."""
import argparse
from pathlib import Path
import re
from diting_usb_ports import serial_ports
from diting_usb_session import DitingUsbSession


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['rec', 'fastboot', 'help', 'status', 'gpu', 'read', 'trace'])
    parser.add_argument('arguments', nargs='*', help='read: HEX-KVA BYTES; trace: decimal AFTER COUNT(1..20)')
    parser.add_argument('--port')
    parser.add_argument('--timeout', type=float, default=8)
    parser.add_argument('--output', type=Path, help='save received logs and the complete command response')
    args = parser.parse_args()
    if not 0 < args.timeout <= 60:
        parser.error('timeout must be 0..60 seconds')
    if args.command == 'read':
        if len(args.arguments) != 2 or not re.fullmatch(r'(?:0[xX])?[0-9a-fA-F]{1,16}', args.arguments[0]):
            parser.error('read expects a hexadecimal address and decimal byte count')
        try:
            address, count = int(args.arguments[0], 16), int(args.arguments[1], 10)
        except ValueError:
            parser.error('read expects a hexadecimal address and decimal byte count')
        if not 1 <= count <= 512 or address + count > 0xffffffffffffffff:
            parser.error('read size must be 1..512 with no address overflow')
        command = f'read {address:x} {count}'
    elif args.command == 'trace':
        if len(args.arguments) != 2 or any(not re.fullmatch(r'[0-9]{1,20}', x) for x in args.arguments):
            parser.error('trace expects a decimal sequence cursor and record count')
        after, count = map(int, args.arguments)
        if not 0 <= after < 0xffffffffffffffff or not 1 <= count <= 20:
            parser.error('trace cursor must be uint64, count must be 1..20')
        command = f'trace {after} {count}'
    else:
        if args.arguments:
            parser.error('this command takes no extra arguments')
        command = 'reboot ' + args.command if args.command in ('rec', 'fastboot') else args.command
    ports = [args.port] if args.port else serial_ports()
    if len(ports) != 1:
        parser.error('expected one Diting CDC port; use --port')
    with DitingUsbSession(ports[0]) as session:
        try:
            response = session.command(command, args.timeout)
            print(response.decode('utf-8', errors='replace'), end='')
            if args.command in ('rec', 'fastboot') and b'\naccepted; syncing filesystems\n' not in response:
                raise RuntimeError('device rejected reboot')
            if args.command == 'read':
                copied = re.search(rb'RAM address=[0-9a-f]+ requested=(\d+) copied=(\d+)', response)
                if not copied or int(copied[2]) != count:
                    raise RuntimeError('requested RAM range was unavailable or only partly mapped')
            if args.command == 'trace' and b'\nDATA ' not in response:
                raise RuntimeError('device could not provide the requested trace range')
            return 0
        finally:
            if args.output:
                args.output.parent.mkdir(parents=True, exist_ok=True)
                args.output.write_bytes(session.received)


if __name__ == '__main__':
    raise SystemExit(main())
