"""Compile the actual Darwin adapter and actual pinned-wiphy extension on host."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from diting_native_inputs import input_path


def idle_prefix():
    result = []
    if shutil.which('chrt'):
        result += ['chrt', '--idle', '0']
    if shutil.which('nice'):
        result += ['nice', '-n', '19']
    if shutil.which('ionice'):
        result += ['ionice', '-c', '3']
    return result


def pinned_headers(linux, output):
    headers = {name: name for name in [
        'linux/mhi.h', 'linux/ieee80211.h', 'linux/soc/qcom/qmi.h',
        'net/cfg80211.h', 'net/regulatory.h', 'net/codel.h', 'net/ieee80211_radiotap.h']}
    headers.update({'linux/nl80211.h': 'uapi/linux/nl80211.h',
                    'linux/rfkill.h': 'uapi/linux/rfkill.h',
                    'uapi/linux/rfkill.h': 'uapi/linux/rfkill.h',
                    'net/mac80211.real.h': 'net/mac80211.h'})
    headers.update({str(p.relative_to(linux / 'include')): str(p.relative_to(linux / 'include'))
                    for p in (linux / 'include/linux').glob('ieee80211-*.h')})
    for destination, source in headers.items():
        target = output / destination
        target.parent.mkdir(parents=True, exist_ok=True)
        target.symlink_to(linux / 'include' / source)


class WifiNetworkTests(unittest.TestCase):
    def compile_and_run(self, compiler, flags, sources, output):
        command = [*idle_prefix(), compiler, *flags, *map(str, sources), '-o', str(output)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=90)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run([*idle_prefix(), str(output)], capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS:', result.stdout)
        print(result.stdout.strip())

    def test_native_socket_abi_events_failures_and_buffers(self):
        shared = input_path('wifi_shared')
        clang = os.environ.get('DITING_WIFI_HOST_CXX') or shutil.which('clang++')
        if not clang or not (shared / 'include/nw_abi.h').is_file():
            self.skipTest('Host clang++ and the pinned station ABI are required')
        with tempfile.TemporaryDirectory(prefix='diting-wifi-net-') as directory:
            flags = ['-std=gnu++17', '-O1', '-g', '-Wall', '-Wextra',
                     '-Wno-unused-function', '-Wno-missing-field-initializers',
                     '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                     '-I', str(shared / 'include')]
            self.compile_and_run(clang, flags, [ROOT / 'tests/diting_wifi_net.cpp'], Path(directory) / 'net-test')

    def test_actual_wiphy_restrictions_and_widths(self):
        shared, linux = input_path('wifi_shared'), input_path('wifi_linux')
        clang = os.environ.get('DITING_WIFI_HOST_CLANG') or shutil.which('clang')
        if not clang or not (shared / 'src/nw_mac80211.h').is_file() or not (linux / 'include/net/mac80211.h').is_file():
            self.skipTest('Host clang and the pinned wireless/Linux headers are required')
        with tempfile.TemporaryDirectory(prefix='diting-wifi-channels-') as directory:
            output = Path(directory)
            pinned_headers(linux, output / 'pinned')
            resource = subprocess.check_output([clang, '-print-resource-dir'], text=True).strip()
            flags = ['-std=gnu11', '-O1', '-g', '-w', '-nostdinc',
                     '-isystem', str(shared / 'shim/libc'), '-isystem', resource + '/include',
                     '-I', str(shared / 'shim/include'), '-I', str(shared / 'include'),
                     '-I', str(shared / 'src'), '-I', str(output / 'pinned'),
                     '-include', 'errno.h',
                     '-include', str(ROOT / 'drivers/DitingWifi/ath11k/compat/DitingCompat.h'),
                     '-include', str(ROOT / 'tests/diting_wifi_host.h'),
                     '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            self.compile_and_run(clang, flags, [ROOT / 'tests/diting_wifi_channels.c',
                ROOT / 'drivers/DitingWifi/ath11k/DitingWifiChannels.c'], output / 'channels-test')


if __name__ == '__main__':
    unittest.main()
