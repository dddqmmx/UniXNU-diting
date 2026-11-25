#!/usr/bin/env python3
"""Prepare and build the isolated Diting iOS 27.2.1 arm64e target."""
import argparse
import hashlib
import importlib.util
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import tomllib
from diting_ios27 import inspect_kernel_collection, require_complete_prelink

ROOT = Path(__file__).resolve().parents[1]
PROFILE = 'ios-27.2.1-24B156'


def run(argv, **kwargs):
    print('Running:', ' '.join(map(str, argv)), flush=True)
    return subprocess.run(list(map(str, argv)), check=True, **kwargs)


def revision(path):
    return subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()


def toml_value(value):
    if isinstance(value, dict):
        return '{ ' + ', '.join(json.dumps(key) + ' = ' + toml_value(item) for key, item in value.items()) + ' }'
    if isinstance(value, list):
        return '[' + ', '.join(map(toml_value, value)) + ']'
    return json.dumps(value, ensure_ascii=False)


def write_sources(path, entries):
    text = '# Target-local source pins; generated from the selected OS profile.\n'
    for entry in entries:
        text += '\n[[source]]\n' + ''.join(json.dumps(key) + ' = ' + toml_value(value) + '\n'
                                          for key, value in entry.items())
    path.write_text(text)


def install_profile(core, receipt):
    source = ROOT / 'os-profiles' / PROFILE
    target = core / 'profiles/os' / PROFILE
    previous = json.loads(receipt.read_text()) if receipt.exists() else {}
    installed = {}
    for item in source.rglob('*'):
        if not item.is_file():
            continue
        destination = target / item.relative_to(source)
        name = str(item.relative_to(source))
        if destination.exists() and destination.read_bytes() != item.read_bytes():
            if previous.get(name) != destination.read_text():
                raise RuntimeError(f'Existing profile edits require review: {destination}')
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(item, destination)
        installed[name] = item.read_text()
    receipt.write_text(json.dumps(installed, indent=2) + '\n')


def install_tooling(core):
    directory = ROOT / 'patches/tooling'
    names = ['linux-profile-target.patch', 'kernel-extra-archive-dependencies.patch',
             'linux-kernel-linker.patch', 'fileset-reference-role.patch']
    patches = [directory / name for name in names if (directory / name).exists()]
    patches += [path for path in sorted(directory.glob('*.patch')) if path not in patches]
    files = sorted({match.group(1) for patch in patches
                    for match in re.finditer(r'^diff --git a/\S+ b/(\S+)$', patch.read_text(), re.M)})
    # Overlapping target patches cannot be checked independently in reverse.
    # Reproduce every known stage privately, then replace only files whose
    # complete contents still match one of those stages. Other edits stay put.
    with tempfile.TemporaryDirectory(prefix='diting-ios27-tooling-') as temporary:
        work = Path(temporary)
        allowed = {}
        for name in files:
            original = subprocess.run(['git', '-C', str(core), 'show', 'HEAD:' + name], capture_output=True)
            data = original.stdout if original.returncode == 0 else None
            allowed[name] = [data]
            if data is not None:
                path = work / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
        for patch in patches:
            run(['git', 'apply', '--check', patch], cwd=work)
            run(['git', 'apply', patch], cwd=work)
            for name in files:
                path = work / name
                allowed[name].append(path.read_bytes() if path.exists() else None)
        for name in files:
            destination = core / name
            data = destination.read_bytes() if destination.exists() else None
            if data not in allowed[name]:
                raise RuntimeError('Independent tooling edits retained: ' + str(destination))
        for name in files:
            source, destination = work / name, core / name
            if source.exists() and (not destination.exists() or source.read_bytes() != destination.read_bytes()):
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, destination)
    return patches


