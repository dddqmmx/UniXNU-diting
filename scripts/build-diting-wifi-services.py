#!/usr/bin/env python3
"""Build native Wi-Fi services from the captured installed 22H374 job cache."""
import argparse
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys
from diting_wifi_services import pack_runtime, sign_bootstrap

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, required=True,
                        help='bounded job cache captured from the installed OS')
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--diagnostics', action='store_true', help='capture native wifid logs in its own file')
    args = parser.parse_args()
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    if not args.sdk.is_dir(): raise SystemExit('Existing iPhoneOS SDK required')
    if args.cache.stat().st_size > 2 << 20: raise SystemExit('Installed cache exceeds its fixed bound')
    cache = args.cache.read_bytes()
    report = dict(runtime_format='diting-wifi-services-v1', guest='22H374',
        board='SM8475-Diting', build_complete=False, commands=[],
        source=str(ROOT/'userland/wifi-bootstrap.c'), cache_source=str(args.cache.resolve()),
        image_verification='skipped', gpu_files_changed=False, ui_files_changed=False,
        data_or_systemos_replaced=False)
    def save(): (output/'build-report.json').write_text(json.dumps(report,indent=2)+'\n')
    def run(argv, name):
        result = subprocess.run(argv, capture_output=True, text=True, timeout=60)
        (output/(name+'.log')).write_text(result.stdout+result.stderr)
        report['commands'].append(dict(argv=argv,exit=result.returncode)); save()
        if result.returncode: raise RuntimeError(result.stderr or 'Wi-Fi service compilation failed')
    save()
    try:
        executable = output/'wifi-bootstrap'
        command = ['clang','-target','arm64-apple-ios18.0','-isysroot',str(args.sdk.resolve()),
            '-Os','-Wall','-Wextra','-Werror','-Wl,-no_fixup_chains',
            '-framework','CoreFoundation','-framework','IOKit',str(ROOT/'userland/wifi-bootstrap.c'),
            '-o',str(executable)]
        if platform.system() == 'Linux': command[1:1] = ['-fuse-ld=lld','-Wl,-adhoc_codesign']
        run(command,'bootstrap-build')
        executable.write_bytes(sign_bootstrap(executable.read_bytes()))
        archive_bytes, metadata = pack_runtime(executable.read_bytes(),cache,diagnostics=args.diagnostics)
        packed = output/'wifi-runtime.pack'; packed.write_bytes(archive_bytes)
        assembly = output/'DitingWifiRuntime.s'
        assembly.write_text('.section __TEXT,__const\n.p2align 3\n.globl _DitingWifiRuntimeData\n'
            '_DitingWifiRuntimeData:\n.incbin '+json.dumps(str(packed))+'\n'
            '.globl _DitingWifiRuntimeEnd\n_DitingWifiRuntimeEnd:\n')
        obj = output/'DitingWifiRuntime.o'
        run(['clang','-target','arm64-apple-ios18.0','-c',str(assembly),'-o',str(obj)],'archive-object')
        archive = output/'libDitingWifiRuntime.a'
        command = ['llvm-ar','rcs',str(archive),str(obj)] if shutil.which('llvm-ar') else \
            ['xcrun','libtool','-static','-o',str(archive),str(obj)]
        run(command,'archive-build')
        report.update(metadata,archive=str(archive),build_complete=True,
            signature_page_bytes=4096, native_manager_entitlements=True)
        save(); print(json.dumps(report,indent=2))
    except BaseException as error:
        report['error'] = str(error); save(); raise


if __name__ == '__main__': main()
