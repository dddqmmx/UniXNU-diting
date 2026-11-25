#!/usr/bin/env python3
"""Exercise the selected production loader parser and both boot_args layouts."""
import argparse
import ctypes
import json
from pathlib import Path
import re
import struct
import subprocess


def fixture():
    base = 0xfffffff007004000
    subtype = 0xc0000002
    entry = base + 0x1400

    def segment(address, offset, size):
        return struct.pack('<II16sQQQQiiII', 0x19, 72, b'__TEXT',
                           address, size, offset, size, 5, 5, 0, 0)

    def thread():
        command = bytearray(288)
        struct.pack_into('<4I', command, 0, 5, 288, 6, 68)
        struct.pack_into('<Q', command, 272, entry)
        return command

    def index(name, address, offset):
        name = name.encode() + b'\0'
        length = (32 + len(name) + 7) & ~7
        return struct.pack('<IIQQII', 0x80000035, length, address, offset, 32, 0) + \
            name + bytes(length - 32 - len(name))

    commands = segment(base, 0, 8192) + thread() + \
        index('com.apple.kernel', base + 4096, 4096) + \
        index('com.apple.fixture', base + 6144, 6144)
    data = bytearray(8192)
    struct.pack_into('<8I', data, 0, 0xfeedfacf, 0x100000c, subtype,
                     12, 4, len(commands), 0, 0)
    data[32:32 + len(commands)] = commands
    commands = segment(base + 4096, 4096, 4096) + thread()
    struct.pack_into('<8I', data, 4096, 0xfeedfacf, 0x100000c, subtype,
                     2, 2, len(commands), 0x80000000, 0)
    data[4128:4128 + len(commands)] = commands
    struct.pack_into('<8I', data, 6144, 0xfeedfacf, 0x100000c, subtype,
                     0xb, 0, 0, 0x80000000, 0)
    return data, base


