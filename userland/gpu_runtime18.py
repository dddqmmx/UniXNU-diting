"""Stage the complete A730 ABI1/22H374 runtime in an offline mounted OS.

The caller owns the filesystem update transaction. This module never mounts
or writes a phone; existing Data shader caches and hardware gates keep their
owners.
"""
import json
import os
from pathlib import Path
import plistlib
import shutil

BUNDLE = 'DitingAdrenoMetal.bundle'
POLICY = ('DitingAdrenoMetal.clients', 'DitingAdrenoMetal.publish')
BINARIES = ('DitingAdrenoMetal', 'DitingMetalBridge.dylib', 'libvulkan_freedreno.dylib')


def load_runtime(runtime):
    runtime = Path(runtime).resolve()
    report = json.loads((runtime / 'build-report.json').read_text())
    if (report.get('runtime_format'), report.get('guest'), report.get('native_abi'),
            report.get('chip_id'), report.get('device_extent')) != (
            'diting-native-gpu-runtime-v1', '22H374', 1, '0x07030002', 600):
        raise RuntimeError('Build the complete A730 ABI1/22H374 GPU runtime first')
    performance = report.get('performance', {})
    if (performance.get('frontend_optimization'), performance.get('turnip_optimization'),
            performance.get('shader_cache'), performance.get('host_import_batch')) != (3, 3, True, 64):
        raise RuntimeError('GPU runtime lacks the matched optimized/cache/batch build')
    bundle = runtime / BUNDLE
    if bundle.is_symlink() or not bundle.is_dir():
        raise RuntimeError('GPU bundle must be an ordinary directory')
    required = [bundle / 'Info.plist', *(bundle / name for name in BINARIES),
                *(runtime / name for name in POLICY)]
    for path in [*required, *bundle.rglob('*')]:
        if path.is_symlink() or not (path.is_file() or path.is_dir()):
            raise RuntimeError('GPU runtime has missing or aliased files: ' + str(path))
        if path.is_file() and not path.stat().st_size:
            raise RuntimeError('GPU runtime has an empty file: ' + str(path))
    info = plistlib.loads((bundle / 'Info.plist').read_bytes())
    if (info.get('CFBundleIdentifier'), info.get('CFBundleExecutable'),
            info.get('NSPrincipalClass'), info.get('MinimumOSVersion')) != (
            'dev.unixnu.diting.Metal', 'DitingAdrenoMetal', 'DitingAdrenoMetalDevice', '18.0'):
        raise RuntimeError('GPU plugin belongs to another OS or implementation')
    if (runtime / POLICY[1]).read_text().strip() != '1':
        raise RuntimeError('GPU publication policy is missing')
    if (runtime / POLICY[0]).read_text().splitlines() != ['*']:
        raise RuntimeError('22H374 runtime must include native system and application clients')
    return report


def stage(root, runtime):
    root, runtime = Path(root).resolve(), Path(runtime).resolve()
    report = load_runtime(runtime)  # Refuse incomplete input before changing OS.
    version = plistlib.loads((root / 'System/Library/CoreServices/SystemVersion.plist').read_bytes())
    if version.get('ProductBuildVersion') != '22H374':
        raise RuntimeError('GPU runtime requires the matching 22H374 OS')
    extensions = root / 'System/Library/Extensions'
    target = extensions / BUNDLE
    temporary = extensions / (BUNDLE + '.diting-stage')
    previous = extensions / (BUNDLE + '.diting-previous')
    policy_temps = [extensions / (name + '.diting-stage') for name in POLICY]
    for path in [root / 'System', root / 'System/Library', extensions, target,
                 temporary, previous, *policy_temps, *(extensions / name for name in POLICY)]:
        if path.is_symlink():
            raise RuntimeError('GPU installation requires an ordinary OS path: ' + str(path))
    if any(path.exists() for path in [temporary, previous, *policy_temps]):
        raise RuntimeError('An earlier GPU staging transaction needs recovery')
    if target.exists():
        old = plistlib.loads((target / 'Info.plist').read_bytes())
        if old.get('CFBundleIdentifier') != 'dev.unixnu.diting.Metal':
            raise RuntimeError('Preserving an existing foreign GPU bundle')
    extensions.mkdir(parents=True, exist_ok=True)
    try:
        shutil.copytree(runtime / BUNDLE, temporary)
        for path in [temporary, *temporary.rglob('*')]:
            os.chown(path, 0, 0)
            os.chmod(path, 0o755 if path.is_dir() or path.name in BINARIES else 0o644)
        for name, staged in zip(POLICY, policy_temps):
            with staged.open('xb') as output:
                output.write((runtime / name).read_bytes())
                output.flush()
                os.fsync(output.fileno())
            os.chown(staged, 0, 0)
            os.chmod(staged, 0o644)
        if target.exists():
            target.rename(previous)
        try:
            temporary.rename(target)
        except BaseException:
            if previous.exists():
                previous.rename(target)
            raise
        # Publish discovery only after all three runtime dependencies exist.
        for name, staged in zip(POLICY, policy_temps):
            os.replace(staged, extensions / name)
        if previous.exists():
            shutil.rmtree(previous)
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)
        for staged in policy_temps:
            staged.unlink(missing_ok=True)
    return dict(build='22H374', native_abi=1, chip_id='0x07030002',
                path='/System/Library/Extensions/' + BUNDLE,
                files=[BUNDLE + '/' + name for name in BINARIES] + list(POLICY),
                performance=report['performance'], data_modified=False,
                hardware_acceleration_confirmed=False,
                activation='real CP/fence, guest ABI and Vulkan device readiness',
                image_verification='skipped by request')
