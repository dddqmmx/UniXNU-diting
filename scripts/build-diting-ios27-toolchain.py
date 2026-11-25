#!/usr/bin/env python3
"""Build a private Linux clang with the XNU authenticated-null qualifier."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
LLVM_REVISION = '58c46bae21181fb4240cf964461bea738d96d50e'


def run(command):
    print('Running:', ' '.join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--unixnu', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--llvm-source', type=Path, help='optional private checkout at the pinned LLVM revision')
    parser.add_argument('--base-toolchain', type=Path, default=Path('/opt/unixnu-linux'))
    parser.add_argument('--jobs', type=int, default=8)
    args = parser.parse_args()
    out = args.out.resolve()
    if out == ROOT or ROOT in out.parents or args.jobs < 1:
        parser.error('Use a private output directory outside the repository and positive --jobs')
    out.mkdir(parents=True, exist_ok=True)
    source = args.llvm_source.resolve() if args.llvm_source else out / 'llvm-project'
    if not (source / '.git').exists():
        source.mkdir(parents=True, exist_ok=True)
        run(['git', 'init', '-q', source])
        run(['git', '-C', source, 'remote', 'add', 'origin', 'https://github.com/llvm/llvm-project.git'])
        run(['git', '-C', source, 'sparse-checkout', 'set', '--cone', 'llvm', 'clang', 'cmake', 'third-party', 'libc'])
        run(['git', '-C', source, 'fetch', '--quiet', '--depth', '1', '--filter=blob:none', 'origin', LLVM_REVISION])
        run(['git', '-C', source, 'checkout', '--quiet', '--detach', 'FETCH_HEAD'])
    actual = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != LLVM_REVISION:
        parser.error('LLVM checkout differs from the pinned compiler source')
    if not (source / 'libc/CMakeLists.txt').is_file():
        run(['git', '-C', source, 'sparse-checkout', 'add', 'libc'])
    patches = [args.unixnu.resolve() / 'scripts/linux/llvm-xnu-builtin-type.patch',
               ROOT / 'patches/toolchain/clang-ptrauth-auth-null.patch']
    for patch in patches:
        applied = subprocess.run(['git', '-C', str(source), 'apply', '--reverse', '--check', str(patch)],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if applied.returncode:
            run(['git', '-C', source, 'apply', '--check', patch])
            run(['git', '-C', source, 'apply', patch])
    build = out / 'build'
    record = out / 'build-state.json'
    state = {'source_commit': LLVM_REVISION, 'jobs': args.jobs, 'shared_toolchain_written': False,
             'stage': 'configure', 'started': time.time()}
    try:
        record.write_text(json.dumps(state, indent=2) + '\n')
        run(['cmake', '-S', source / 'llvm', '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
             '-DCMAKE_C_COMPILER=/usr/bin/clang', '-DCMAKE_CXX_COMPILER=/usr/bin/clang++',
             '-DLLVM_ENABLE_PROJECTS=clang', '-DLLVM_TARGETS_TO_BUILD=AArch64', '-DLLVM_ENABLE_ASSERTIONS=ON',
             '-DLLVM_INCLUDE_TESTS=OFF', '-DCLANG_INCLUDE_TESTS=OFF', '-DLLVM_INCLUDE_BENCHMARKS=OFF',
             '-DLLVM_ENABLE_TERMINFO=OFF', '-DLLVM_ENABLE_LIBEDIT=OFF', '-DLLVM_ENABLE_LIBXML2=OFF',
             '-DLLVM_PARALLEL_LINK_JOBS=1'])
        state['stage'] = 'compile'
        record.write_text(json.dumps(state, indent=2) + '\n')
        run(['ninja', '-C', build, '-j' + str(args.jobs), 'clang', 'clang-resource-headers'])
        run([sys.executable, ROOT / 'scripts/check-diting-ptrauth-compiler.py', '--clang', build / 'bin/clang',
             '--out', out / 'checks'])
        toolchain = out / 'toolchain'
        toolchain.mkdir(exist_ok=True)
        links = {'llvm': build, 'CommandLineTools': args.base_toolchain / 'CommandLineTools',
                 'XcodeDefault.xctoolchain': args.base_toolchain / 'XcodeDefault.xctoolchain',
                 'bin': args.base_toolchain / 'bin'}
        for name, target in links.items():
            path = toolchain / name
            if path.is_symlink() and path.resolve() != target.resolve():
                raise RuntimeError('Existing toolchain link differs: ' + str(path))
            if not path.exists():
                path.symlink_to(target)
        state.update(stage='complete', clang=str(build / 'bin/clang'), toolchain=str(toolchain), finished=time.time())
    except Exception as error:
        state.update(stage='failed', error=str(error))
        raise
    finally:
        record.write_text(json.dumps(state, indent=2) + '\n')
    print(json.dumps(state, indent=2))


if __name__ == '__main__':
    main()