def check_boot_args(source, tree, out, clang):
    loader_header = source / 'DarwinBootPkg/Driver/BootArgs.h'
    xnu_header = tree / '.cache/xnu-work/pexpert/pexpert/arm64/boot.h'
    loader = loader_header.read_text()
    xnu = xnu_header.read_text()
    video = re.search(r'struct Boot_Video \{.*?\};', xnu, re.S).group(0)
    args = re.search(r'typedef struct boot_args \{.*?\} boot_args;', xnu, re.S).group(0)
    line = re.search(r'^#define BOOT_LINE_LENGTH\s+(\d+)', xnu, re.M).group(1)
    loader = loader.replace('#include <Base.h>', '')
    code = '''#include <stdint.h>
#include <stddef.h>
typedef uint8_t UINT8; typedef uint16_t UINT16; typedef uint32_t UINT32;
typedef uint64_t UINT64; typedef char CHAR8;
#define STATIC_ASSERT(c,m) _Static_assert(c,m)
#define OFFSET_OF(t,f) offsetof(t,f)
_Static_assert(sizeof(void *) == 8 && sizeof(unsigned long) == 8, "LP64 host required");
'''
    code += loader + '\n#define BOOT_LINE_LENGTH ' + line + '\n' + video + \
        '\ntypedef struct Boot_Video Boot_Video;\n' + args + '\n'
    fields = {'Revision': 'Revision', 'Version': 'Version', 'VirtualBase': 'virtBase',
              'PhysicalBase': 'physBase', 'MemorySize': 'memSize',
              'TopOfKernelData': 'topOfKernelData', 'Video': 'Video',
              'MachineType': 'machineType', 'DeviceTree': 'deviceTreeP',
              'DeviceTreeLength': 'deviceTreeLength', 'CommandLine': 'CommandLine',
              'BootFlags': 'bootFlags', 'MemorySizeActual': 'memSizeActual'}
    for loader_field, xnu_field in fields.items():
        code += f'_Static_assert(offsetof(DARWIN_BOOT_ARGS,{loader_field}) == ' \
            f'offsetof(boot_args,{xnu_field}), "{loader_field} ABI");\n'
    code += '_Static_assert(sizeof(DARWIN_BOOT_ARGS) == sizeof(boot_args), "size ABI");\n'
    code += '_Static_assert(sizeof(((boot_args *)0)->CommandLine) == 1024, "command ABI");\n'
    path = out / 'boot-args.c'
    path.write_text(code)
    subprocess.run([clang, '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsyntax-only',
                    str(path)], check=True)
    return {'result': 'PASS', 'loader_header': str(loader_header),
            'xnu_header': str(xnu_header), 'compared_fields': list(fields),
            'bytes': 0x480, 'command_line_bytes': 1024}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--tree', type=Path, required=True)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--reference-fileset', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--clang', default='clang')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    source = (args.source / 'DarwinBootPkg/Driver/DarwinBoot.c').read_text()
    types = source[source.index('typedef struct {'):source.index('STATIC EFI_HANDLE')]
    production = source[source.index('STATIC\nEFI_STATUS\nParseKernelHeader'):
                        source.index('STATIC\nBOOLEAN\nAdtNode')]
    # The parser's code is compiled verbatim; only its UEFI primitive types are supplied here.
    preamble = '''#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint8_t UINT8; typedef uint32_t UINT32; typedef uint64_t UINT64;
typedef size_t UINTN; typedef int BOOLEAN; typedef int EFI_STATUS;
#define STATIC static
#define CONST const
#define TRUE 1
#define FALSE 0
#define EFI_SUCCESS 0
#define EFI_UNSUPPORTED 3
#define EFI_LOAD_ERROR 1
#define MAX_UINT64 UINT64_MAX
#define WINDOW_SIZE 0x10000000ULL
#define SIZE_2MB 0x200000ULL
#define EFI_PAGE_MASK 4095
#define MAX_SEGMENTS 64
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define ZeroMem(p,n) memset(p,0,n)
#define CompareMem(a,b,n) memcmp(a,b,n)
#define EFI_ERROR(s) ((s) != EFI_SUCCESS)
static UINT32 U32(const void *p) { UINT32 v; memcpy(&v,p,4); return v; }
static UINT64 U64(const void *p) { UINT64 v; memcpy(&v,p,8); return v; }
'''
    path = args.out / 'parser.c'
    path.write_text(preamble + types + production +
                    '\nint check(const UINT8 *p, UINTN n) { KERNEL k; return ParseKernel(p,n,&k); }\n')
    library = args.out / 'parser.so'
    subprocess.run([args.clang, '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                    str(path), '-o', str(library)], check=True)
    lib = ctypes.CDLL(str(library.resolve()))
    lib.check.argtypes = [ctypes.c_void_p, ctypes.c_size_t]

    def check(data):
        return lib.check(ctypes.create_string_buffer(bytes(data)), len(data))

    data, base = fixture()
    if check(data):
        raise RuntimeError('Production parser refused a valid fileset fixture')
    index = 32 + 72 + 288
    second = index + struct.unpack_from('<I', data, index + 4)[0]
    cases = [
        ('missing-kernel-member', index + 32, '<16s', b'com.apple.absent'),
        ('duplicate-kernel-member', second + 32, '<17s', b'com.apple.kernel\0'),
        ('unmapped-index-va', index + 8, '<Q', base + 0x3000),
        ('index-offset-outside-file', index + 16, '<Q', 8192),
        ('unaligned-index-header', index + 16, '<Q', 4097),
        ('index-name-outside-command', index + 24, '<I', 64),
        ('member-not-executable', 4096 + 12, '<I', 0xb),
        ('member-without-fileset-flag', 4096 + 24, '<I', 0),
        ('member-subtype-differs', 4096 + 8, '<I', 2),
        ('member-entry-differs', 4128 + 72 + 272, '<Q', base + 0x1404),
        ('member-entry-unmapped', 4128 + 72 + 272, '<Q', base + 0x3000),
        ('member-file-mapping-differs', 4128 + 40, '<Q', 0),
        ('member-range-outside-root', 4128 + 32, '<Q', 8192),
        ('member-executable-uncovered', 4128 + 60, '<I', 7),
    ]
    for name, offset, fmt, value in cases:
        bad = bytearray(data)
        struct.pack_into(fmt, bad, offset, value)
        if check(bad) != 1:
            raise RuntimeError('Malformed fileset was not refused: ' + name)
    if check(args.kernel.read_bytes()):
        raise RuntimeError('Production parser refused the actual native kernel')
    # Preserve the selected component's existing architecture/range regressions as well.
    subprocess.run(['python3', str(args.source / 'Test/test-kernel-parser.py'),
                    str(args.kernel)], check=True)
    report = {'production_parser': 'PASS', 'valid_fileset_fixture': 'PASS',
              'malformed_filesets_refused': [case[0] for case in cases],
              'native_kernel': str(args.kernel), 'native_kernel_parser': 'PASS',
              'boot_args': check_boot_args(args.source, args.tree, args.out, args.clang),
              'kernel_collection_qualified': False, 'hardware_qualified': False}
    if args.reference_fileset:
        report['original_restore_fileset'] = {'path': str(args.reference_fileset),
                                             'parser_status': check(args.reference_fileset.read_bytes()),
                                             'used_as_boot_candidate': False}
    (args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