def checkout(entry, destination, seed=None):
    commit = entry.get('commit') or entry['revision']
    repository = entry.get('git') or entry['repository']
    ready = destination.parent / (destination.name + '.checkout-ready')
    if not destination.exists():
        if seed is not None and (seed / '.git').exists():
            run(['git', 'clone', '--quiet', '--shared', '--no-checkout', seed, destination])
            run(['git', '-C', destination, 'remote', 'set-url', 'origin', repository])
        else:
            run(['git', 'clone', '--quiet', '--no-checkout', '--depth', '1', repository, destination])
    legacy = destination / '.unixnu-checkout-ready'
    if legacy.is_file() and legacy.read_bytes() == b'':
        legacy.unlink()
        ready.touch()
    changes = subprocess.check_output(['git', '-C', str(destination), 'status', '--porcelain',
                                       '--untracked-files=no'], text=True)
    head = subprocess.run(['git', '-C', str(destination), 'rev-parse', '--verify', 'HEAD'],
                          capture_output=True, text=True)
    # A fresh --no-checkout clone has staged deletions, which are not independent edits.
    if changes and ready.is_file():
        raise RuntimeError(f'Independent source edits retained: {destination}')
    found = subprocess.run(['git', '-C', str(destination), 'cat-file', '-e', commit + '^{commit}'],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if found.returncode:
        run(['git', '-C', destination, 'fetch', '--quiet', '--depth', '1', 'origin', commit])
    if head.returncode or head.stdout.strip() != commit or not ready.exists():
        run(['git', '-C', destination, 'checkout', '--quiet', '--detach', commit])
    if revision(destination) != commit:
        raise RuntimeError(f'Unexpected source revision: {destination}')
    ready.touch()
    return destination


def component_overrides(entry, clean, out):
    """Apply target patches in a separate clone with an exact edit receipt."""
    paths = [ROOT / name for name in entry.get('patches', [])]
    if not paths:
        return clean
    parent = out / 'patched-components'
    parent.mkdir(exist_ok=True)
    destination = parent / entry['name']
    receipt = parent / (entry['name'] + '.json')
    if not destination.exists():
        run(['git', 'clone', '--quiet', '--shared', '--no-checkout', clean, destination])
        run(['git', '-C', destination, 'checkout', '--quiet', '--detach', entry['revision']])
    if revision(destination) != entry['revision']:
        raise RuntimeError('Patched component revision differs: ' + str(destination))
    diff = subprocess.check_output(['git', '-C', str(destination), 'diff', 'HEAD'], text=True)
    if diff:
        saved = json.loads(receipt.read_text()) if receipt.exists() else {}
        if saved.get('diff') != diff:
            raise RuntimeError('Independent component edits retained: ' + str(destination))
        index = ['--index'] if saved.get('indexed') else []
        run(['git', '-C', destination, 'apply', '--reverse', *index, '--check', '-'], input=diff.encode())
        run(['git', '-C', destination, 'apply', '--reverse', *index, '-'], input=diff.encode())
    run(['git', '-C', destination, 'update-index', '--refresh'])
    for patch in paths:
        run(['git', '-C', destination, 'apply', '--index', '--check', patch])
        run(['git', '-C', destination, 'apply', '--index', patch])
    diff = subprocess.check_output(['git', '-C', str(destination), 'diff', 'HEAD'], text=True)
    receipt.write_text(json.dumps({'revision': entry['revision'], 'indexed': True,
                                   'patches': [str(p) for p in paths],
                                   'diff': diff}, indent=2) + '\n')
    return destination


def prepare(options, inputs):
    core, out = options.unixnu.resolve(), options.out.resolve()
    if core == ROOT or out == ROOT or ROOT in out.parents:
        raise RuntimeError('Use a separate UniXNU checkout and an output directory outside the device repository')
    if revision(core) != inputs['upstream']['revision']:
        raise RuntimeError('The UniXNU checkout does not match configs/diting-ios27-inputs.toml')
    marker = out / 'diting-ios27-state.json'
    if marker.exists() and json.loads(marker.read_text()).get('profile') != PROFILE:
        raise RuntimeError('The output directory belongs to another target')
    out.mkdir(parents=True, exist_ok=True)
    tooling = install_tooling(core)
    install_profile(core, out / 'installed-profile.json')
    device = out / 'device'
    ignored = shutil.ignore_patterns('.git', '.cache', 'refer', 'reference', 'mac-build', 'out',
                                    'build', 'apple', 'local.toml', 'unixnu.local.toml', '__pycache__',
                                    '*.pyc', '*.ipsw', '*.img', '*.dmg', '*.cdr', '*.a', '*.o', '*.dylib')
    shutil.copytree(ROOT, device, symlinks=True, dirs_exist_ok=True, ignore=ignored)
    sources = {}
    for filename in (core / 'feeds.toml', ROOT / 'feeds.toml'):
        for entry in tomllib.loads(filename.read_text()).get('source', []):
            sources[(entry['kind'], entry['name'])] = entry
    profile = tomllib.loads((ROOT / 'os-profiles' / PROFILE / 'profile.toml').read_text())
    for entry in profile['source']:
        sources[(entry['kind'], entry['name'])] = entry
    xnu = sources[('refer', 'xnu')]
    for field, key in (('commit', 'xnu_revision'), ('version', 'xnu_version'), ('darwin', 'darwin'),
                       ('driverkit_tag', 'driverkit_tag'), ('trustcache_tag', 'trustcache_tag')):
        xnu[field] = profile['os'][key]
    xnu['ref'] = 'xnu-' + profile['os']['xnu_version']
    write_sources(device / 'feeds.toml', sources.values())
    overrides = [('soc', 'sm8475', device / 'migration/format2/soc-seed')]
    source_dir = out / 'sources'
    source_dir.mkdir(exist_ok=True)
    for entry in sources.values():
        if entry['kind'] != 'refer' or entry['name'] == 'edk2':
            continue
        name = entry['name']
        seed = None
        if options.source_cache:
            candidates = [options.source_cache / name, options.source_cache / entry.get('ref', name)]
            candidates += sorted(options.source_cache.glob(name + '-*'))
            seed = next((path for path in candidates if (path / '.git').exists()), None)
        path = checkout(entry, source_dir / name, seed)
        overrides.append(('refer', name, path))
    for entry in inputs['component']:
        seed = options.component_cache / entry['name'] if options.component_cache else None
        path = checkout(entry, source_dir / entry['name'], seed)
        path = component_overrides(entry, path, out)
        overrides.append((entry['kind'], entry['name'], path))
    common = ['-D', str(device), '--os', PROFILE, '--out', str(out / 'output'), '-j', str(options.jobs)]
    for kind, name, path in overrides:
        common += ['--local-module', f'{kind}:{name}={path}']
    run([sys.executable, core / 'unixnu', 'defconfig', 'diting_ios27_defconfig', *common], cwd=core)
    sys.path.insert(0, str(core / 'scripts'))
    from unixnu.cli import parser
    from unixnu.config import Context
    from unixnu import feeds, tree, devicetree, patches
    ctx = Context(parser().parse_args(['status', *common]))
    modules = feeds.resolve(ctx)
    # CLI paths retain their local origin, and must also retain the selected
    # source metadata used by archive builders and header revision checks.
    for key, module in modules.items():
        if key in sources and module.path is not None and key[0] == 'refer':
            module.source = sources[key]
            module.commit = revision(module.path)
    tree.assemble(ctx, modules)
    hardware = devicetree.compile_hardware(ctx)
    order = patches.plan(ctx, modules)
    state = {'profile': PROFILE, 'version': profile['ipsw']['version'], 'build': profile['ipsw']['build'],
             'device': 'diting', 'soc': 'SM8475', 'arch': 'arm64e', 'jobs': options.jobs,
             'source_snapshot': str(device), 'build_tree': str(ctx.tree), 'hardware_dtb': str(hardware),
             'source_kernel': profile['os']['xnu_version'], 'source_darwin': profile['os']['darwin'],
             'restore_kernel': inputs['target']['kernel_xnu'],
             'restore_darwin': inputs['target']['kernel_darwin'],
             'core_revision': inputs['upstream']['revision'],
             'core_patches': [str(path) for path in tooling],
             'source_revisions': {name: revision(path) for kind, name, path in overrides if kind != 'soc'},
             'component_patches': {entry['name']: entry['patches'] for entry in inputs['component']
                                   if entry.get('patches')},
             'patches': [str(path) for path in order], 'prepared': True, 'native_kernel_built': False,
             'kernel_collection_linked': False, 'runtime_device_qualified': False,
             'pac_device_qualified': inputs['target']['native_pac_device_qualified'],
             'device_written': False, 'image_verification': 'skipped by user request',
             'updated_utc': datetime.now(timezone.utc).isoformat()}
    marker.write_text(json.dumps(state, indent=2) + '\n')
    (out / 'resolved-board.json').write_text(json.dumps(ctx.board, indent=2) + '\n')
    return ctx, modules, state, marker


def select_compile_environment(ctx):
    """Regenerate target-dependent compile records after a toolchain change."""
    profile = ctx.board['os']
    signature = json.dumps({'arch': profile['arch'], 'ios_version': profile['ios_version'],
                            'fp_type_discrimination': profile['arch'] == 'arm64e',
                            'toolchain': os.environ.get('UNIXNU_TOOLCHAIN'),
                            'compiler_patch': (ROOT / 'patches/toolchain/clang-ptrauth-auth-null.patch').read_text()},
                           sort_keys=True).encode()
    flag = '-DDITING_TARGET_ENV=0x' + hashlib.sha256(signature).hexdigest()[:8]
    text = ctx.config_path.read_text()
    pattern = r'^CONFIG_XNU_EXTRA_CFLAGS=(".*")$'
    match = re.search(pattern, text, re.M)
    if not match:
        raise RuntimeError('The iOS 27 defconfig must declare its full compiler flags')
    flags = json.loads(match.group(1))
    if flag not in flags.split():
        text = re.sub(pattern, lambda unused: 'CONFIG_XNU_EXTRA_CFLAGS=' + json.dumps(flags + ' ' + flag),
                      text, flags=re.M)
        ctx.config_path.write_text(text)
    objects = ctx.tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    record = objects / 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    arguments = json.loads(record.read_text().strip().rstrip(','))['arguments'] if record.exists() else []
    triple = f'{profile["arch"]}-apple-ios{profile["ios_version"]}'
    targets = [value.split('=', 1)[1] for value in arguments if value.startswith('--target=')]
    compatible = lambda args: ((flag in args or flag[2:] in args) and
                              any(value.startswith('--target=') for value in args) and
                              all(value.split('=', 1)[1] in (triple, triple + '.0')
                                  for value in args if value.startswith('--target=')))
    startup = objects / 'osfmk/DEVELOPMENT/startup.o.json'
    previous = ctx.work / 'previous-startup-record.json'
    if compatible(arguments) and not startup.exists() and previous.exists():
        saved = json.loads(previous.read_text().strip().rstrip(','))
        if compatible(saved['arguments']) and (startup.with_suffix('')).is_file():
            startup.write_bytes(previous.read_bytes())
    if not compatible(arguments):
        if startup.exists():
            previous.write_bytes(startup.read_bytes())
            startup.unlink()
        # A compile record is generated by compilation; invalidate its own
        # private object as well so make actually recreates the requested record.
        startup.with_suffix('').unlink(missing_ok=True)
    return {'triple': triple, 'compile_flag': flag}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--unixnu', type=Path, required=True, help='separate pinned UniXNU checkout')
    parser.add_argument('--out', type=Path, required=True, help='private output directory outside this repository')
    parser.add_argument('--source-cache', type=Path, help='optional existing public reference checkouts')
    parser.add_argument('--component-cache', type=Path, help='optional existing public component checkouts')
    parser.add_argument('--stage', choices=('prepare', 'kernel', 'prelink'), default='prelink')
    parser.add_argument('--kernelcache', type=Path, help='decompressed 24B156 MH_FILESET input for prelink')
    parser.add_argument('--jobs', type=int, default=8)
    options = parser.parse_args()
    if options.jobs < 1:
        parser.error('--jobs must be positive')
    if options.stage == 'prelink' and not options.kernelcache:
        parser.error('--kernelcache is required for the prelink stage')
    inputs = tomllib.loads((ROOT / 'configs/diting-ios27-inputs.toml').read_text())
    ctx, modules, state, marker = prepare(options, inputs)
    try:
        if options.stage != 'prepare':
            from unixnu import kernel
            state['compile_environment'] = select_compile_environment(ctx)
            kernel.build(ctx, modules)
            path = ctx.tree / ctx.board['kernel']['prelink']['kernel']
            data = path.read_bytes()
            if len(data) < 32 or struct.unpack_from('<II', data) != (0xfeedfacf, 0x100000c) or \
                    struct.unpack_from('<I', data, 8)[0] & 0xff != 2 or struct.unpack_from('<I', data, 12)[0] != 2:
                raise RuntimeError('The native kernel is not an arm64e MH_EXECUTE image')
            state.update(native_kernel_built=True, native_kernel=str(path), native_kernel_bytes=len(data))
            driver_builder_spec = importlib.util.spec_from_file_location(
                'diting_ios27_link_check', ROOT / 'scripts/build-diting-ios27-drivers.py')
            driver_builder = importlib.util.module_from_spec(driver_builder_spec)
            driver_builder_spec.loader.exec_module(driver_builder)
            driver_report = driver_builder.qualify_kernel_link(
                path, ctx.tree / 'build/diting-ios27-drivers/report.json',
                dict(os.environ, **(kernel.linux_env(ctx) if sys.platform == 'linux' else {})))
            state['drivers_linked'] = driver_report['kernel_link_qualified']
            marker.write_text(json.dumps(state, indent=2) + '\n')
            if options.stage == 'prelink':
                state['restore_kernel_info'] = inspect_kernel_collection(options.kernelcache.read_bytes(), inputs['target'])
                env = dict(os.environ, **kernel.xnu_env(ctx), **kernel.version_env(modules, ctx))
                if sys.platform == 'linux':
                    env.update(kernel.linux_env(ctx))
                    if not env.get('UNIXNU_LTO_LD') and env.get('UNIXNU_KERNEL_LD'):
                        env['UNIXNU_LTO_LD'] = env['UNIXNU_KERNEL_LD']
                run([sys.executable, ctx.tree / 'scripts/build-fileset-release-reference.py',
                     '--tree', ctx.tree, '--lto'], cwd=ctx.tree, env=env)
                ctx.args.kernelcache = str(options.kernelcache.resolve())
                linked = kernel.prelink(ctx)
                report_path = linked.with_suffix('.json')
                require_complete_prelink(json.loads(report_path.read_text()))
                state.update(kernel_collection_linked=True, kernel_collection=str(linked))
    except Exception as error:
        state.update(failed_stage=options.stage, error=str(error))
        raise
    finally:
        state['updated_utc'] = datetime.now(timezone.utc).isoformat()
        marker.write_text(json.dumps(state, indent=2) + '\n')
    print(json.dumps({key: value for key, value in state.items() if key != 'patches'}, indent=2))


if __name__ == '__main__':
    main()
