#!/usr/bin/env python3
"""Stream real GPU events through one CDC session, without a Recovery reboot."""
import argparse
import base64
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import struct
import time
from diting_usb_ports import serial_ports
from diting_usb_session import DitingUsbSession

TRACE = struct.Struct('<4I2Q')
CHUNK = struct.Struct('<4I6Q')
RECORD = struct.Struct('<2Q2I12Q')


def decode_chunk(response, after, requested):
    hit = re.search(rb'\nGPU trace bytes=(\d+)\nDATA ([A-Za-z0-9+/=]+)\n', response)
    if not hit or len(hit[2]) > 3288:
        raise ValueError('complete bounded GPU trace data missing')
    raw = base64.b64decode(hit[2], validate=True)
    if len(raw) != int(hit[1]) or len(raw) < CHUNK.size:
        raise ValueError('GPU trace size mismatch')
    magic, version, size, count, issued, dropped, first, next_cursor, skipped, missing = CHUNK.unpack_from(raw)
    if (magic, version, size) != (0x31545047, 1, RECORD.size) or count > requested:
        raise ValueError('GPU trace ABI mismatch')
    if len(raw) != CHUNK.size + count * size or not after <= next_cursor <= issued:
        raise ValueError('GPU trace cursor or record extent mismatch')
    if first != (max(1, issued - 16384 + 1)) or skipped + missing + count != next_cursor - after:
        raise ValueError('GPU trace does not account for every sequence position')
    records = raw[CHUNK.size:]
    previous = max(after, first - 1)
    for off in range(0, len(records), size):
        sequence, _, event, *_ = RECORD.unpack_from(records, off)
        if not previous < sequence <= next_cursor or not event:
            raise ValueError('GPU records are not ordered, valid events')
        previous = sequence
    return dict(issued=issued, dropped=dropped, first=first, next=next_cursor,
                skipped=skipped, missing=missing, count=count), records


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port')
    p.add_argument('--seconds', type=float, default=20)
    p.add_argument('--max-records', type=int, default=65536)
    p.add_argument('--max-raw-bytes', type=int, default=16 * 1048576)
    p.add_argument('--output-dir', type=Path, required=True)
    a = p.parse_args()
    if not 0 < a.seconds <= 60 or not 1 <= a.max_records <= 131000 or not 1 <= a.max_raw_bytes <= 16 * 1048576:
        p.error('duration, record or raw log limit outside bounds')
    ports = [a.port] if a.port else serial_ports()
    if len(ports) != 1:
        p.error('expected one Diting CDC port; use --port')
    a.output_dir.mkdir(parents=True, exist_ok=False)
    report = dict(status='RUNNING', started_utc=datetime.now(timezone.utc).isoformat(),
                  port=ports[0], transport='single CDC session, bounded cursor ranges',
                  target_fps=120, physical_panel_timestamps_available=False,
                  hardware_performance_pass=False, records=0, skipped_positions=0,
                  missing_positions=0, commands=0, raw_bytes=0)
    cursor = dropped = 0
    start = time.monotonic()
    try:
        with (a.output_dir/'trace.bin').open('xb') as trace, (a.output_dir/'console.raw.log').open('xb') as raw:
            trace.write(TRACE.pack(0x31545047, 1, RECORD.size, 0, 0, 0))
            try:
                with DitingUsbSession(ports[0]) as session:
                    deadline = time.monotonic() + a.seconds
                    while time.monotonic() < deadline and report['records'] < a.max_records:
                        requested = min(20, a.max_records - report['records'])
                        response = session.command(f'trace {cursor} {requested}')
                        room = a.max_raw_bytes - report['raw_bytes']
                        captured = session.received[:room]
                        raw.write(captured)
                        report['raw_bytes'] += len(captured)
                        if len(captured) != len(session.received):
                            report['raw_log_truncated'] = True
                        chunk, records = decode_chunk(response, cursor, requested)
                        cursor, dropped = chunk['next'], chunk['dropped']
                        trace.write(records)
                        report['records'] += chunk['count']
                        report['skipped_positions'] += chunk['skipped']
                        report['missing_positions'] += chunk['missing']
                        report['kernel_issued_last'] = chunk['issued']
                        report['commands'] += 1
                        if chunk['next'] == chunk['issued']:
                            time.sleep(.01)
            finally:
                # issued is the last inspected position. The separate receipt
                # preserves the kernel's later producer watermark and losses.
                trace.seek(0)
                trace.write(TRACE.pack(0x31545047, 1, RECORD.size, report['records'], cursor, dropped))
        report['status'] = 'PASS'
    except Exception as error:
        report['status'] = 'FAIL'
        report['error'] = f'{type(error).__name__}: {error}'
        raise
    finally:
        report['finished_utc'] = datetime.now(timezone.utc).isoformat()
        report['elapsed_seconds'] = time.monotonic() - start
        report['last_inspected_sequence'] = cursor
        report['kernel_dropped_last'] = dropped
        (a.output_dir/'capture.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report, ensure_ascii=False))


if __name__ == '__main__':
    main()
