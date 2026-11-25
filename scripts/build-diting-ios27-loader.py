#!/usr/bin/env python3
"""Build the pinned DarwinBoot in a private EDK workspace and qualify its parser."""
import argparse
import datetime
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tomllib

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--tree', type=Path, required=True)
    parser.add_argument('--edk2', type=Path, required=True)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--llvm', type=Path, default=Path('/usr/bin'))
    parser.add_argument('--reference-fileset', type=Path)
    parser.add_argument('--jobs', type=int, default=8)
    args = parser.parse_args()
    out = args.out.resolve()
    if out == ROOT or ROOT in out.parents or args.jobs < 1:
        parser.error('Use a private output outside the device repository and positive --jobs')
    source, tree, edk2 = args.source.resolve(), args.tree.resolve(), args.edk2.resolve()
    inputs = tomllib.loads((ROOT / 'configs/diting-ios27-inputs.toml').read_text())
    entry = next(c for c in inputs['component'] if c['name'] == 'DarwinBoot')
    pin = entry['revision']
    actual = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    dirty = subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain'], text=True)
    if actual != pin:
        parser.error('DarwinBoot must match the selected component revision')
    if dirty:
        receipt_path = source.parent / (source.name + '.json')
        diff = subprocess.check_output(['git', '-C', str(source), 'diff', 'HEAD'], text=True)
        saved = json.loads(receipt_path.read_text()) if receipt_path.is_file() else {}
        if not entry.get('patches') or saved.get('revision') != pin or saved.get('diff') != diff:
            parser.error('DarwinBoot has unrecorded local edits outside component_overrides')
    tools = edk2 / 'BaseTools'
    for required in (edk2 / 'MdePkg/MdePkg.dec', args.package / 'UniXNUPkg/UniXNUPkg.dec',
                     tools / 'Source/C/bin/GenFw'):
        if not required.is_file():
            parser.error('Missing EDK input: ' + str(required))
    for name in ('clang', 'lld-link', 'llvm-lib', 'llvm-rc'):
        if not (args.llvm / name).is_file():
            parser.error('Missing CLANGPDB tool: ' + name)
    conf = out / 'Conf'
    conf.mkdir(parents=True, exist_ok=True)
    for name in ('target', 'tools_def', 'build_rule'):
        path = conf / (name + '.txt')
        if not path.exists():
            shutil.copyfile(tools / 'Conf' / (name + '.template'), path)
    env = dict(os.environ, WORKSPACE=str(out), CONF_PATH=str(conf), EDK_TOOLS_PATH=str(tools),
               PACKAGES_PATH=os.pathsep.join(map(str, (edk2, args.package.resolve(), source))),
               PYTHON_COMMAND=sys.executable, PYTHONPATH=str(tools / 'Source/Python'),
               CLANG_BIN=str(args.llvm.resolve()) + '/', SOURCE_DATE_EPOCH='1792454400')
    env['PATH'] = os.pathsep.join(map(str, (Path(sys.executable).parent,
        tools / 'BinWrappers/PosixLike', tools / 'Source/C/bin', args.llvm, '/usr/bin', '/bin')))
    command = [str(tools / 'BinWrappers/PosixLike/build'), '-p',
               str(source / 'DarwinBootPkg/DarwinBoot.dsc'), '-a', 'AARCH64',
               '-b', 'RELEASE', '-t', 'CLANGPDB', '-n', str(args.jobs)]
    state = {'profile': 'ios-27.2.1-24B156', 'component': 'DarwinBoot',
             'source_revision': actual, 'source': str(source), 'command': command,
             'jobs': args.jobs, 'stage': 'build',
             'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
             'saved_firmware_written': False, 'device_written': False,
             'hardware_qualified': False, 'image_verification': 'skipped by user request'}
    receipt = out / 'BUILD_RESULT.json'
    log = out / 'build.log'
    try:
        receipt.write_text(json.dumps(state, indent=2) + '\n')
        with log.open('w') as output:
            result = subprocess.run(command, cwd=out, env=env, stdout=output, stderr=subprocess.STDOUT)
        if result.returncode:
            raise RuntimeError('DarwinBoot build failed:\n' + '\n'.join(log.read_text().splitlines()[-30:]))
        binary = out / 'build/DarwinBoot/RELEASE_CLANGPDB/AARCH64/DarwinBoot.efi'
        if not binary.is_file():
            raise RuntimeError('EDK build returned without the selected DarwinBoot output')
        artifact = out / 'artifact/DarwinBoot.efi'
        artifact.parent.mkdir(exist_ok=True)
        shutil.copyfile(binary, artifact)
        state.update(stage='parser', artifact=str(artifact), bytes=artifact.stat().st_size)
        receipt.write_text(json.dumps(state, indent=2) + '\n')
        kernel = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf'
        check = [sys.executable, str(ROOT / 'scripts/check-diting-ios27-loader.py'),
                 '--source', str(source), '--tree', str(tree), '--kernel', str(kernel),
                 '--out', str(out / 'checks')]
        if args.reference_fileset:
            check += ['--reference-fileset', str(args.reference_fileset.resolve())]
        subprocess.run(check, check=True)
        state.update(stage='complete', parser_qualified=True, boot_args_qualified=True)
    except Exception as error:
        state.update(stage='failed', error=str(error))
        raise
    finally:
        state['finished_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        receipt.write_text(json.dumps(state, indent=2) + '\n')
    print(json.dumps(state, indent=2))


if __name__ == '__main__':
    main()
