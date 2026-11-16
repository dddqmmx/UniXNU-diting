#!/usr/bin/env python3
"""Build a host-only owned AIR checker against the selected Metal frontend."""
import argparse
import json
import os
from pathlib import Path
import subprocess
ROOT = Path(__file__).resolve().parents[1]
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--translator', required=True, type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    args = parser.parse_args()
    source = args.translator.resolve() / 'crates/metal2vulkan-air'
    if not (source / 'Cargo.toml').is_file(): parser.error('selected metal2vulkan-air crate missing')
    out = args.output_dir.resolve()
    if out == ROOT or ROOT in out.parents: parser.error('generated checker belongs outside the checkout')
    out.mkdir(parents=True, exist_ok=True)
    manifest = out / 'Cargo.toml'
    manifest.write_text('[package]\nname="diting-owned-air-check"\nversion="0.1.0"\nedition="2021"\n[workspace]\n'
        '[[bin]]\nname="diting-owned-air-check"\npath=' + json.dumps(str(ROOT / 'tests/diting_gpu_air_check.rs')) +
        '\n[dependencies]\nmetal2vulkan-air={path=' + json.dumps(str(source)) + '}\n[profile.release]\nopt-level=2\ndebug=false\n')
    env = dict(os.environ, CARGO_BUILD_JOBS='1')
    env.pop('RUSTFLAGS', None)
    command = ['cargo','build','--release','--offline','--manifest-path',str(manifest),'--target-dir',str(out / 'target')]
    subprocess.run(command, env=env, check=True)
    binary = out / 'target/release/diting-owned-air-check'
    (out / 'build.json').write_text(json.dumps(dict(binary=str(binary), command=command, translator=str(args.translator.resolve())),indent=2)+'\n')
    print(binary)
if __name__ == '__main__': main()
