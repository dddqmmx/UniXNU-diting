"""Run the patched, pinned station and its real crypto against a host AP.

No phone, firmware, shared build cache or image verification is used.
"""
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
from diting_wifi_station import prepare_station


class WifiStationTests(unittest.TestCase):
    def test_wpa2_data_retries_disconnect_and_key_rotation(self):
        shared = input_path('wifi_shared')
        linux = input_path('wifi_linux')
        clang = os.environ.get('DITING_WIFI_HOST_CLANG') or shutil.which('clang')
        if not clang or not (shared / 'src/nw_sta.c').is_file() or not (linux / 'include/net/mac80211.h').is_file():
            self.skipTest('Host clang and the pinned Wi-Fi/Linux sources are required')
        with tempfile.TemporaryDirectory(prefix='diting-wifi-station-') as directory:
            output = Path(directory)
            station = prepare_station(shared / 'src/nw_sta.c', output)
            # Use the same real mac80211 ABI and shim overlays as the native
            # archive; glibc's Linux typedefs cannot replace these headers.
            pinned = output / 'pinned'
            headers = {
                'linux/mhi.h': 'linux/mhi.h',
                'linux/ieee80211.h': 'linux/ieee80211.h',
                'linux/soc/qcom/qmi.h': 'linux/soc/qcom/qmi.h',
                'net/cfg80211.h': 'net/cfg80211.h',
                'net/regulatory.h': 'net/regulatory.h',
                'net/codel.h': 'net/codel.h',
                'net/ieee80211_radiotap.h': 'net/ieee80211_radiotap.h',
                'linux/nl80211.h': 'uapi/linux/nl80211.h',
                'linux/rfkill.h': 'uapi/linux/rfkill.h',
                'uapi/linux/rfkill.h': 'uapi/linux/rfkill.h',
                'net/mac80211.real.h': 'net/mac80211.h',
            }
            headers.update({str(p.relative_to(linux / 'include')): str(p.relative_to(linux / 'include'))
                            for p in (linux / 'include/linux').glob('ieee80211-*.h')})
            for destination, source in headers.items():
                target = pinned / destination
                target.parent.mkdir(parents=True, exist_ok=True)
                target.symlink_to(linux / 'include' / source)
            resource = subprocess.check_output([clang, '-print-resource-dir'], text=True).strip()
            prefix = []
            if shutil.which('nice'):
                prefix += ['nice', '-n', '19']
            if shutil.which('ionice'):
                prefix += ['ionice', '-c', '3']
            flags = ['-std=gnu11', '-O1', '-g', '-w', '-nostdinc',
                     '-isystem', str(shared / 'shim/libc'), '-isystem', resource + '/include',
                     '-I', str(shared / 'shim/include'), '-I', str(shared / 'include'),
                     '-I', str(shared / 'src'), '-I', str(pinned), '-include', 'errno.h',
                     '-include', str(ROOT / 'drivers/DitingWifi/ath11k/compat/DitingCompat.h'),
                     '-include', str(ROOT / 'tests/diting_wifi_host.h'),
                     '-DLX_BOOT_SETTLE_MS=0', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            sources = [ROOT / 'tests/diting_wifi_station.c', station,
                       shared / 'src/nw_mac80211.c', shared / 'src/nw_net.c',
                       shared / 'src/nw_crypto.c', shared / 'shim/src/lx_rt.c']
            binary = output / 'station-test'
            result = subprocess.run([*prefix, clang, *flags, *map(str, sources), '-o', str(binary)],
                                    capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([*prefix, str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('PASS:', result.stdout)
            print(result.stdout.strip())


if __name__ == '__main__':
    unittest.main()
