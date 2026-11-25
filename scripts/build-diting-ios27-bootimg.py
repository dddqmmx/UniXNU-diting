#!/usr/bin/env python3
"""Package an iOS 27 arm64e prelinked kernel with DarwinBoot into a temporary fastboot image."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import tomllib
import uuid
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from diting_memory import DEFAULT_MEMORY_MAP, load_memory_map
from diting_panel_contract import adt_panel_properties
from diting_platform_identity import platform_identity
import diting_paths


def adt_node(name, properties=(), children=()):
    properties = [('name', name.encode() + b'\0'), *properties]
    result = struct.pack('<II', len(properties), len(children))
    for key, data in properties:
        result += struct.pack('<32sI', key.encode(), len(data))
        result += data + bytes(-len(data) % 4)
    return result + b''.join(children)


def empty_nvram_proxy():
    """Encode the volatile CHRP bank consumed by IODTNVRAM."""
    def header(name, size):
        raw = bytearray(struct.pack('<BBH12s', 0x70, 0, size // 16, name))
        checksum = sum(raw)
        while checksum > 255:
            checksum = (checksum & 255) + (checksum >> 8)
        raw[1] = checksum
        return raw

    image = bytearray(8192)
    image[:16] = header(b'nvram', 32)
    image[32:48] = header(b'common', 16 + 2048)
    free_at = 32 + 16 + 2048
    image[free_at:free_at + 16] = header(b'free', len(image) - free_at)
    struct.pack_into('<I', image, 16, zlib.adler32(image[20:]))
    return [('nvram-bank-size', struct.pack('<I', len(image))),
            ('nvram-bank-count', struct.pack('<I', 1)),
            ('nvram-current-bank', struct.pack('<I', 0)),
            ('nvram-proxy-data', bytes(image))]


def load_fdt(hardware_dtb):
    spec = importlib.util.spec_from_file_location('diting_fdt', ROOT / 'scripts/prepare-diting-wifi.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.fdt_nodes(Path(hardware_dtb).read_bytes()), module.cells


def build_ios27_adt(hardware_dtb, profile, board, memory_map, dram_size, window_size, cpus,
                    unique_chip_id, root_uuid, development_debug, ufs_probe, trust_cache_bytes=0,
                    gpu_addon_bytes=0, live_dtb=None, platform_uuid=None):
    memory, dram, allocatable = load_memory_map(memory_map, hardware_dtb)
    if dram_size is None:
        dram_size = memory['physical_bytes']
    if dram_size != memory['physical_bytes']:
        raise ValueError('--dram-size differs from selected RAM profile')

    nodes, u32_cells = load_fdt(hardware_dtb)
    u32_val = lambda p, k: int.from_bytes(p[k], 'big')
    apss_base, apss_size = 0x17000000, 0x02000000
    gic = next(p for p in nodes.values() if p.get('compatible') == b'arm,gic-v3\0')
    gic_regs = u32_cells(gic, 'reg')
    gic_dist = (gic_regs[0] << 32) | gic_regs[1]
    gic_dist_size = (gic_regs[2] << 32) | gic_regs[3]
    gic_redist = (gic_regs[4] << 32) | gic_regs[5]
    gic_redist_size = (gic_regs[6] << 32) | gic_regs[7]
    gic_stride = u32_val(gic, 'redistributor-stride')
    timebase = u32_val(nodes['/cpus'], 'timebase-frequency')
    cpu_props = [p for n, p in nodes.items() if n.startswith('/cpus/cpu@') and
                 p.get('status', b'okay\0') in (b'okay\0', b'ok\0')]
    affinities = sorted(u32_val(p, 'reg') for p in cpu_props)
    if len(affinities) != 8 or affinities[0] != 0:
        raise ValueError('Expected 8 CPU nodes in hardware DTB')
    ufs = next(p for p in nodes.values() if b'qcom,sm8475-ufshc\0' in p.get('compatible', b''))
    ufs_hc = int.from_bytes(ufs['reg'][:8], 'big')
    ufs_hc_size = int.from_bytes(ufs['reg'][8:16], 'big')
    gcc_ufs_page = u32_val(ufs, 'diting,gcc-ufs-page')
    ufs_stream_id = u32_val(ufs, 'diting,smmu-stream-id')
    ufs_spi = u32_val(ufs, 'diting,ufs-spi')
    dram_base = dram[0][0]

    display = json.loads((ROOT / 'configs/diting-display-sm8475.json').read_text())
    backlight, timing = display['brightness'], display['timing']

    # Profile ADT overrides for iOS 27.2.1 (24B156 / V54AP / iPhone18,2)
    adt_cfg = profile.get('adt', {})
    chosen_str = adt_cfg.get('chosen', {}).get('string', {})
    chosen_u32 = adt_cfg.get('chosen', {}).get('u32', {})
    root_str = adt_cfg.get('root', {}).get('string', {})
    product_str = adt_cfg.get('product', {}).get('string', {})
    product_u32 = adt_cfg.get('product', {}).get('u32', {})
    defaults_u32 = adt_cfg.get('defaults', {}).get('u32', {})
    buttons_u32 = adt_cfg.get('buttons', {}).get('u32', {})

    chosen = [
        # xnu-12377 requires 256 bytes of random-seed (4 x 64).
        ('random-seed', os.urandom(256)),
        ('osenvironment', (chosen_str.get('osenvironment', 'main') + '\0').encode()),
        ('crypto-hash-method', (chosen_str.get('crypto-hash-method', 'sha2-384') + '\0').encode()),
        ('display-scale', struct.pack('<I', 3)),
        ('display-rotation', struct.pack('<I', 0)),
        ('diting-dsi-backlight', struct.pack('<Q6I',
            int(backlight['controller'], 0), backlight['register_bytes'],
            backlight['maximum'], backlight['minimum_nonzero'],
            int(backlight['bl_inverted_dbv']), backlight['channel'], backlight['version'])),
        ('diting-dpu-timing', struct.pack('<3Q4I',
            int(timing['mdp'], 0), int(timing['dispcc'], 0), int(timing['dsi0'], 0),
            timing['mdp_register_bytes'], timing['dispcc_register_bytes'],
            timing['dsi0_register_bytes'], timing['version'])),
        *adt_panel_properties(live_dtb),
        ('diting-audio-dma', bytes(16)),
        ('darwinboot-memory-ranges', bytes(32 + 64 * 16)),
        ('diting-dram-ranges', b''.join(struct.pack('<QQ', b, s) for b, s in dram)),
        ('diting-allocatable-ram', b''.join(struct.pack('<QQ', b, s) for b, s in allocatable)),
        ('boot-epoch', struct.pack('<Q', int(time.time()))),
        ('debug-enabled', struct.pack('<I', int(development_debug))),
        ('mix-n-match-prevention-status', struct.pack('<I', 0)),
        ('dram-base', struct.pack('<Q', dram_base)),
        ('dram-size', struct.pack('<Q', dram_size)),
        ('darwinboot-memory-size', struct.pack('<Q', window_size)),
        ('psci-method', b'smc\0'),
        *empty_nvram_proxy(),
    ]
    for key, val in chosen_u32.items():
        chosen.append((key, struct.pack('<I', int(val))))
    if platform_uuid is not None:
        chosen.append(('diting-platform-uuid', platform_identity(platform_uuid)))
    if unique_chip_id is not None:
        chip_id = int(profile['ipsw']['chip_id'])
        chosen.append(('unique-chip-id', struct.pack('<Q', unique_chip_id)))
        chosen.append(('chip-id', struct.pack('<I', chip_id)))
    if root_uuid:
        vol = str(uuid.UUID(root_uuid)).upper()
        chosen.append(('root-matching', (
            '<dict><key>IOProviderClass</key><string>IOMedia</string>'
            '<key>IOPropertyMatch</key><dict><key>UUID</key><string>' + vol +
            '</string></dict></dict>\0').encode()))

    memory_map_entries = []
    if trust_cache_bytes:
        memory_map_entries.append(('TrustCache', bytes(16)))
    if gpu_addon_bytes:
        memory_map_entries.append(('GpuAddon', bytes(16)))

    cpu_nodes = [adt_node(f'cpu{i}', [
        ('state', b'running\0' if i == 0 else b'waiting\0'),
        ('reg', struct.pack('<I', affinities[i])),
        ('device_type', b'cpu\0'),
        ('compatible', b'qcom,kryo\0'),
        ('timebase-frequency', struct.pack('<I', timebase)),
    ]) for i in range(cpus)]

    psci_node = adt_node('psci', [
        ('compatible', b'arm,psci-1.0\0arm,psci-0.2\0'),
        ('method', b'smc\0'),
    ])

    gic_node = adt_node('intc', [
        ('compatible', b'arm,gic-v3\0'),
        ('reg', struct.pack('<4Q', gic_dist - apss_base, gic_dist_size,
                            gic_redist - apss_base, gic_redist_size)),
        ('redistributor-stride', struct.pack('<Q', gic_stride)),
    ])

    userdata = board.get('userdata', {})
    ufs_props = [
        ('compatible', b'qcom,sm8475-ufshc\0qcom,sm8450-ufshc\0'),
        ('diting,ufs-hc-base', struct.pack('<Q', ufs_hc)),
        ('diting,ufs-hc-size', struct.pack('<Q', ufs_hc_size)),
        ('diting,gcc-ufs-page', struct.pack('<Q', gcc_ufs_page)),
        ('diting,smmu-stream-id', struct.pack('<I', ufs_stream_id)),
        ('diting,smmu-base', struct.pack('<Q', 0x15000000)),
        ('diting,smmu-size', struct.pack('<Q', 0x100000)),
        ('diting,smmu-context-banks', struct.pack('<I', 78)),
        ('diting,smmu-match-groups', struct.pack('<I', 120)),
        ('diting,ufs-spi', struct.pack('<I', ufs_spi)),
    ]
    if 'first_lba' in userdata and 'last_lba' in userdata:
        ufs_props += [
            ('unixnu,userdata-first-lba', struct.pack('<Q', userdata['first_lba'])),
            ('unixnu,userdata-last-lba', struct.pack('<Q', userdata['last_lba'])),
        ]
    ufs_node = adt_node('ufshc', ufs_props)

    buttons_node = adt_node('diting-buttons', [
        ('compatible', b'diting,pm8350-keys\0'),
        ('core-base', struct.pack('<Q', 0x0c400000)),
        ('cnfg-base', struct.pack('<Q', 0x0c42d000)),
        ('observer-base', struct.pack('<Q', 0x0c440000)),
        ('ee', struct.pack('<I', 0)),
        ('pon-ppid', struct.pack('<I', 0x013)),
        ('volume-up-ppid', struct.pack('<I', 0x18d)),
    ])

    wlan_node = adt_node('wlan', [
        ('compatible', b'qcom,qca6490-wlan\0'),
        ('device_type', b'wlan\0'),
    ])

    arm_io_children = [gic_node, buttons_node, wlan_node] + ([ufs_node] if ufs_probe else [])

    model = root_str.get('model', 'iPhone18,2')
    target_type = root_str.get('target-type', 'V54')
    compat = root_str.get('compatible', 'V54AP')
    compat_bytes = f'{compat}\0{model}\0qcom,sm8475\0'.encode()

    defaults_props = [('sleep-disabled', struct.pack('<I', 1))]
    for k, v in defaults_u32.items():
        defaults_props.append((k, struct.pack('<I', int(v))))

    buttons_props = [
        ('button-names', b'volup\0voldown\0hold\0'),
        ('opposed-power-vol-buttons', struct.pack('<I', 1)),
    ]
    for k, v in buttons_u32.items():
        buttons_props.append((k, struct.pack('<I', int(v))))

    product_props = []
    for k, v in product_str.items():
        product_props.append((k, (v + '\0').encode()))
    for k, v in product_u32.items():
        product_props.append((k, struct.pack('<I', int(v))))

    boot_nonce_hash = hashlib.sha384(os.urandom(32)).digest()
    tree = adt_node('device-tree', [
        ('model', (model + '\0').encode()),
        ('compatible', compat_bytes),
        ('target-type', (target_type + '\0').encode()),
        ('target-sub-type', (compat + '\0').encode()),
        ('diting,physical-model', b'Redmi K50 Ultra\0'),
        ('#address-cells', struct.pack('<I', 2)),
        ('#size-cells', struct.pack('<I', 2)),
    ], [
        adt_node('chosen', chosen, [
            adt_node('memory-map', memory_map_entries),
            adt_node('manifest-properties', [('BNCH', boot_nonce_hash)]),
        ]),
        adt_node('cpus', [('#address-cells', struct.pack('<I', 1))], cpu_nodes),
        psci_node,
        adt_node('arm-io', [
            ('device_type', b'arm-io\0'),
            ('#address-cells', struct.pack('<I', 2)),
            ('#size-cells', struct.pack('<I', 2)),
            ('ranges', struct.pack('<QQQ', 0, apss_base, apss_size)),
        ], arm_io_children),
        adt_node('defaults', defaults_props),
        adt_node('options'),
        adt_node('buttons', buttons_props),
        adt_node('product', product_props),
    ])
    return tree


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--loader', type=Path, required=True)
    parser.add_argument('--hardware-dtb', type=Path, required=True)
    parser.add_argument('--live-dtb', type=Path, default=None)
    parser.add_argument('--template', type=Path, default=diting_paths.boot_template())
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--memory-map', type=Path, default=DEFAULT_MEMORY_MAP)
    parser.add_argument('--dram-size', type=lambda v: int(v, 0), default=None)
    parser.add_argument('--window-size', type=lambda v: int(v, 0), default=0x100000000)
    parser.add_argument('--cpus', type=int, default=8)
    parser.add_argument('--fat-mib', type=int, default=112)
    parser.add_argument('--boot-timeout', type=int, default=1)
    parser.add_argument('--unique-chip-id', type=lambda v: int(v, 16), default=None)
    parser.add_argument('--platform-uuid', default=None)
    parser.add_argument('--trust-cache', type=Path, default=None)
    parser.add_argument('--gpu-addon', type=Path, default=None)
    parser.add_argument('--root-match-volume-uuid', default=None)
    parser.add_argument('--kernel-arg', action='append', default=[])
    parser.add_argument('--no-ufs', action='store_true')
    parser.add_argument('--skip-image-verification', action='store_true')
    args = parser.parse_args()

    profile = tomllib.loads((ROOT / 'os-profiles/ios-27.2.1-24B156/profile.toml').read_text())
    board = tomllib.loads((ROOT / 'board.toml').read_text())

    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    boot = out / 'diting-ios27-boot.img'
    payload = out / 'stage/EFI/UniXNU'
    shutil.rmtree(out / 'stage', ignore_errors=True)
    payload.mkdir(parents=True, exist_ok=True)

    shutil.copyfile(args.kernel, payload / 'kernel.macho')
    shutil.copyfile(args.loader, payload / 'DarwinBoot.efi')
    trust_bytes = 0
    if args.trust_cache:
        shutil.copyfile(args.trust_cache, payload / 'kernel.macho.trustcache')
        trust_bytes = args.trust_cache.stat().st_size
    gpu_addon_bytes = 0
    if args.gpu_addon:
        shutil.copyfile(args.gpu_addon, payload / 'kernel.macho.gpuaddon')
        gpu_addon_bytes = args.gpu_addon.stat().st_size

    override = board.get('os_overrides', {}).get('ios-27.2.1-24B156', {})
    base_args = list(override.get('payload', {}).get('kernel_args') or board['payload']['kernel_args'])
    extra = [item for value in args.kernel_arg for item in value.split()]
    extra_keys = {item.split('=', 1)[0] for item in extra}
    final_args = [a for a in base_args if a.split('=', 1)[0] not in extra_keys] + extra
    boot_args = ' '.join(final_args)
    if len(boot_args.encode('ascii')) > 1000:
        raise ValueError(f'Kernel args exceed 1000 bytes: {len(boot_args)}')

    live_dtb = args.live_dtb
    if live_dtb is None and not os.environ.get('DITING_LIVE_DTB'):
        default_live = Path('/home/dddqmmx/UniXNU/diting-work/build/services169/live.dtb')
        if default_live.is_file():
            live_dtb = default_live

    local_cfg = {}
    local_toml = ROOT / 'local.toml'
    if local_toml.is_file():
        local_cfg = tomllib.loads(local_toml.read_text()).get('diting_build', {})
    platform_uuid = args.platform_uuid or local_cfg.get('platform_uuid')

    adt = build_ios27_adt(
        args.hardware_dtb, profile, board, args.memory_map, args.dram_size,
        args.window_size, args.cpus, args.unique_chip_id,
        args.root_match_volume_uuid or profile['payload']['adt'].get('root_match_volume_uuid'),
        development_debug=True, ufs_probe=not args.no_ufs, trust_cache_bytes=trust_bytes,
        gpu_addon_bytes=gpu_addon_bytes, live_dtb=live_dtb, platform_uuid=platform_uuid,
    )
    (payload / 'kernel.macho.adt').write_bytes(adt)

    (payload / 'unixnu.toml').write_text(
        'version = 1\n[boot]\n'
        f'timeout = {args.boot_timeout}\n'
        'default = "diting-ios27"\n[kits]\nautoload = false\n'
        '[[driver]]\npath = "DarwinBoot.efi"\nconnect = false\n'
        '[[entry]]\nid = "diting-ios27"\ntitle = "Diting iOS 27.2.1"\n'
        'type = "xnu"\npath = "kernel.macho"\n'
        f'args = "{boot_args}"\n'
        '[[entry]]\nid = "tools"\ntitle = "Tools"\ntype = "action"\naction = "tools"\n'
    )

    fat = out / 'payload-fat.img'
    fat.unlink(missing_ok=True)
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1', TZ='UTC')
    subprocess.run(['mformat', '-C', '-T', str(args.fat_mib * 2048), '-v', 'DITINGIOS27', '-i', str(fat), '::'],
                   check=True, env=env)
    subprocess.run(['mcopy', '-i', str(fat), '-s', str(out / 'stage/EFI'), '::/'], check=True, env=env)
    raw = fat.read_bytes()
    header = bytearray(4096)
    struct.pack_into('<8sIIQII', header, 0, b'PONGFAT1', 1, 4096, len(raw), zlib.crc32(raw) & 0xffffffff, 0)
    ramdisk = bytes(header) + raw
    (out / 'payload.ramdisk').write_bytes(ramdisk)

    template = args.template.read_bytes()
    if template[:8] != b'ANDROID!' or struct.unpack_from('<I', template, 40)[0] != 4:
        raise ValueError('Expected Android boot v4 template')
    kernel_size, old_size = struct.unpack_from('<2I', template, 8)
    header_size = struct.unpack_from('<I', template, 20)[0]
    page = lambda s: (s + 4095) & ~4095
    ramdisk_at = 4096 + page(kernel_size)
    signature_at = ramdisk_at + page(old_size)
    image = bytearray(template[:ramdisk_at])
    struct.pack_into('<I', image, 12, len(ramdisk))
    image += ramdisk + bytes(page(len(ramdisk)) - len(ramdisk)) + template[signature_at:]
    boot.write_bytes(image)
    meta = {
        'boot_image': str(boot),
        'image_bytes': len(image),
        'kernel': str(args.kernel),
        'kernel_bytes': args.kernel.stat().st_size,
        'adt_bytes': len(adt),
        'fat_mib': args.fat_mib,
        'boot_args': boot_args,
        'image_verification': 'skipped by request' if args.skip_image_verification else 'checked',
    }
    (out / 'boot.json').write_text(json.dumps(meta, indent=2) + '\n')
    print(json.dumps(meta, indent=2))


if __name__ == '__main__':
    main()
