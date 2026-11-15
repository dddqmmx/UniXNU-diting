#!/usr/bin/env python3
"""Embed one complete signed UI library for a bounded guest OS file update."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import uuid
import zlib


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    library = args.library.resolve()
    raw = library.read_bytes()
    if not 32 <= len(raw) <= 8 << 20 or struct.unpack_from('<II', raw) != (0xfeedfacf, 0x0100000c) or struct.unpack_from('<I', raw, 12)[0] != 6:
        parser.error('a complete arm64 Mach-O UI dylib is required')
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if (out / 'build-report.json').exists():
        parser.error('completed stage retained; choose a new output directory')
    identity = out / 'install-id'
    if not identity.exists():
        identity.write_text(str(uuid.uuid4()) + '\n')
    install_id = uuid.UUID(identity.read_text().strip())
    packed = zlib.compress(raw, 9)
    padded = packed + bytes((-len(packed)) & 7)
    total = 64 + len(padded)
    if total > 4 << 20:
        parser.error('UI stage exceeds the packed size bound')
    payload = struct.pack('<8sIIIIQ16sQQ', b'DTUI0001', 1, 0x8475, 1, 0, total, install_id.bytes, len(raw), len(packed)) + padded
    package = out / 'ui-runtime.pack'
    package.write_bytes(payload)
    assert zlib.decompress(payload[64:64 + len(packed)]) == raw
    assembly = out / 'DitingUiRuntimeData.S'
    assembly.write_text('.section __DATA_CONST,__diting_ui\n.p2align 3\n'
        '.globl _DitingUiRuntimeData\n_DitingUiRuntimeData:\n.incbin ' + json.dumps(str(package)) + '\n'
        '.globl _DitingUiRuntimeEnd\n_DitingUiRuntimeEnd:\n')
    obj = out / 'DitingUiRuntimeData.o'
    subprocess.run(['clang', '-target', 'arm64-apple-ios18.0', '-c', str(assembly), '-o', str(obj)], check=True)
    archive = out / 'libDitingUiRuntime.a'
    subprocess.run(['llvm-ar', '--format=darwin', 'rcs', str(archive), str(obj)], check=True)
    report = dict(runtime_format='diting-ui-boot-stage-v1', guest='22H374', board='SM8475-Diting',
        install_id=str(install_id), archive=str(archive), library=str(library),
        packed_bytes=len(payload), raw_bytes=len(raw), build_complete=True,
        destination='/diting/libditing-ui18.dylib', gpu_files_changed=False,
        installation_confirmed=False, image_verification='skipped by user request')
    (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(archive)


if __name__ == '__main__':
    main()
