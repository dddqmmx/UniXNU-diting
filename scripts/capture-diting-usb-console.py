#!/usr/bin/env python3
"""Save bounded, read-only Diting CDC console output without rebooting the phone."""
import argparse
from pathlib import Path
from diting_usb_ports import serial_ports
from diting_usb_session import DitingUsbSession

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--port', help='Diting CDC serial path; automatically selected when unambiguous')
p.add_argument('--seconds', type=float, default=15)
p.add_argument('--max-bytes', type=int, default=1048576)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
if not 0 < a.seconds <= 60 or not 1 <= a.max_bytes <= 16 * 1048576:
    p.error('use 0..60 seconds and 1..16777216 bytes')
ports = [a.port] if a.port else serial_ports()
if len(ports) != 1:
    p.error('expected one Diting CDC port; use --port for an explicit device')
if a.output.exists():
    p.error('preserving existing output; choose a new path')
a.output.parent.mkdir(parents=True, exist_ok=True)
with DitingUsbSession(ports[0]) as session:
    data = session.capture(a.seconds, a.max_bytes)
    with a.output.open('xb') as output:
        output.write(data)
print(f'{len(data)} live USB console bytes saved to {a.output}')
