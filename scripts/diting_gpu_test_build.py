"""Compile fixed GPU diagnostics and locally generated, owned shader inputs."""
from pathlib import Path
import hashlib
import json
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def wrap_bitcode(raw):
    if not isinstance(raw,bytes) or not 8 <= len(raw) <= 1024*1024:
        raise ValueError('owned AIR bitcode extent outside bounds')
    if raw.startswith(b'BC\xc0\xde'):
        return struct.pack('<5I',0x0B17C0DE,0,20,len(raw),0xffffffff)+raw
    if len(raw)>=20 and raw.startswith(b'\xde\xc0\x17\x0b'):
        magic,version,offset,amount,cpu=struct.unpack_from('<5I',raw)
        if (magic==0x0B17C0DE and version==0 and offset==20 and 8<=amount<=len(raw)-offset and
                raw[offset:offset+4]==b'BC\xc0\xde' and len(raw)-offset-amount<16 and
                not any(raw[offset+amount:]) and cpu==0xffffffff):
            return raw
    raise ValueError('invalid locally assembled AIR wrapper/bitcode')


def metallib(bitcodes):
    """MIT metal2vulkan-devtools layout, using only owned bitcode.

    Reference: metal2vulkan 624bab3918de5b978e1101517abca2b4fa426565,
    crates/metal2vulkan-devtools/src/metallib.rs.
    HASH is a mandatory container tag, unrelated to image verification.
    """
    if not isinstance(bitcodes, dict) or set(bitcodes) != {'vertex','fragment','compute'}:
        raise ValueError('one locally assembled LLVM module per owned AIR entry required')
    def tag(name, value):
        return name + struct.pack('<H', len(value)) + value
    public = private = stream = b''
    entries = []
    for stage, kind in [('vertex', 0), ('fragment', 1), ('compute', 2)]:
        name = 'diting_test_' + stage
        wrapped = wrap_bitcode(bitcodes[stage])
        positions = len(public), len(private), len(stream)
        public += struct.pack('<I', 4) + b'ENDT'
        private += struct.pack('<I', 4) + b'ENDT'
        stream += wrapped
        fields = (tag(b'NAME', name.encode() + b'\0') + tag(b'TYPE', bytes([kind])) +
                  tag(b'HASH', hashlib.sha256(wrapped).digest()) + tag(b'MDSZ', struct.pack('<Q', len(wrapped))) +
                  tag(b'OFFT', struct.pack('<3Q', *positions)) + tag(b'VERS', bytes([2, 0, 8, 0, 3, 0, 1, 0])) + b'ENDT')
        entries.append(struct.pack('<I', len(fields) + 4) + fields)
    listing = b''.join(entries)
    public_at = 0x58 + 4 + len(listing) + 4
    private_at = public_at + len(public)
    stream_at = private_at + len(private)
    length = stream_at + len(stream)
    header = struct.pack('<4s3H2B2H9Q', b'MTLB', 0x8001, 2, 9, 0, 0x81, 18, 0,
                         length, 0x58, len(listing), public_at, len(public),
                         private_at, len(private), stream_at, len(stream))
    result = header + struct.pack('<I', 3) + listing + b'ENDT' + public + private + stream
    if len(header) != 0x58 or len(result) != length:
        raise ValueError('owned metallib extent mismatch')
    return result


