"""Keep native build tools and optimized iOS GPU compilation distinct on Linux."""
import os
from pathlib import Path
import shutil
import subprocess
import sys


def environment(sdk=None, linker=None, codesign=None):
    env = dict(os.environ)
    if codesign:
        if not Path(codesign).is_file() or not os.access(codesign, os.X_OK):
            raise ValueError('--codesign must name an executable signing tool')
        env['DITING_CODESIGN'] = str(Path(codesign).resolve())
    if sys.platform != 'linux':
        return env
    if not env.get('UNIXNU_TOOLCHAIN') or not sdk or not codesign:
        raise ValueError('Linux needs the UniXNU toolchain, --sdk and a real --codesign tool')
    if not Path(sdk).is_dir():
        raise ValueError('the iPhoneOS SDK directory is missing')
    linker = Path(linker or shutil.which('ld64.lld') or '/usr/bin/ld64.lld')
    if not linker.is_file() or not os.access(linker, os.X_OK):
        raise ValueError('Linux needs an executable Mach-O linker with SDK text-stub support (--linker)')
    ar = shutil.which('llvm-ar')
    if not ar:
        raise ValueError('Linux userland needs llvm-ar')
    prefix = Path(env['UNIXNU_TOOLCHAIN'])
    env.update(UNIXNU_SDKROOT=str(Path(sdk).resolve()), SDKROOT=str(Path(sdk).resolve()),
               XNU_TOOLCHAIN_BIN=str(prefix / 'CommandLineTools/usr/bin') + ' ' + str(prefix / 'llvm/bin'),
               DARWIN_TRIPLE='arm64-apple-ios18.0',
               # argv[0] selects Mach-O mode; do not resolve ld64.lld's symlink.
               CROSS_EXTRA_FLAGS='-Qunused-arguments --ld-path=' + str(linker.absolute()) + ' -mlinker-version=956.6',
               CC_FOR_BUILD='/usr/bin/cc', CXX_FOR_BUILD='/usr/bin/c++')
    env['PATH'] = os.pathsep.join([str(prefix / 'CommandLineTools/usr/bin'),
                                 str(prefix / 'llvm/bin'), env['PATH']])
    host = next(line.split(': ', 1)[1] for line in
                subprocess.check_output(['rustc', '-vV'], text=True).splitlines()
                if line.startswith('host: '))
    env['CARGO_TARGET_' + host.upper().replace('-', '_') + '_LINKER'] = '/usr/bin/cc'
    env['CC_' + host.replace('-', '_')] = '/usr/bin/cc'
    env['CXX_' + host.replace('-', '_')] = '/usr/bin/c++'
    env['CC_aarch64_apple_ios'] = str(prefix / 'CommandLineTools/usr/bin/clang')
    env['AR_aarch64_apple_ios'] = ar
    return env
