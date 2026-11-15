#!/usr/bin/env python3
"""Stage a completed GPU runtime into an offline mounted 22H374 OS.

For installed systems, first refresh the unmounted guest base, then use the
existing begin/stage/finish changed-band workflow. This writes files only
under --os-root and never mounts, writes or boots a phone.
"""
import argparse
import json
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'userland'))
import gpu_runtime18


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--os-root', type=Path, required=True)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--json-out', type=Path)
    args = parser.parse_args()
    if os.geteuid():
        parser.error('OS files require root ownership; use the offline image write environment')
    result = gpu_runtime18.stage(args.os_root, args.runtime)
    text = json.dumps(result, indent=2) + '\n'
    if args.json_out:
        args.json_out.write_text(text)
    print(text, end='')


if __name__ == '__main__':
    main()
