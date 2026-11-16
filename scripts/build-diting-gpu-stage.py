#!/usr/bin/env python3
"""Embed the complete GPU runtime for an OS-only first installation at boot.

Uses the guest APFS driver before launchd; no host APFS edits or userdata
images. The kernel installer is bounded, preserves an existing foreign
runtime, and reports installation separately from real GPU performance.
"""
import argparse
import json
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import uuid
from diting_gpu_build_host import environment
from diting_gpu_test_build import build as build_tests

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'userland'))
import gpu_archive18


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--codesign', type=Path, required=True)
    parser.add_argument('--sdk', type=Path)
    parser.add_argument('--linker', type=Path)
    parser.add_argument('--vulkan-headers', type=Path, help='selected Mesa include directory for the native GPU test worker')
    args = parser.parse_args()
    runtime_report = gpu_archive18.gpu_runtime18.load_runtime(args.runtime)
    headers = args.vulkan_headers or runtime_report.get('vulkan_headers')
    if not headers:
        parser.error('provide --vulkan-headers from the matched A730 Mesa source')
    try:
        env = environment(args.sdk, args.linker, args.codesign)
    except ValueError as error:
        parser.error(str(error))
    out = args.output_dir.resolve()
    if out == ROOT or ROOT in out.parents:
        parser.error('GPU stage must stay outside the source checkout')
    out.mkdir(parents=True, exist_ok=True)
    capture = out / 'gpu-capture18'
    if (out / 'build-report.json').exists():
        parser.error('completed candidate retained; choose a new --output-dir')
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], env=env, text=True).strip()
    built, automation = build_tests(out / 'automation', sdk, headers, env, str(args.codesign.resolve()))
    shutil.copy2(built, capture)
    job = out / 'dev.unixnu.diting.gpu-capture18.plist'
    job.write_bytes(plistlib.dumps(dict(
        Label='dev.unixnu.diting.gpu-capture18', ProgramArguments=['/diting/gpu-capture18', '--daemon'],
        RunAtLoad=True, KeepAlive=True, ThrottleInterval=30, UserName='root',
        StandardOutPath='/private/var/logs/diting/gpu-capture18.log',
        StandardErrorPath='/private/var/logs/diting/gpu-capture18.log')))
    receipt = out / 'build-report.json'
    # Preserve a candidate's identity over reruns, including an interrupted build.
    identity_path = out / 'install-id'
    if not identity_path.exists():
        identity_path.write_text(str(uuid.uuid4()) + '\n')
    identity = str(uuid.UUID(identity_path.read_text().strip()))
    package = out / 'gpu-runtime.pack'
    package.write_bytes(gpu_archive18.pack(args.runtime, capture, job, identity))
    assembly = out / 'DitingGpuRuntimeData.S'
    # JSON quoting is also valid for this assembler's .incbin string literal.
    assembly.write_text('.section __DATA_CONST,__diting_gpu\n.p2align 3\n'
                        '.globl _DitingGpuRuntimeData\n_DitingGpuRuntimeData:\n'
                        '.incbin ' + json.dumps(str(package)) + '\n'
                        '.globl _DitingGpuRuntimeEnd\n_DitingGpuRuntimeEnd:\n')
    obj = out / 'DitingGpuRuntimeData.o'
    subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios18.0',
                    '-c', str(assembly), '-o', str(obj)], env=env, check=True)
    archive = out / 'libDitingGpuRuntime.a'
    archive.unlink(missing_ok=True)
    ar = shutil.which('llvm-ar') if sys.platform == 'linux' else 'ar'
    if not ar:
        parser.error('llvm-ar is required on Linux')
    subprocess.run([ar, 'rcs', str(archive), str(obj)], env=env, check=True)
    receipt.write_text(json.dumps(dict(
        runtime_format='diting-gpu-boot-stage-v1', guest='22H374', native_abi=1,
        chip_id='0x07030002', install_id=identity, archive=str(archive),
        runtime=str(args.runtime.resolve()), packed_bytes=package.stat().st_size,
        files=list(gpu_archive18.FILES), automation=automation,
        build_complete=True, installation_confirmed=False,
        hardware_acceleration_confirmed=False, device_performance='UNTESTED',
        scope='new GPU bundle, publication policy and trace helper on OS; no Data/SystemOS replacement',
        image_verification='skipped by request'), indent=2) + '\n')
    print(archive)


if __name__ == '__main__':
    main()