def shaders(output_dir, env, run):
    out = Path(output_dir)
    out.mkdir(parents=True, exist_ok=True)
    glslang, validator, assembler = (shutil.which(x, path=env.get('PATH')) for x in ('glslangValidator', 'spirv-val', 'llvm-as'))
    if not all((glslang, validator, assembler)):
        raise ValueError('GPU test build requires glslangValidator, spirv-val and llvm-as')
    arrays = []
    for stage in ('vert', 'frag', 'comp'):
        target = out / ('pattern.' + stage + '.spv')
        run([glslang, '-V', '--target-env', 'vulkan1.1', '-o', target, ROOT / ('userland/gpu-test-pattern.' + stage)])
        run([validator, '--target-env', 'vulkan1.1', target])
        raw = target.read_bytes()
        if len(raw) % 4 or struct.unpack_from('<I', raw)[0] != 0x07230203:
            raise ValueError('generated SPIR-V extent or magic mismatch')
        words = struct.unpack('<' + 'I' * (len(raw) // 4), raw)
        arrays.append('static const uint32_t diting_test_' + stage + '[] = {' + ','.join(hex(x) for x in words) + '};\n')
    target = out / 'input.frag.spv'
    run([glslang, '-V', '--target-env', 'vulkan1.1', '-o', target, ROOT / 'userland/gpu-test-input.frag'])
    run([validator, '--target-env', 'vulkan1.1', target])
    raw = target.read_bytes()
    if len(raw) % 4 or struct.unpack_from('<I', raw)[0] != 0x07230203:
        raise ValueError('generated input-attachment SPIR-V extent or magic mismatch')
    words = struct.unpack('<' + 'I' * (len(raw) // 4), raw)
    arrays.append('static const uint32_t diting_test_input_frag[] = {' + ','.join(hex(x) for x in words) + '};\n')
    # A metallib function is one module with exactly one AIR entry. Sharing
    # the three-entry module would assemble but fail the real frontend ABI.
    source = (ROOT / 'userland/gpu-test-pattern.ll').read_text()
    bitcodes = {}
    entries = {'vertex':'air.vertex', 'fragment':'air.fragment', 'compute':'air.kernel'}
    for stage, entry in entries.items():
        text = '\n'.join(line for line in source.splitlines()
                         if not any(line.startswith('!' + other + ' =') for other in entries.values() if other != entry)) + '\n'
        owned = out / ('pattern.' + stage + '.ll')
        owned.write_text(text)
        air = out / ('pattern.' + stage + '.bc')
        run([assembler, owned, '-o', air])
        bitcodes[stage] = air.read_bytes()
    library = metallib(bitcodes)
    (out / 'pattern.metallib').write_bytes(library)
    arrays.append('static const uint8_t diting_test_metallib[] = {' + ','.join(str(x) for x in library) + '};\n')
    (out / 'GpuTestShaders.h').write_text('/* Generated locally from MIT source; do not commit. */\n#pragma once\n#include <stdint.h>\n' + ''.join(arrays))


def build(output_dir, sdk, vulkan_headers, env, codesign):
    out, headers = Path(output_dir).resolve(), Path(vulkan_headers).resolve()
    if not (headers / 'vulkan/vulkan.h').is_file():
        raise ValueError('--vulkan-headers must contain vulkan/vulkan.h from the selected Mesa')
    out.mkdir(parents=True, exist_ok=True)
    commands = []
    def run(command):
        command = [str(x) for x in command]
        subprocess.run(command, env=env, check=True)
        commands.append(command)
    shaders(out, env, run)
    checker = env.get('DITING_GPU_AIR_CHECKER')
    if not checker or not Path(checker).is_file():
        raise ValueError('set DITING_GPU_AIR_CHECKER to the host checker built against the selected translator')
    run([checker, out / 'pattern.metallib'])
    target = out / 'gpu-capture18'
    base = ['-target', 'arm64-apple-ios18.0', '-isysroot', sdk, '-O2', '-Wall', '-Wextra', '-Werror',
            '-Wno-deprecated-declarations', '-I' + str(out)]
    objects = []
    for name in ('gpu-capture18.c', 'gpu-test18d.c'):
        obj = out / (name + '.o')
        run(['xcrun', 'clang', *base, '-c', ROOT / 'userland' / name, '-o', obj])
        objects.append(obj)
    obj = out / 'gpu-test18.mm.o'
    run(['xcrun', 'clang++', *base, '-std=c++17', '-fobjc-arc', '-fblocks',
         '-Wno-missing-field-initializers', '-I' + str(headers), '-c', ROOT / 'userland/gpu-test18.mm', '-o', obj])
    objects.append(obj)
    run(['xcrun', 'clang++', *base, *objects, '-framework', 'Foundation', '-framework', 'CoreFoundation',
         '-framework', 'IOKit', '-framework', 'Metal', '-framework', 'IOSurface', '-lz', '-o', target])
    run([codesign, '--force', '--sign', '-', '--pagesize', '4096', '--entitlements',
         ROOT / 'userland/gpu-test18.entitlements', target])
    report = dict(schema=1, binary=str(target), commands=commands, vulkan_headers=str(headers),
                  shader_source='owned GLSL/SPIR-V and AIR/metallib generated locally', air_abi='PASS',
                  profiles=['qualification', 'performance', 'all', 'ui', 'keyboard'], policies=['auto', 'sysmem', 'gmem'],
                  timeout_ms=120000, result_bytes=1048576, target_fps=120, hardware_tested=False,
                  required_cases=json.loads((ROOT / 'configs/diting-gpu-tests.json').read_text())['cases'])
    (out / 'automation-build.json').write_text(json.dumps(report, indent=2) + '\n')
    return target, report
