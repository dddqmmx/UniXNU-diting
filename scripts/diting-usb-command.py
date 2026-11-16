#!/usr/bin/env python3
"""Run one acknowledged command on the actual Diting CDC interface."""
import argparse
from pathlib import Path
import re
from diting_usb_ports import serial_ports
from diting_usb_session import DitingUsbSession


def gpu_command(arguments):
    """Keep the host's accepted grammar identical to the bounded broker ABI."""
    if arguments == ['status']:
        return 'gputest status'
    if (len(arguments) == 3 and arguments[0] == 'run' and
            arguments[1] in ('qualification','performance','all','ui') and
            arguments[2] in ('auto','sysmem','gmem')):
        if arguments[1] == 'ui' and arguments[2] != 'auto':
            raise ValueError('UI policy belongs to the running render server; use auto')
        return 'gputest ' + ' '.join(arguments)
    if arguments and arguments[0] in ('cancel','result'):
        count = 2 if arguments[0] == 'cancel' else 4
        if len(arguments) == count and all(re.fullmatch(r'[0-9]{1,10}', word) for word in arguments[1:]):
            values = list(map(int, arguments[1:]))
            if 0 < values[0] <= 0xffffffff and (count == 2 or 0 <= values[1] <= 131072 and 0 < values[2] <= 1024):
                return 'gputest ' + arguments[0] + ' ' + ' '.join(map(str,values))
    raise ValueError('gputest expects status, run PROFILE POLICY, cancel JOB, or result JOB OFFSET BYTES(1..1024)')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['rec', 'fastboot', 'help', 'status', 'gpu', 'display', 'read', 'trace', 'gputest'])
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
    elif args.command == 'gputest':
        try:
            command = gpu_command(args.arguments)
        except ValueError as error:
            parser.error(str(error))
    elif args.command == 'gpu':
        if args.arguments not in ([], ['bandwidth']):
            parser.error('gpu accepts bandwidth for one bounded BWMON observation')
        command = 'gpu' + (' bandwidth' if args.arguments else '')
    elif args.command == 'display':
        if not args.arguments:
            command = 'display'
        elif (len(args.arguments)==2 and args.arguments[0]=='fetch-swap' and
              re.fullmatch(r'[0-9]{1,5}',args.arguments[1]) and 1000<=int(args.arguments[1])<=60000):
            command='display '+' '.join(args.arguments)
        elif (len(args.arguments)==2 and args.arguments[0]=='solid' and args.arguments[1] in ('status','cancel')):
            command='display '+' '.join(args.arguments)
        elif (len(args.arguments)==3 and args.arguments[0]=='solid' and args.arguments[1] in ('blue','green','red')
              and re.fullmatch(r'[0-9]{1,5}',args.arguments[2]) and 1000<=int(args.arguments[2])<=60000):
            command='display '+' '.join(args.arguments)
        elif args.arguments in (['color'], ['panel'], ['pixels']):
            command = 'display ' + args.arguments[0]
        elif (len(args.arguments) == 3 and args.arguments[0] == 'pixels' and
              all(re.fullmatch(r'[0-9]{1,4}', value) for value in args.arguments[1:])):
            first, count = map(int, args.arguments[1:])
            if not 0 <= first < 4096 or not 1 <= count <= 64 or first + count > 4096:
                parser.error('display pixels expects OFFSET 0..4095 and COUNT 1..64 without crossing the grid')
            command = f'display pixels {first} {count}'
        else:
            parser.error('display accepts color, panel, pixels [OFFSET COUNT], fetch-swap MS, or solid status/cancel/blue/green/red [MS 1000..60000]')
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
            if args.command == 'gputest':
                from diting_gpu_automation import body, fields, status
                if args.arguments[0] == 'result':
                    header = fields(body(response, command).decode('ascii').splitlines()[0])
                    if header.get('error') != '0':
                        raise RuntimeError('device rejected GPU result read: '+header.get('error','missing status'))
                elif status(response, command)['error']:
                    raise RuntimeError('device rejected GPU test command')
            return 0
        finally:
            if args.output:
                args.output.parent.mkdir(parents=True, exist_ok=True)
                args.output.write_bytes(session.received)


if __name__ == '__main__':
    raise SystemExit(main())
