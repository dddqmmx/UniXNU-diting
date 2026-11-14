"""Use the UniXNU Linux toolchain for the Darwin 24 kernel and archives."""
import os
import platform
from pathlib import Path


def require_host():
    if platform.system() == 'Darwin' and platform.machine() == 'x86_64':
        return
    if platform.system() == 'Linux' and os.environ.get('UNIXNU_TOOLCHAIN'):
        return
    raise SystemExit('Use Intel macOS/Xcode or the UniXNU Linux toolchain (UNIXNU_TOOLCHAIN)')


def linux_env(tree):
    prefix = Path(os.environ.get('UNIXNU_TOOLCHAIN', '/opt/unixnu-linux'))
    clt = prefix / 'CommandLineTools/usr'
    xct = prefix / 'XcodeDefault.xctoolchain/usr'
    for path in (prefix / 'llvm/bin/clang', clt / 'bin/ld', clt / 'bin/xnu-host-cc',
                 clt / 'libexec/migcom', xct / 'bin/iig', Path('/usr/bin/xcrun')):
        if not path.exists():
            raise SystemExit(f'Missing UniXNU Linux toolchain input: {path}')
    fakeroot = tree / '.cache/fakeroot'
    return dict(UNIXNU_TOOLCHAIN=str(prefix), CLT_PREFIX=str(clt),
                XCTOOLCHAIN_PREFIX=str(xct), XNU_TOOLCHAIN_BIN=f'{clt / "bin"} {xct / "bin"}',
                UNIXNU_SDKROOT=str(fakeroot), UNIXNU_SDK_VERSION='18.0',
                UNIXNU_SDK_BUILD='22H374',
                UNIXNU_PLATFORM_PATH=str(fakeroot / 'Platforms/iPhoneOS.platform'),
                CROSS_CLANG=str(prefix / 'llvm/bin/clang'), DARWIN_TRIPLE='arm64-apple-ios18.0',
                # Upstream AArch64 clang's -mkernel still permits implicit SIMD.
                # Kernel/interrupt code must not borrow the thread's FP state
                # for integer copies. Explicit SIMD intrinsics/assembly remain.
                CROSS_EXTRA_FLAGS='-mllvm -disable-atexit-based-global-dtor-lowering -mno-implicit-float',
                CCC_OVERRIDE_OPTIONS='# +-Wno-missing-designated-field-initializers +-Wno-unused-but-set-global '
                                     '+-Wno-c99-designator +-Wno-reorder-init-list +-Wno-initializer-overrides '
                                     '+-Wno-unnecessary-virtual-specifier',
                PATH=os.pathsep.join([str(clt / 'bin'), str(prefix / 'bin'), os.environ.get('PATH', '')]))
