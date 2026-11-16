#!/usr/bin/env python3
"""Build the complete optimized A730 ABI1 Metal runtime for 22H374.

Uses the selected recipes in an output-only overlay and the existing pinned
source preparation scripts. No OS or phone is modified. Compilation is never
a hardware performance result.
"""
import argparse
from datetime import datetime, timezone
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
from diting_gpu_build_host import environment

ROOT = Path(__file__).resolve().parents[1]
MESA = '85fda3b36405bcaea8bffc64aa4f2651dc90770d'
TRANSLATOR = '624bab3918de5b978e1101517abca2b4fa426565'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mesa', required=True, type=Path)
    parser.add_argument('--translator', required=True, type=Path)
    parser.add_argument('--tools-directory', required=True, type=Path)
    parser.add_argument('--venv', required=True, type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--sdk', type=Path, help='explicit iPhoneOS SDK for Linux')
    parser.add_argument('--codesign', type=Path, help='real codesign-compatible tool; required on Linux')
    parser.add_argument('--linker', type=Path, help='Mach-O linker with SDK text-stub support on Linux (default: ld64.lld)')
    parser.add_argument('--prime-list', type=Path, help='metallib paths from this 22H374 OS')
    args = parser.parse_args()
    mesa, translator = args.mesa.resolve(), args.translator.resolve()
    mesa_info = json.loads((mesa / '.diting-mesa.json').read_text())
    metal_info = json.loads((translator / '.diting-parity.json').read_text())
    if (mesa_info.get('mesa'), mesa_info.get('guest'), mesa_info.get('native_abi')) != (MESA, '22H374', 1):
        parser.error('prepare the pinned A730 ABI1 Mesa source first')
    if (metal_info.get('translator'), metal_info.get('guest')) != (TRANSLATOR, '22H374'):
        parser.error('prepare the matching 22H374 translator source first')
    abi = ROOT / 'drivers/DitingAdreno/submit'
    if 'NativeAdrenoAbiVersion = 1' not in (abi / 'NativeAdrenoABI.h').read_text():
        parser.error('this runtime requires the selected Diting kernel ABI1')
    try:
        env = environment(args.sdk, args.linker, args.codesign)
    except ValueError as error:
        parser.error(str(error))
    out = args.output_dir.resolve()
    if out == ROOT or ROOT in out.parents:
        parser.error('generated GPU runtime must stay outside the source checkout')
    out.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location('diting_runtime_recipes', ROOT / 'kits/DitingPlatform24/build-with-runtime.py')
    recipes = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(recipes)
    overlay = recipes.overlay(ROOT, out / 'runtime-source')
    record = dict(guest='22H374', native_abi=1, mesa=MESA, translator=TRANSLATOR,
                  started_utc=datetime.now(timezone.utc).isoformat(), stages=[],
                  hardware_acceleration_confirmed=False, device_performance='UNTESTED')

    def run(stage, argv):
        log = out / (stage + '.log')
        print('GPU runtime: ' + stage + ' -> ' + str(log), flush=True)
        with log.open('w') as output:
            result = subprocess.run([str(v) for v in argv], cwd=overlay, env=env,
                                    stdout=output, stderr=subprocess.STDOUT)
        record['stages'].append(dict(stage=stage, command=[str(v) for v in argv],
                                     log=str(log), exit=result.returncode))
        (out / 'build.json').write_text(json.dumps(record, indent=2) + '\n')
        if result.returncode:
            raise SystemExit('GPU runtime ' + stage + ' failed; inspect ' + str(log))

    common = ['--guest-build', '22H374']
    run('turnip', [sys.executable, overlay / 'scripts/build-diting-turnip.py', *common,
                   '--mesa', mesa, '--abi-directory', abi,
                   '--tools-directory', args.tools_directory.resolve(),
                   '--venv', args.venv.resolve(), '--output-dir', out / 'turnip'])
    run('bridge', [sys.executable, overlay / 'scripts/build-diting-metal-bridge.py', *common,
                   '--translator', translator, '--output-dir', out / 'bridge'])
    plugin = [sys.executable, overlay / 'scripts/build-diting-metal-plugin.py', *common,
              '--bridge-directory', out / 'bridge', '--turnip-library',
              out / 'turnip/src/freedreno/vulkan/libvulkan_freedreno.dylib',
              '--output-dir', out / 'plugin', '--clients', '*']
    if args.prime_list:
        plugin += ['--prime-list', args.prime_list.resolve()]
    run('plugin', plugin)
    bridge = json.loads((out / 'bridge/build-report.json').read_text())
    turnip = json.loads((out / 'turnip/build-report.json').read_text())
    if '-O3' not in bridge['commands'][0] or '--release' not in bridge['commands'][1]:
        raise SystemExit('The actual frontend/bridge commands did not select optimized builds')
    for option in ('-Dshader-cache=enabled', '-Doptimization=3', '-Dbuildtype=release'):
        if option not in turnip['commands'][0]:
            raise SystemExit('The actual Turnip configure command lacks ' + option)
    if 'tu_darwin_sync_host_batch' not in (mesa / 'src/freedreno/vulkan/tu_knl_darwin.cc').read_text():
        raise SystemExit('The matched Turnip host-import batch backend is missing')
    path = out / 'plugin/build-report.json'
    report = json.loads(path.read_text())
    report.update(runtime_format='diting-native-gpu-runtime-v1', native_abi=1,
                  device_extent=600, mesa=MESA, translator=TRANSLATOR,
                  vulkan_headers=str(mesa / 'include'),
                  performance=dict(frontend_optimization=3, turnip_optimization=3,
                                   shader_cache=True, host_import_batch=64),
                  component_reports=dict(bridge=bridge, turnip=turnip),
                  device_performance='UNTESTED')
    path.write_text(json.dumps(report, indent=2) + '\n')
    record.update(build_complete=True, runtime=str(out / 'plugin'),
                  finished_utc=datetime.now(timezone.utc).isoformat())
    (out / 'build.json').write_text(json.dumps(record, indent=2) + '\n')
    print(out / 'plugin')


if __name__ == '__main__':
    main()
