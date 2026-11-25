#!/usr/bin/env python3
"""Build a private Mach-O kernel linker with the matching LLVM LTO library."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import struct
import time

ROOT = Path(__file__).resolve().parents[1]
CCTOOLS_REVISION = '3e694ee7393af2a4ce8bd6e7dae28fe37450aa87'
LLVM_REVISION = '58c46bae21181fb4240cf964461bea738d96d50e'


def run(command, **kwargs):
    print('Running:', ' '.join(map(str, command)), flush=True)
    return subprocess.run(list(map(str, command)), check=True, **kwargs)


def apply_patches(source, out):
    receipt = out / 'source-patches.json'
    diff_command = ['git', '-C', str(source), 'diff', 'HEAD', '--', '.',
                    ':(exclude)cctools/include/llvm-c/lto.h']
    diff = subprocess.check_output(diff_command, text=True)
    if diff:
        previous = json.loads(receipt.read_text()) if receipt.exists() else {}
        if previous.get('diff') != diff:
            raise RuntimeError('Independent linker source edits retained: ' + str(source))
        run(['git', '-C', source, 'apply', '--reverse', '--check', '-'], input=diff.encode())
        run(['git', '-C', source, 'apply', '--reverse', '-'], input=diff.encode())
    paths = [ROOT / 'patches/toolchain/ld64-null-weak-auth-pointer.patch']
    for path in paths:
        run(['git', '-C', source, 'apply', '--check', path])
        run(['git', '-C', source, 'apply', path])
    diff = subprocess.check_output(diff_command, text=True)
    receipt.write_text(json.dumps({'revision': CCTOOLS_REVISION, 'diff': diff,
                                   'patches': [str(path) for path in paths]}, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--llvm-build', type=Path, required=True)
    parser.add_argument('--source', type=Path, help='private cctools-port checkout at the pinned revision')
    parser.add_argument('--base-toolchain', type=Path, default=Path('/opt/unixnu-linux'))
    parser.add_argument('--jobs', type=int, default=8)
    args = parser.parse_args()
    out, llvm = args.out.resolve(), args.llvm_build.resolve()
    if out == ROOT or ROOT in out.parents or args.jobs < 1:
        parser.error('Use an output directory outside the device repository and positive --jobs')
    source = args.source.resolve() if args.source else out / 'source'
    if source == ROOT or ROOT in source.parents:
        parser.error('The linker source must also be outside the device repository')
    out.mkdir(parents=True, exist_ok=True)
    if not (source / '.git').exists():
        run(['git', 'clone', '--quiet', '--depth', '1', '--single-branch',
             '--branch', '1030.6.3-ld64-956.6',
             'https://github.com/tpoechtrager/cctools-port.git', source])
    actual = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != CCTOOLS_REVISION:
        parser.error('Linker source differs from the pinned cctools-port revision')
    apply_patches(source, out)
    changes = subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain',
                                       '--untracked-files=no', '--', 'cctools/include/llvm-c/lto.h'], text=True)
    if changes:
        # cctools configure copies the selected LLVM API header into its own
        # source directory, including when configured in a separate build tree.
        # Only that exact generated header is safe to reuse.
        include = Path(subprocess.check_output([str(llvm / 'bin/llvm-config'), '--includedir'],
                                               text=True).strip())
        generated = source / 'cctools/include/llvm-c/lto.h'
        if any(line[3:] != 'cctools/include/llvm-c/lto.h' for line in changes.splitlines()) or \
                generated.read_bytes() != (include / 'llvm-c/lto.h').read_bytes():
            parser.error('Keep independent linker source edits in a separate checkout')
    run(['ninja', '-C', llvm, '-j' + str(args.jobs), 'LTO', 'llvm-config', 'llvm-nm'])
    compiler = subprocess.check_output([str(llvm / 'bin/clang'), '--version'], text=True)
    if LLVM_REVISION not in compiler:
        parser.error('The LTO library must come from the pinned clang build')
    dispatch = args.base_toolchain / 'CommandLineTools/usr/libdispatch'
    if not (dispatch / 'lib/libdispatch.so').is_file():
        parser.error('The base UniXNU toolchain has no private libdispatch')
    build, prefix = out / 'build', out / 'prefix'
    build.mkdir(exist_ok=True)
    env = dict(os.environ, CC='/usr/bin/clang', CXX='/usr/bin/clang++')
    command = [source / 'cctools/configure', '--prefix=' + str(prefix),
               '--target=arm64-apple-darwin', '--program-prefix=', '--enable-lto-support',
               '--disable-xar-support', '--with-llvm-config=' + str(llvm / 'bin/llvm-config'),
               '--with-libdispatch=' + str(dispatch), '--with-libblocksruntime=' + str(dispatch)]
    state = {'cctools_revision': CCTOOLS_REVISION, 'llvm_revision': LLVM_REVISION,
             'jobs': args.jobs, 'started': time.time(), 'shared_toolchain_written': False,
             'stage': 'configure'}
    record = out / 'build-state.json'
    try:
        record.write_text(json.dumps(state, indent=2) + '\n')
        run(command, cwd=build, env=env)
        state['stage'] = 'compile'
        record.write_text(json.dumps(state, indent=2) + '\n')
        run(['make', '-j' + str(args.jobs), 'PROGRAM_PREFIX='], cwd=build, env=env)
        run(['make', 'install', 'PROGRAM_PREFIX='], cwd=build, env=env)
        checks = out / 'checks'
        checks.mkdir(exist_ok=True)
        source_fixture = checks / 'kernel-lto.c'
        source_fixture.write_text('int kernel_lto_fixture(int value) { return value + 27; }\n')
        bitcode, obj, generated = checks / 'bitcode.o', checks / 'kernel-lto.o', checks / 'generated.o'
        anchor_source, anchor = checks / 'anchor.c', checks / 'anchor.o'
        anchor_source.write_text('extern int kernel_lto_fixture(int);\n'
                                 'int kernel_lto_anchor(void) { return kernel_lto_fixture(0); }\n')
        run([llvm / 'bin/clang', '-target', 'arm64e-apple-ios27.2', '-mkernel', '-flto',
             '-O2', '-c', source_fixture, '-o', bitcode])
        run([llvm / 'bin/clang', '-target', 'arm64e-apple-ios27.2', '-mkernel',
             '-O2', '-c', anchor_source, '-o', anchor])
        env['LIBLTO_PATH'] = str(llvm / 'lib/libLTO.so')
        env['LIBLTO'] = env['LIBLTO_PATH']
        run([llvm / 'bin/clang', '-target', 'arm64e-apple-ios27.2',
             '-nostdlib', '-fuse-ld=' + str(prefix / 'bin/ld'), '-Wl,-r',
             '-Wl,-object_path_lto,' + str(generated), anchor, bitcode, '-o', obj], env=env)
        symbols = subprocess.check_output([str(llvm / 'bin/llvm-nm'), '--defined-only',
                                           '--just-symbol-name', str(obj)], text=True)
        if '_kernel_lto_fixture' not in symbols.splitlines() or not generated.exists():
            raise RuntimeError('LTO did not produce the expected Mach-O object and saved code')
        header = struct.unpack_from('<4I', obj.read_bytes())
        if header[0:2] != (0xfeedfacf, 0x100000c) or header[2] & 0xff != 2 or header[3] != 1:
            raise RuntimeError('LTO output is not an arm64e MH_OBJECT')
        state.update(stage='complete', linker=str(prefix / 'bin/ld'),
                     liblto=str(llvm / 'lib/libLTO.so'), lto_object_check='PASS',
                     finished=time.time())
    except Exception as error:
        state.update(stage='failed', error=str(error))
        raise
    finally:
        record.write_text(json.dumps(state, indent=2) + '\n')
    print(json.dumps(state, indent=2))


if __name__ == '__main__':
    main()
