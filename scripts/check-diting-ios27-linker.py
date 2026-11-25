#!/usr/bin/env python3
"""Check static arm64e weak NULL pointers and strong provider replacement."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys


SOURCE = '''
extern const char image_header[] __asm__("__mh_execute_header");
const void *volatile kernel_header = image_header;
extern void optional_callback(void) __attribute__((weak_import));
void present_callback(void) { __asm__ volatile("" ::: "memory"); }
void (*volatile callbacks[3])(void) = {present_callback, optional_callback, present_callback};
void kernel_entry(void) {
    void (*callback)(void) = callbacks[1];
    if (callback) callback();
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--clang', type=Path, required=True)
    parser.add_argument('--linker', type=Path, required=True)
    parser.add_argument('--prelink-tools', type=Path, required=True, help='selected XnuPrelink checkout')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    builder_path = Path(__file__).with_name('build-diting-ios27-drivers.py')
    builder_spec = importlib.util.spec_from_file_location('diting_ios27_driver_linker_check', builder_path)
    builder = importlib.util.module_from_spec(builder_spec)
    builder_spec.loader.exec_module(builder)
    sys.path.insert(0, str(args.prelink_tools.resolve()))
    from kc_image import Image
    source = args.out / 'weak-callback.c'
    source.write_text(SOURCE)
    weak = args.out / 'absent.S'
    weak.write_text('.globl _optional_callback\n.weak_definition _optional_callback\n_optional_callback = 0\n')
    strong = args.out / 'present.c'
    strong.write_text('void optional_callback(void) { __asm__ volatile("" ::: "memory"); }\n')
    objects = []
    for path in (source, weak, strong):
        obj = path.with_suffix('.o')
        subprocess.run([str(args.clang), '-target', 'arm64e-apple-ios27.2', '-mkernel',
                        '-ffreestanding', '-fno-stack-protector', '-O2', '-c', str(path), '-o', str(obj)], check=True)
        objects.append(obj)
    reports = []
    for mode in ('absent', 'strong-replacement'):
        output = args.out / (mode + '.macho')
        # The archive builder omits NULL aliases for actual providers.
        selected = objects[:2] if mode == 'absent' else [objects[0], objects[2]]
        subprocess.run([str(args.linker), '-arch', 'arm64e', '-kernel', '-static', '-pie', '-fixup_chains',
                        '-pagezero_size', '0',
                        '-image_base', '0xfffffff007004000', '-e', '_kernel_entry',
                        '-rename_section', '__TEXT', '__text', '__TEXT_EXEC', '__text',
                        '-segprot', '__TEXT_EXEC', 'r-x', 'r-x',
                        '-sectcreate', '__TEXT', '__const', '/dev/null',
                        '-platform_version', 'ios', '27.2', '27.2', '-o', str(output),
                        *map(str, selected)], check=True)
        data = output.read_bytes()
        header = struct.unpack_from('<4I', data)
        if header[:2] != (0xfeedfacf, 0x100000c) or header[2] & 0xff != 2 or header[3] != 2:
            raise RuntimeError('Linker fixture is not an arm64e executable')
        image = Image(data)
        names = {name: address for address, group in image.symbols().items() for name in group}
        table, callback = names['_callbacks'], names['_present_callback']
        chains = image.fixups()
        for slot in (table, table + 16):
            if chains.get(slot, ())[:2] != (callback, 1):
                raise RuntimeError('Authenticated live callbacks were lost across the NULL slot')
        if mode == 'absent':
            if image.read(table + 8, 8) != bytes(8) or table + 8 in chains:
                raise RuntimeError('Absent weak callback is not an unchained NULL pointer')
        elif chains.get(table + 8, ())[:2] != (names['_optional_callback'], 1):
            raise RuntimeError('Actual provider did not replace the absent-provider configuration')
        exports = builder.provider_exports(output)
        if '_present_callback' not in exports or \
                ('_optional_callback' in exports) != (mode == 'strong-replacement'):
            raise RuntimeError('Provider discovery confused an absent NULL alias with a live callback')
        reports.append({'mode': mode, 'result': 'PASS', 'kernel_base': hex(image.base),
                        'live_callback_authentication': 'PASS', 'provider_discovery': 'PASS',
                        'path': str(output)})
    report = {'result': 'PASS', 'checks': reports, 'hardware_qualified': False}
    (args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
