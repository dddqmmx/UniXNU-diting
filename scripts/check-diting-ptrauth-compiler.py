#!/usr/bin/env python3
"""Check actual clang code generation for authenticated-null arm64e fields."""
import argparse
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def body(ir, name):
    match = re.search(r'^define[^\n]*@' + re.escape(name) + r'\([^\n]*\)[^\n]*\{\n(.*?)^}', ir, re.M | re.S)
    if not match:
        raise RuntimeError('Missing compiler fixture function: ' + name)
    return match.group(1)


def reachable_body(ir, name):
    defined = set(re.findall(r'^define[^\n]*@([^ (]+)\(', ir, re.M))
    pending, visited, bodies = [name], set(), []
    while pending:
        name = pending.pop()
        if name in visited or name not in defined:
            continue
        visited.add(name)
        current = body(ir, name)
        bodies.append(current)
        pending += re.findall(r'\bcall[^\n]*@([^ (]+)\(', current)
    return '\n'.join(bodies)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--clang', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    target = ['-target', 'arm64e-apple-ios27.2', '-O0']
    output = args.out / 'ptrauth-null.ll'
    subprocess.run([str(args.clang), *target, '-S', '-emit-llvm', str(ROOT / 'tests/diting_ptrauth_null.c'),
                    '-o', str(output)], check=True)
    ir = output.read_text()
    clear, load = body(ir, 'signed_null_clear'), body(ir, 'signed_null_load')
    nullable_clear, nullable_load = body(ir, 'nullable_clear'), body(ir, 'nullable_load')
    if not re.search(r'call i64 @llvm\.ptrauth\.sign\(i64 0, i32 2,', clear):
        raise RuntimeError('Authenticated-null store must sign zero with the declared data key')
    if 'llvm.ptrauth.auth' not in load or 'br i1' in load:
        raise RuntimeError('Authenticated-null load must authenticate even a zero stored value')
    if 'llvm.ptrauth.sign' in nullable_clear or 'br i1' not in nullable_load:
        raise RuntimeError('The three-argument qualifier must retain its nullable semantics')
    to_nullable, to_signed = body(ir, 'signed_to_nullable'), body(ir, 'nullable_to_signed')
    if not (to_nullable.find('@llvm.ptrauth.auth') < to_nullable.find('br i1') < to_nullable.find('@llvm.ptrauth.sign')):
        raise RuntimeError('Signed-to-nullable conversion must authenticate before testing the decoded pointer')
    if 'phi ptr' not in to_signed or to_signed.rfind('@llvm.ptrauth.sign') < to_signed.rfind('phi ptr'):
        raise RuntimeError('Nullable-to-signed conversion must sign the merged value, including zero')
    for language in ('c', 'c++'):
        path = args.out / ('aggregate-' + language.replace('+', 'p') + '.ll')
        subprocess.run([str(args.clang), *target, '-x', language, '-S', '-emit-llvm',
                        str(ROOT / 'tests/diting_ptrauth_null.c'), '-o', str(path)], check=True)
        aggregate = path.read_text()
        initialize = body(aggregate, 'signed_null_initialize')
        replace = reachable_body(aggregate, 'signed_null_replace')
        if not re.search(r'@llvm\.ptrauth\.sign\(i64 0, i32 2,', initialize) or \
                initialize.find('@llvm.ptrauth.sign') > initialize.find('@inspect_signed_null'):
            raise RuntimeError(language + ': aggregate initializer exposed an unsigned null')
        if '@llvm.ptrauth.sign' not in replace or '@llvm.ptrauth.resign' not in replace or 'br i1' in replace:
            raise RuntimeError(language + ': aggregate copy must resign the address-discriminated field')
    global_source = args.out / 'static-null.c'
    global_source.write_text('struct value { int *__ptrauth(2,1,42,"authenticates-null-values") p; };\n'
                             'struct value explicit_null = {0};\nstruct value implicit_null;\n')
    global_ir = args.out / 'static-null.ll'
    subprocess.run([str(args.clang), *target, '-S', '-emit-llvm', str(global_source),
                    '-o', str(global_ir)], check=True)
    # Static initialization is a separate qualification boundary. Record what
    # the compiler actually emits instead of treating runtime checks as proof.
    static_qualified = all(re.search(r'^@' + name + r' = .*ptrauth \(ptr null,',
                                    global_ir.read_text(), re.M)
                           for name in ('explicit_null', 'implicit_null'))
    for label, qualifier in [('unknown-option', '__ptrauth(2,1,42,"unrecognised")'),
                             ('non-string-option', '__ptrauth(2,1,42,1)')]:
        source = args.out / (label + '.c')
        source.write_text('int *' + qualifier + ' value;\n')
        result = subprocess.run([str(args.clang), *target, '-fsyntax-only', str(source)], capture_output=True, text=True)
        (args.out / (label + '.log')).write_text(result.stdout + result.stderr)
        if result.returncode == 0:
            raise RuntimeError('Invalid qualifier option was accepted: ' + label)
    probe = args.out / 'xnu-types.c'
    probe.write_text('#if !__has_builtin(__builtin_xnu_type_summary)\n#error Missing XNU type builtins\n#endif\n')
    subprocess.run([str(args.clang), *target, '-fsyntax-only', str(probe)], check=True)
    report = {'authenticated_null_store': 'PASS', 'authenticated_null_load': 'PASS',
              'three_argument_nullable_control': 'PASS', 'invalid_options': 'REFUSED',
              'nullable_schema_conversions': 'PASS',
              'c_cpp_aggregate_initialization_and_copy': 'PASS',
              'static_authenticated_null_initialization': 'PASS' if static_qualified else 'UNQUALIFIED',
              'static_authenticated_null_qualified': static_qualified,
              'xnu_type_builtins': 'PASS', 'target': 'arm64e-apple-ios27.2',
              'hardware_pac_qualified': False}
    (args.out / 'compiler-check.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
