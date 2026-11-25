#!/usr/bin/env python3
"""Compile Diting SM8475 UFS + shared resources for iOS 27 arm64e source storage."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tomllib
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]

SOURCE_STORAGE_RUNTIME = r'''/* Generated: deferred SM8475 source-storage metaclass registration for iOS 27 arm64e. */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOCatalogue.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSNumber.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
void __GLOBAL__sub_I_IOBlockStorageDevice_cpp(void) __asm__("__GLOBAL__sub_I_IOBlockStorageDevice.cpp");
void __GLOBAL__sub_I_NativeUfs_cpp(void) __asm__("__GLOBAL__sub_I_NativeUfs.cpp");
void __GLOBAL__sub_I_DitingUfsTransport_cpp(void) __asm__("__GLOBAL__sub_I_DitingUfsTransport.cpp");
}

static const OSMetaClass *gDitingStorageParent;
static bool gDitingStorageAttempted;

extern "C" const OSMetaClass *native_source_storage_parent(void)
{
    if (!gDitingStorageParent) {
        panic("DitingSourceStorage27: metaclass initialized before IOStorageFamily");
    }
    return gDitingStorageParent;
}

extern "C" void native_source_storage_start(void)
{
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("native-source-storage", &enabled, sizeof(enabled)) || enabled != 1 || gDitingStorageAttempted) {
        return;
    }
    gDitingStorageAttempted = true;
    OSReturn status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOStorageFamily");
    if (status != kOSReturnSuccess) {
        panic("DitingSourceStorage27: IOStorageFamily load failed %x", status);
    }
    const OSSymbol *name = OSSymbol::withCString("IOBlockStorageDevice");
    gDitingStorageParent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    if (name) {
        name->release();
    }
    if (!gDitingStorageParent || gDitingStorageParent->getClassSize() != sizeof(IOBlockStorageDevice)) {
        panic("DitingSourceStorage27: IOBlockStorageDevice layout mismatch (%u vs %lu)",
              gDitingStorageParent ? gDitingStorageParent->getClassSize() : 0,
              sizeof(IOBlockStorageDevice));
    }
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {
        panic("DitingSourceStorage27: preModLoad failed");
    }
    __GLOBAL__sub_I_IOBlockStorageDevice_cpp();
    __GLOBAL__sub_I_NativeUfs_cpp();
    __GLOBAL__sub_I_DitingUfsTransport_cpp();
    status = OSMetaClass::postModLoad(handle);
    if (status != kOSReturnSuccess) {
        panic("DitingSourceStorage27: postModLoad failed %x", status);
    }
    // Match the real SM8475 ufshc nub after its classes are available. Class
    // registration alone does not instantiate NativeUfs or publish its media.
    OSDictionary *personality = OSDictionary::withCapacity(4);
    OSArray *drivers = OSArray::withCapacity(1);
    if (!personality || !drivers) panic("DitingSourceStorage27: matching allocation failed");
    const char *keys[] = {"IOClass", "IOProviderClass", "IONameMatch"};
    const char *values[] = {"NativeUfs", "IOPlatformDevice", "ufshc"};
    for (unsigned i = 0; i < 3; ++i) {
        OSString *value = OSString::withCString(values[i]);
        if (!value || !personality->setObject(keys[i], value))
            panic("DitingSourceStorage27: matching property failed");
        value->release();
    }
    OSNumber *score = OSNumber::withNumber(1000u, 32);
    if (!score || !personality->setObject("IOProbeScore", score) || !drivers->setObject(personality))
        panic("DitingSourceStorage27: matching score failed");
    score->release();
    const bool matched = gIOCatalogue && gIOCatalogue->addDrivers(drivers, true);
    drivers->release();
    personality->release();
    if (!matched) panic("DitingSourceStorage27: UFS matching request rejected");
    IOLog("DitingSourceStorage27: deferred SM8475 UFS classes registered\n");
}
'''

USB_CONSOLE_WRAPPER = r'''/* Generated: SM8475 DWC3 CDC-ACM console wrapper for iOS 27 arm64e bring-up. */
#include <stdint.h>
#include <stdbool.h>
#include <mach/vm_param.h>
#include <mach/kmod.h>
#include <machine/machine_routines.h>
#include <libkern/c++/OSMetaClass.h>
#include "DitingPlatformResources.h"
#include "../DitingBootCapture/DitingBootCapture.h"
#include "DitingUsbDevice.h"
extern "C" {
extern kmod_info_t g_kernel_kmod_info;
void sheng_adsp_probe(void);
void sheng_battery_start(void);
void diting_memory_bandwidth_start(void);
void diting_cpu_policy_start(void);
void diting_wifi_start_optional(void);
void __GLOBAL__sub_I_DitingBattery_cpp(void) __asm__("__GLOBAL__sub_I_DitingBattery.cpp");
void __GLOBAL__sub_I_DitingWifiNet_cpp(void) __asm__("__GLOBAL__sub_I_DitingWifiNet.cpp");
void __GLOBAL__sub_I_DitingBacklight_cpp(void) __asm__("__GLOBAL__sub_I_DitingBacklight.cpp");
void DitingBatteryStaticInit(void) { __GLOBAL__sub_I_DitingBattery_cpp(); }
void DitingWifiNetStaticInit(void) { __GLOBAL__sub_I_DitingWifiNet_cpp(); }
void DitingBacklightStaticInit(void) { __GLOBAL__sub_I_DitingBacklight_cpp(); }
volatile int32_t native_battery_stats[16];
}
extern "C" unsigned diting_usb_read_kernel(uint64_t address, void *out, unsigned bytes)
{
    if (!out || !bytes || bytes > 512 || address < VM_MIN_KERNEL_ADDRESS ||
        address > UINT64_MAX - bytes) return 0;
    return (unsigned)ml_nofault_copy((vm_offset_t)address, (vm_offset_t)out, bytes);
}
extern "C" void diting_usb_start(void)
{
    diting_rpmh_init();
    diting_memory_bandwidth_start();
    diting_cpu_policy_start();
    DitingBootCapture::start();
    DitingUsb::start();
    unsigned adspMode = 0;
    if (PE_parse_boot_argn("diting_adsp", &adspMode, sizeof(adspMode)) && adspMode == 2) {
        sheng_adsp_probe();
        void *h = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
        if (h) {
            DitingBatteryStaticInit();
            if (!OSMetaClass::postModLoad(h)) sheng_battery_start();
            else { native_battery_stats[0] = -11; IOLog("DitingBattery: metaclass postModLoad failed\n"); }
        } else {
            native_battery_stats[0] = -10;
            IOLog("DitingBattery: metaclass preModLoad failed\n");
        }
    }
    diting_wifi_start_optional();
}
'''


def load_helper(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def symbols(path, mode, env=None):
    output = subprocess.check_output(['xcrun', 'nm', mode, '-j', str(path)], text=True, env=env)
    return {name for name in output.splitlines() if name.startswith('_') and ' ' not in name}


def provider_exports(path, env=None):
    """Exclude static NULL aliases when finding actual provider implementations.

    The previous native kernel contains this archive's optional NULL aliases.
    Counting those as providers omits their definitions on the next build and
    leaves the same weak imports unresolved at the static kernel link.
    """
    output = subprocess.check_output(['xcrun', 'nm', '-gU', '-m', str(path)], text=True, env=env)
    exports = set()
    for line in output.splitlines():
        fields = line.split()
        if not fields or not fields[-1].startswith('_'):
            continue
        if ('(absolute)' in fields and int(fields[0], 16) == 0) or 'weak' in fields:
            continue
        exports.add(fields[-1])
    return exports


def optional_imports(path):
    output = subprocess.check_output(['xcrun', 'nm', '-u', '-m', str(path)], text=True)
    return {line.split()[-1] for line in output.splitlines()
            if '(undefined)' in line and 'weak' in line and line.split()[-1].startswith('_')}


def qualify_kernel_link(native, report_path, env=None):
    report = json.loads(report_path.read_text())
    if report.get('archive_built') is not True:
        raise RuntimeError('Diting driver archive did not complete')
    exported = provider_exports(native, env)
    missing = sorted(set(report['required_imports']) - exported)
    hooks = {'_native_source_storage_start', '_diting_usb_start',
             '_diting_ramoops_console_write', '_diting_ramoops_diagnostic_write'}
    missing += sorted(hooks - exported)
    report.update(kernel_link_qualified=not missing, pending_imports=missing,
                  linked_native_kernel=str(native), required_hooks=sorted(hooks))
    report_path.write_text(json.dumps(report, indent=2) + '\n')
    if missing:
        raise RuntimeError('Diting drivers are absent from the native link: ' + ', '.join(missing))
    return report


def find_external_driver(tree, device, name):
    for candidate in (
        tree / 'drivers' / name,
        device.parent / 'UniXNU/drivers' / name,
        Path('/home/dddqmmx/UniXNU/UniXNU/drivers') / name,
    ):
        if candidate.is_dir() and any(candidate.glob('*.cpp')):
            return candidate
    raise RuntimeError(f'External driver directory not found: {name}')


def emit_legacy_display_header(abi_path, out_header):
    abi = json.loads(abi_path.read_text())
    base = abi.get('base_slots', 168)
    slots = [(int(t, 16), int(d, 16)) for t, d in abi['slots']]
    d, r = abi['display'], abi['request']
    prototypes = {
        'isMainDisplay': 'bool isMainDisplay()',
        'getDotPitch': 'IOReturn getDotPitch(uint32_t *)',
        'supportedFrameInfo': 'IOReturn supportedFrameInfo(uint32_t,uint32_t *,char *,uint32_t *)=0',
        'getDisplayDimensions': 'void getDisplayDimensions(uint32_t *,uint32_t *)',
        'startHardware': 'bool startHardware()',
        'kickSwapQueue': 'void kickSwapQueue()',
        'completeCurrentSwap': 'void completeCurrentSwap()',
        'validateSwap': 'bool validateSwap(const OriginalLegacySwapRequest *)',
        'getRefreshAttributes': 'bool getRefreshAttributes(uint64_t *,bool)',
        'collectCompletionReports': 'IOReturn collectCompletionReports(uint32_t,void *,uint32_t *,uint32_t)=0',
        'applyStateRequest': 'int applyStateRequest(OriginalLegacyStateRequest *,bool)',
        'cancelSwapsAfterTimeout': 'bool cancelSwapsAfterTimeout()',
        'isDisplayPowered': 'bool isDisplayPowered()',
        'create_default_fb_surface': 'void create_default_fb_surface()',
    }
    by_slot = {slot: name for name, slot in abi['methods'].items()}
    pure_target = int(abi['pure_target'], 16) if 'pure_target' in abi else slots[214][0]
    pure = {i for i, (t, _) in enumerate(slots) if t == pure_target}
    renames = ''.join(f'#define {k} {name}\n' for k, (name, _) in abi['names'].items())
    leaves = ' '.join(f'LEAF({i})' for i in abi['leaves'])
    header = f'''#pragma once
#include <IOKit/IOService.h>
#include <libkern/libkern.h>
extern "C" OSObject *native_legacy_default_surface_create(OSObject *,uint32_t,uint32_t);
struct OriginalLegacySwapRequest;
struct OriginalLegacyStateRequest {{uint32_t skipRequested,requested,skipPowered,powered;uint8_t changed;uint8_t pad[11];}};
static constexpr unsigned kLegacyVsyncClients={d['vsync_clients']};
static constexpr unsigned kRequestRect0={r['rect0']},kRequestRect1={r['rect1']},kRequestMasks={r['masks']},kRequestOptions={r['options']},kRequestDims={r['dims']},kRequestSurfaces={r['surfaces']};
#define NATIVE_LEGACY_LEAVES(LEAF) {leaves}
#define NATIVE_LEGACY_DEFAULT_SURFACE 1
#define NATIVE_LEGACY_CANCEL_SWAPS 1
static constexpr unsigned kLegacyDefaultSurface={d['default_surface']};
{renames}class IOMobileFramebufferLegacy : public IOService {{
 OSDeclareAbstractStructors(IOMobileFramebufferLegacy)
 unsigned char originalPrivate[{abi['instance_size']}-sizeof(IOService)];
public:
 bool installDefaultSurface(uint32_t width,uint32_t height) {{
  auto self=reinterpret_cast<unsigned char *>(this);
  OSObject *root=nullptr,*surface=nullptr;
  memcpy(&surface,self+kLegacyDefaultSurface,sizeof(surface));
  if(surface)return true;
  memcpy(&root,self+{d['surface_root']},sizeof(root));
  if(!root)return false;
  surface=native_legacy_default_surface_create(root,width,height);
  if(!surface)return false;
  memcpy(self+kLegacyDefaultSurface,&surface,sizeof(surface));
  return true;
 }}
 void installBootDimensions(uint32_t width,uint32_t height) {{
  memcpy(reinterpret_cast<unsigned char *>(this)+{d['width']},&width,4);memcpy(reinterpret_cast<unsigned char *>(this)+{d['height']},&height,4);
 }}
 const OriginalLegacySwapRequest *pendingSwapRequest() const {{
  const OriginalLegacySwapRequest *request=nullptr;
  memcpy(&request,reinterpret_cast<const unsigned char *>(this)+{d['head']},sizeof(request));return request;
 }}
 bool activateOnlyPendingSwap(const OriginalLegacySwapRequest *request) {{
  auto self=reinterpret_cast<unsigned char *>(this);
  auto bytes=reinterpret_cast<const unsigned char *>(request);
  const void *current=nullptr,*head=nullptr,*tail=nullptr,*next=nullptr,*previous=nullptr;
  memcpy(&current,self+{d['current']},8);memcpy(&head,self+{d['head']},8);memcpy(&tail,self+{d['tail']},8);
  if(!request || current || head!=request)return false;
  memcpy(&next,bytes+{r['next']},8);memcpy(&previous,bytes+{r['prev']},8);
  if(previous!=self+{d['head']})return false;
  if(next) {{
   const void *link=self+{d['head']};
   memcpy(const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(next))+{r['prev']},&link,8);
  }} else if(tail!=bytes+{r['next']})return false;
  memcpy(self+{d['current']},&request,8);
  if(next) {{memcpy(self+{d['head']},&next,8);}}
  else {{const void *empty=nullptr,*emptyTail=self+{d['head']};memcpy(self+{d['head']},&empty,8);memcpy(self+{d['tail']},&emptyTail,8);}}
  return true;
 }}
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 IOReturn setProperties(OSObject *) override;
 void systemWillShutdown(IOOptionBits) override;
 IOService *probe(IOService *,SInt32 *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 IOWorkLoop *getWorkLoop() const override;
 IOReturn newUserClient(task_t,void *,UInt32,IOUserClient **) override;
 IOReturn setPowerState(unsigned long,IOService *) override;
'''
    for i in range(base, len(slots)):
        if i in by_slot:
            header += f' virtual {prototypes[by_slot[i]]};\n'
        elif i in pure:
            header += f' virtual __attribute__((noreturn)) void opaque{i}()=0;\n'
        else:
            header += f' virtual __attribute__((deprecated("Unclassified ABI"))) void opaque{i}();\n'
    header += f'}};\nstatic_assert(sizeof(IOMobileFramebufferLegacy)=={abi["instance_size"]},"Actual original instance extent");\n'
    out_header.write_text(header)
    return abi


def build_legacy_display_bindings(abi, obj_path, out_dir, triple):
    raw = obj_path.read_bytes()
    magic, cpu, _, _, count = struct.unpack_from('<5I', raw)
    if magic != 0xfeedfacf or cpu != 0x100000c:
        raise RuntimeError('Expected arm64 Mach-O object for NativeLegacyDisplay')
    commands, segments, offset = [], [], 32
    for _ in range(count):
        command, size = struct.unpack_from('<2I', raw, offset)
        commands.append((command, offset, size))
        if command == 0x19:
            nsects = struct.unpack_from('<I', raw, offset + 64)[0]
            sections = []
            for i in range(nsects):
                at = offset + 72 + 80 * i
                va, length, fileoff = struct.unpack_from('<QQI', raw, at + 32)
                sections.append({'va': va, 'size': length, 'fileoff': fileoff, 'command_offset': at})
            segments.append({'sections': sections})
        offset += size
    st = next(p for c, p, _ in commands if c == 2)
    sy, scount, strings, _ = struct.unpack_from('<4I', raw, st + 8)
    syms = []
    for i in range(scount):
        ix, kind, _, _, va = struct.unpack_from('<IBBHQ', raw, sy + i * 16)
        syms.append((raw[strings + ix:raw.index(b'\0', strings + ix)].decode(), va, kind))
    vtable = next(va for name, va, _ in syms if name == '__ZTV19NativeLegacyDisplay')
    entries = {}
    for segment in segments:
        for sec in segment['sections']:
            if sec['va'] <= vtable < sec['va'] + sec['size']:
                ro, rc = struct.unpack_from('<2I', raw, sec['command_offset'] + 56)
                for i in range(rc):
                    off, bits = struct.unpack_from('<2I', raw, ro + i * 8)
                    if bits >> 27 & 1 and bits >> 25 & 3 == 3 and bits >> 28 == 11:
                        word = struct.unpack_from('<Q', raw, sec['fileoff'] + off)[0]
                        entries[sec['va'] + off] = (syms[bits & 0xffffff][0], word)
    slots = [(int(t, 16), int(d, 16)) for t, d in abi['slots']]
    slide = int(abi['collection_slide'], 16)
    base = abi.get('base_slots', 168)
    pure_target = int(abi['pure_target'], 16) if 'pure_target' in abi else slots[214][0]
    pure = {i for i, (t, _) in enumerate(slots) if t == pure_target}
    compiled = [entries.get(vtable + 16 + i * 8) for i in range(len(slots))]
    if not all(compiled) or (vtable + 16 + len(slots) * 8) in entries:
        raise RuntimeError(f'NativeLegacyDisplay vtable size mismatch (expected {len(slots)})')
    bindings = {name: int(va, 16) + slide for name, va in abi['bindings'].items()}
    for i, (name, word) in enumerate(compiled):
        if not (word >> 63 and (word >> 48) & 1 and ((word >> 49) & 3) == 0 and ((word >> 32) & 0xffff) == slots[i][1]):
            raise RuntimeError(f'Slot {i} ({name}) diversity mismatch: got {((word >> 32) & 0xffff):#x}, want {slots[i][1]:#x}')
        if '25IOMobileFramebufferLegacy' in name:
            if i in pure:
                raise RuntimeError(f'Unresolved abstract slot {i}')
            bindings[name] = slots[i][0] + slide
    data_syms = {'__ZN25IOMobileFramebufferLegacy10gMetaClassE', '__ZTV25IOMobileFramebufferLegacy'}
    text, addresses = '.text\n.p2align 2\n', ''
    for name, va in sorted(bindings.items()):
        if name in data_syms:
            addresses += f'.globl {name}\n.set {name}, {va:#x}\n'
        else:
            addresses += f'.globl {name}.original\n.set {name}.original, {va:#x}\n'
            text += f'.globl {name}\n{name}:\n bti c\n b {name}.original\n'
    bind_s = out_dir / 'legacy-original-bindings.S'
    addr_s = out_dir / 'legacy-original-addresses.S'
    bind_s.write_text(text)
    addr_s.write_text(addresses)
    bind_o = out_dir / 'legacy-original-bindings.o'
    addr_o = out_dir / 'legacy-original-addresses.o'
    subprocess.run(['xcrun', 'clang', '-target', triple, '-c', str(bind_s), '-o', str(bind_o)], check=True)
    subprocess.run(['xcrun', 'clang', '-target', triple, '-c', str(addr_s), '-o', str(addr_o)], check=True)
    return [bind_o, addr_o]


def pac_discriminator(name):
    """clang ptrauth_string_discriminator: SipHash-2-4 modulo 0xffff + 1."""
    mask = (1 << 64) - 1
    rotl = lambda x, b: ((x << b) | (x >> (64 - b))) & mask
    key = bytes([0xb5, 0xd4, 0xc9, 0xeb, 0x79, 0x10, 0x4a, 0x79, 0x6f, 0xec, 0x8b, 0x1b, 0x42, 0x87, 0x81, 0xd4])
    k0, k1 = struct.unpack('<2Q', key)
    v = [0x736f6d6570736575 ^ k0, 0x646f72616e646f6d ^ k1, 0x6c7967656e657261 ^ k0, 0x7465646279746573 ^ k1]
    def rounds(n):
        for _ in range(n):
            v[0] = (v[0] + v[1]) & mask; v[1] = rotl(v[1], 13) ^ v[0]; v[0] = rotl(v[0], 32)
            v[2] = (v[2] + v[3]) & mask; v[3] = rotl(v[3], 16) ^ v[2]
            v[0] = (v[0] + v[3]) & mask; v[3] = rotl(v[3], 21) ^ v[0]
            v[2] = (v[2] + v[1]) & mask; v[1] = rotl(v[1], 17) ^ v[2]; v[2] = rotl(v[2], 32)
    data = name.encode()
    full = len(data) & ~7
    for i in range(0, full, 8):
        m = struct.unpack_from('<Q', data, i)[0]
        v[3] ^= m; rounds(2); v[0] ^= m
    b = (len(data) << 56) & mask
    for i, c in enumerate(data[full:]):
        b |= c << (8 * i)
    v[3] ^= b; rounds(2); v[0] ^= b; v[2] ^= 0xff; rounds(4)
    return (v[0] ^ v[1] ^ v[2] ^ v[3]) % 0xffff + 1


def build_hid_device_bindings(abi_path, obj_path, out_dir, triple):
    abi = json.loads(abi_path.read_text())
    raw = obj_path.read_bytes()
    magic, cpu, _, _, count = struct.unpack_from('<5I', raw)
    if magic != 0xfeedfacf or cpu != 0x100000c:
        raise RuntimeError('Expected arm64 Mach-O object for NativeHidInput')
    commands, segments, offset = [], [], 32
    for _ in range(count):
        command, size = struct.unpack_from('<2I', raw, offset)
        commands.append((command, offset, size))
        if command == 0x19:
            nsects = struct.unpack_from('<I', raw, offset + 64)[0]
            sections = []
            for i in range(nsects):
                at = offset + 72 + 80 * i
                va, length, fileoff = struct.unpack_from('<QQI', raw, at + 32)
                sections.append({'va': va, 'size': length, 'fileoff': fileoff, 'command_offset': at})
            segments.append({'sections': sections})
        offset += size
    st = next(p for c, p, _ in commands if c == 2)
    sy, scount, strings, _ = struct.unpack_from('<4I', raw, st + 8)
    syms = []
    for i in range(scount):
        ix, kind, _, _, va = struct.unpack_from('<IBBHQ', raw, sy + i * 16)
        syms.append((raw[strings + ix:raw.index(b'\0', strings + ix)].decode(), va, kind))
    vtable = next(va for name, va, _ in syms if name == '__ZTV14NativeHidInput')
    entries = {}
    for segment in segments:
        for sec in segment['sections']:
            if sec['va'] <= vtable < sec['va'] + sec['size']:
                ro, rc = struct.unpack_from('<2I', raw, sec['command_offset'] + 56)
                for i in range(rc):
                    off, bits = struct.unpack_from('<2I', raw, ro + i * 8)
                    if bits >> 27 & 1 and bits >> 25 & 3 == 3 and bits >> 28 == 11:
                        word = struct.unpack_from('<Q', raw, sec['fileoff'] + off)[0]
                        entries[sec['va'] + off] = (syms[bits & 0xffffff][0], word)
    slots = [(int(t, 16), int(d, 16)) for t, d in abi['slots']]
    slide = int(abi['collection_slide'], 16)
    compiled = [entries.get(vtable + 16 + i * 8) for i in range(len(slots))]
    if not all(compiled):
        raise RuntimeError(f'NativeHidInput vtable size mismatch (expected {len(slots)})')
    bindings = {name: int(va, 16) + slide for name, va in abi['bindings'].items()}
    by_div = {}
    for i, (_, d) in enumerate(slots):
        by_div.setdefault(d, []).append(i)
    slot_of = {}
    for i, (name, word) in enumerate(compiled):
        if not (word >> 63 and (word >> 48) & 1 and ((word >> 49) & 3) == 0 and ((word >> 32) & 0xffff) == slots[i][1]):
            raise RuntimeError(f'HID slot {i} ({name}) diversity mismatch: got {((word >> 32) & 0xffff):#x}, want {slots[i][1]:#x}')
        if '11IOHIDDevice' in name:
            slot_of[name] = i
    for name in sorted({n for n, _, k in syms if k == 1 and '11IOHIDDevice' in n} - set(bindings)):
        if name in slot_of:
            slot = slot_of[name]
        else:
            matches = by_div.get(pac_discriminator(name[1:]), [])
            if len(matches) != 1:
                raise RuntimeError(f'HID symbol {name}: expected 1 slot for discriminator, got {matches}')
            slot = matches[0]
        bindings[name] = slots[slot][0] + slide
    data_syms = {'__ZN11IOHIDDevice10gMetaClassE', '__ZTV11IOHIDDevice'}
    text, addresses = '.text\n.p2align 2\n', ''
    for name, va in sorted(bindings.items()):
        if name in data_syms:
            addresses += f'.globl {name}\n.set {name}, {va:#x}\n'
        else:
            addresses += f'.globl {name}.original\n.set {name}.original, {va:#x}\n'
            text += f'.globl {name}\n{name}:\n bti c\n b {name}.original\n'
    bind_s = out_dir / 'hid-original-bindings.S'
    addr_s = out_dir / 'hid-original-addresses.S'
    bind_s.write_text(text)
    addr_s.write_text(addresses)
    bind_o = out_dir / 'hid-original-bindings.o'
    addr_o = out_dir / 'hid-original-addresses.o'
    subprocess.run(['xcrun', 'clang', '-target', triple, '-c', str(bind_s), '-o', str(bind_o)], check=True)
    subprocess.run(['xcrun', 'clang', '-target', triple, '-c', str(addr_s), '-o', str(addr_o)], check=True)
    return [bind_o, addr_o]


def build_software_aes_bindings(abi_path, obj_path, out_dir, triple):
    abi = json.loads(abi_path.read_text())
    if int(abi['instance_size'], 16) != 0x120:
        raise RuntimeError(f'Unexpected IOAESAccelerator instance_size: {abi["instance_size"]}')
    raw = obj_path.read_bytes()
    magic, cpu, _, _, count = struct.unpack_from('<5I', raw)
    if magic != 0xfeedfacf or cpu != 0x100000c:
        raise RuntimeError('Expected arm64 Mach-O object for NativeAES27')
    commands, segments, offset = [], [], 32
    for _ in range(count):
        command, size = struct.unpack_from('<2I', raw, offset)
        commands.append((command, offset, size))
        if command == 0x19:
            nsects = struct.unpack_from('<I', raw, offset + 64)[0]
            sections = []
            for i in range(nsects):
                at = offset + 72 + 80 * i
                va, length, fileoff = struct.unpack_from('<QQI', raw, at + 32)
                sections.append({'va': va, 'size': length, 'fileoff': fileoff, 'command_offset': at})
            segments.append({'sections': sections})
        offset += size
    st = next(p for c, p, _ in commands if c == 2)
    sy, scount, strings, _ = struct.unpack_from('<4I', raw, st + 8)
    syms = []
    for i in range(scount):
        ix, kind, _, _, va = struct.unpack_from('<IBBHQ', raw, sy + i * 16)
        syms.append((raw[strings + ix:raw.index(b'\0', strings + ix)].decode(), va, kind))
    vtable = next(va for name, va, _ in syms if name == '__ZTV9NativeAES')
    entries = {}
    for segment in segments:
        for sec in segment['sections']:
            if sec['va'] <= vtable < sec['va'] + sec['size']:
                ro, rc = struct.unpack_from('<2I', raw, sec['command_offset'] + 56)
                for i in range(rc):
                    off, bits = struct.unpack_from('<2I', raw, ro + i * 8)
                    if bits >> 27 & 1 and bits >> 25 & 3 == 3 and bits >> 28 == 11:
                        word = struct.unpack_from('<Q', raw, sec['fileoff'] + off)[0]
                        entries[sec['va'] + off] = (syms[bits & 0xffffff][0], word)
    slots = [(int(t, 16), int(d, 16)) for t, d in abi['slots']]
    slide = int(abi['collection_slide'], 16)
    base = abi.get('base_slots', 168)
    compiled = [entries.get(vtable + 16 + i * 8) for i in range(len(slots))]
    if not all(compiled) or (vtable + 16 + len(slots) * 8) in entries:
        raise RuntimeError(f'NativeAES vtable size mismatch (expected {len(slots)})')
    bindings = {name: int(va, 16) + slide for name, va in abi['bindings'].items()}
    family = ('19IOCryptoAccelerator', '16IOAESAccelerator')
    own = ('__ZN9NativeAES', '__ZNK9NativeAES')
    for i, (name, word) in enumerate(compiled):
        if not (word >> 63 and (word >> 48) & 1 and ((word >> 49) & 3) == 0 and ((word >> 32) & 0xffff) == slots[i][1]):
            raise RuntimeError(f'AES slot {i} ({name}) diversity mismatch: got {((word >> 32) & 0xffff):#x}, want {slots[i][1]:#x}')
        if name.startswith(own):
            continue
        if any(c in name for c in family):
            expected = slots[i][0] + slide
            if bindings.setdefault(name, expected) != expected:
                raise RuntimeError(f'AES slot {i} ({name}) binding mismatch')
        elif i >= base:
            raise RuntimeError(f'AES slot {i} ({name}) unexpected non-family method above IOService base {base}')
    used = {n for n, _, k in syms if k == 1 and any(c in n for c in family)}
    missing = sorted(used - set(bindings))
    if missing:
        raise RuntimeError(f'Unbound SoftwareAES imports: {missing}')
    used |= {e[0] for e in compiled if not e[0].startswith(own) and any(c in e[0] for c in family)}
    text, addresses = '.text\n.p2align 2\n', ''
    for name in sorted(used):
        va = bindings[name]
        if name.endswith('10gMetaClassE') or name.startswith('__ZTV'):
            addresses += f'.globl {name}\n.set {name}, {va:#x}\n'
        else:
            addresses += f'.globl {name}.original\n.set {name}.original, {va:#x}\n'
            text += f'.globl {name}\n{name}:\n bti c\n b {name}.original\n'
    bind_s = out_dir / 'aes-original-bindings.S'
    addr_s = out_dir / 'aes-original-addresses.S'
    bind_s.write_text(text)
    addr_s.write_text(addresses)
    bind_o = out_dir / 'aes-original-bindings.o'
    addr_o = out_dir / 'aes-original-addresses.o'
    subprocess.run(['xcrun', 'clang', '-target', triple, '-c', str(bind_s), '-o', str(bind_o)], check=True)
    subprocess.run(['xcrun', 'clang', '-target', triple, '-c', str(addr_s), '-o', str(addr_o)], check=True)
    return [bind_o, addr_o]


def build_iogpu_accelerator_bindings(abi_path, out_dir, triple):
    abi = json.loads(abi_path.read_text())
    slide = int(abi['collection_slide'], 16)
    bindings = {
        '__ZN15IOAcceleratorESC2EPK11OSMetaClass': int(abi['constructor'], 16) + slide,
        '__ZN15IOAcceleratorESD2Ev': int(abi['destructor'], 16) + slide,
        '__ZN15IOAcceleratorES10gMetaClassE': int(abi['metaclass'], 16) + slide,
    }
    text, addresses = '.text\n.p2align 2\n', ''
    for name, va in sorted(bindings.items()):
        if name.endswith('gMetaClassE'):
            addresses += f'.globl {name}\n.set {name}, {va:#x}\n'
        else:
            addresses += f'.globl {name}.original\n.set {name}.original, {va:#x}\n'
            text += f'.globl {name}\n{name}:\n bti c\n b {name}.original\n'
    bind_s = out_dir / 'iogpu-original-bindings.S'
    addr_s = out_dir / 'iogpu-original-addresses.S'
    bind_s.write_text(text)
    addr_s.write_text(addresses)
    bind_o = out_dir / 'iogpu-original-bindings.o'
    addr_o = out_dir / 'iogpu-original-addresses.o'
    subprocess.run(['xcrun', 'clang', '-target', triple, '-c', str(bind_s), '-o', str(bind_o)], check=True)
    subprocess.run(['xcrun', 'clang', '-target', triple, '-c', str(addr_s), '-o', str(addr_o)], check=True)
    return [bind_o, addr_o]


def find_adreno_firmware_dir(device):
    candidates = []
    if os.environ.get('DITING_ADRENO_FIRMWARE'):
        candidates.append(Path(os.environ['DITING_ADRENO_FIRMWARE']))
    candidates += [
        Path('/home/dddqmmx/UniXNU/diting-work/build/services168/firmware/adreno730'),
        device / 'out/ipados18/firmware/adreno730',
        device / 'out/firmware/adreno730',
    ]
    needed = ('gmu_gen70000.bin', 'a730_sqe.fw', 'a730_zap.mdt', 'a730_zap.b01', 'a730_zap.b02')
    for candidate in candidates:
        if candidate.is_dir() and all((candidate / name).is_file() for name in needed):
            return candidate
    raise RuntimeError('SM8475 Adreno 730 firmware directory not found (set DITING_ADRENO_FIRMWARE)')


def emit_adreno_firmware_source(firmware_dir, out_c):
    files = [
        ('DitingAdrenoGmuFirmware', 'gmu_gen70000.bin'),
        ('DitingAdrenoSqeFirmware', 'a730_sqe.fw'),
        ('DitingAdrenoZapMdt', 'a730_zap.mdt'),
        ('DitingAdrenoZapCode', 'a730_zap.b01'),
        ('DitingAdrenoZapSignature', 'a730_zap.b02'),
    ]
    chunks = ['#include <stddef.h>\n#include <stdint.h>\n']
    for sym, fname in files:
        data = (firmware_dir / fname).read_bytes()
        chunks.append(f'const uint8_t {sym}[] = {{\n')
        for i in range(0, len(data), 32):
            chunks.append(','.join(str(b) for b in data[i:i + 32]) + ',\n')
        chunks.append(f'}};\nconst size_t {sym}Bytes = sizeof({sym});\n')
    content = ''.join(chunks)
    if not out_c.is_file() or out_c.read_text() != content:
        out_c.write_text(content)
    return out_c


def emit_adreno_registers_header(adreno_dir, out_header):
    regs = (adreno_dir / 'DitingAdrenoCpProfile.h').read_text()
    needed = sorted(set(re.findall(r'\bGEN7_[A-Z0-9_]+\b', (adreno_dir / 'DitingAdrenoGmu.c').read_text())))
    lines = ['/* Register facts from NothingOSS SM8475 gen7_reg.h. Dword offsets. */']
    for name in needed:
        match = re.search(r'^#define\s+' + name + r'\s+(0x[0-9a-fA-F]+)[uU]?\s*$', regs, re.M)
        if not match:
            raise RuntimeError('Missing SM8475 register ' + name)
        lines.append(f'#define {name} {hex(int(match[1], 16))}u')
    content = '\n'.join(lines) + '\n'
    if not out_header.is_file() or out_header.read_text() != content:
        out_header.write_text(content)
    return out_header


def find_objcopy():
    for candidate in (Path(os.environ.get('UNIXNU_TOOLCHAIN', '')) / 'llvm/bin/llvm-objcopy',
                      Path('/usr/bin/llvm-objcopy')):
        if candidate.is_file():
            return str(candidate)
    found = shutil.which('llvm-objcopy')
    if not found:
        raise RuntimeError('llvm-objcopy not found')
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, required=True, help='prepared private iOS 27 XNU tree')
    parser.add_argument('--device', type=Path, default=Path(os.environ.get('UNIXNU_DEVICE', ROOT)))
    parser.add_argument('--platform-helpers', type=Path, help='pinned UniXNU-sheng helper checkout')
    parser.add_argument('--out', type=Path, help='private output; defaults to the selected build tree')
    args = parser.parse_args()
    tree, device = args.tree.resolve(), args.device.resolve()
    out = args.out.resolve() if args.out else tree / 'build/diting-ios27-drivers'
    if out == device or device in out.parents:
        parser.error('Generated driver objects must stay outside the device repository')
    profile = tomllib.loads((device / 'os-profiles/ios-27.2.1-24B156/profile.toml').read_text())
    helper_pin = next(item for item in profile['source'] if item['name'] == 'DitingPlatformHelpers')
    helpers = (args.platform_helpers or tree / 'refer/DitingPlatformHelpers').resolve()
    helper_revision = subprocess.check_output(['git', '-C', str(helpers), 'rev-parse', 'HEAD'], text=True).strip()
    helper_changes = subprocess.check_output(['git', '-C', str(helpers), 'status', '--porcelain',
                                              '--untracked-files=no'], text=True)
    if helper_revision != helper_pin['commit'] or helper_changes:
        parser.error('Diting platform helpers must match the clean selected source revision')
    if sys.platform == 'linux':
        sys.path.insert(0, str(tree / 'scripts'))
        from unixnu.kernel import linux_env
        os.environ.update(linux_env(SimpleNamespace(tree=tree, board=profile)))
    os.environ['UNIXNU_XNU_ARCH'] = profile['os']['arch']
    os.environ['UNIXNU_IOS_VERSION'] = profile['os']['ios_version']
    target = load_helper('diting_ios27_driver_target', tree / 'scripts/kernel_target.py')
    headers = load_helper('diting_ios27_storage_headers', tree / 'scripts/prepare-storage-headers.py')
    compiler = load_helper('diting_ios27_source_compiler', tree / 'scripts/build-sheng-source-drivers.py')
    pin = next(item for item in profile['source'] if item['name'] == 'IOStorageFamily')
    header_report = headers.prepare(tree / 'refer/IOStorageFamily', pin['commit'], out / 'storage-headers')
    hid_root = tree / 'refer/IOHIDFamily'
    hid_inc = out / 'storage-headers/include'
    for src_dir, rel in ((hid_root / 'IOHIDFamily', 'IOKit/hid'),
                         (hid_root / 'IOHIDSystem/IOKit/hidsystem', 'IOKit/hidsystem')):
        dst_dir = hid_inc / rel
        dst_dir.mkdir(parents=True, exist_ok=True)
        for hfile in sorted(src_dir.glob('*.h')):
            shutil.copyfile(hfile, dst_dir / hfile.name)
    gfx_dir = hid_inc / 'IOKit/graphics'
    gfx_dir.mkdir(parents=True, exist_ok=True)
    (gfx_dir / 'IOGraphicsTypes.h').write_text(
        '#pragma once\n#include <IOKit/IOTypes.h>\n'
        'typedef struct IOGPoint { SInt16 x; SInt16 y; } IOGPoint;\n'
        'typedef struct IOGSize { SInt16 width; SInt16 height; } IOGSize;\n'
        'typedef struct IOGBounds { SInt16 minx; SInt16 maxx; SInt16 miny; SInt16 maxy; } IOGBounds;\n'
    )
    hiddk_dir = hid_inc / 'HIDDriverKit'
    hiddk_dir.mkdir(parents=True, exist_ok=True)
    (hiddk_dir / 'IOHIDDevice.h').write_text(
        '#pragma once\n'
        '#define IOHIDDevice_Methods\n'
        '#define IOHIDDevice_KernelMethods\n'
        '#define IOHIDDevice_VirtualMethods\n'
    )
    objroot = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    recipe = objroot / 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    record = json.loads(recipe.read_text().strip().rstrip(','))
    c_record = json.loads((objroot / 'osfmk/DEVELOPMENT/startup.o.json').read_text().strip().rstrip(','))
    target.check_object((Path(record['directory']) / 'IOPlatformExpert.cpo').read_bytes(), 'Kernel recipe')
    native = objroot / 'mach.development.bcm2837.unstripped.noctf'
    native_exports = provider_exports(native) if native.is_file() else set()
    out.mkdir(parents=True, exist_ok=True)
    # Compile Apple's public IOBlockStorageDevice as deferred UniXNUBlockStorageDevice
    # whose parent resolves to the kext's IOBlockStorageDevice metaclass at runtime.
    base_cpp = out / 'IOBlockStorageDevice.cpp'
    original_base = (tree / 'refer/IOStorageFamily/IOBlockStorageDevice.cpp').read_text()
    macro = 'OSDefineMetaClassAndAbstractStructors(IOBlockStorageDevice,IOService)'
    if original_base.count(macro) != 1:
        raise RuntimeError('Pinned IOBlockStorageDevice registration macro changed')
    base_cpp.write_text(original_base.replace(macro, '''
extern "C" const OSMetaClass *native_source_storage_parent(void);
OSDefineMetaClassWithInit(IOBlockStorageDevice, IOService, )
IOBlockStorageDevice::MetaClass::MetaClass()
    : OSMetaClass("UniXNUBlockStorageDevice", native_source_storage_parent(), sizeof(IOBlockStorageDevice)) {}
OSDefineAbstractStructors(IOBlockStorageDevice, IOService)
OSDefineOperatorMethods(IOBlockStorageDevice)
'''))
    runtime_cpp = out / 'DitingSourceStorage27.cpp'
    runtime_cpp.write_text(SOURCE_STORAGE_RUNTIME)
    usb_cpp = out / 'DitingUsb27.cpp'
    usb_cpp.write_text(USB_CONSOLE_WRAPPER)
    gop_dir = find_external_driver(tree, device, 'GopFramebuffer')
    aks_dir = find_external_driver(tree, device, 'AppleKeyStoreState')
    legacy_abi = emit_legacy_display_header(device / 'configs/legacy-display-24B156.json',
                                            out / 'OriginalLegacyFramebufferABI.h')
    legacy_cpp = out / 'NativeLegacyDisplay.cpp'
    legacy_src = (gop_dir / 'NativeLegacyDisplay.cpp').read_text()
    legacy_replacements = [
        ('(this)+5592', '(this)+kLegacyVsyncClients'),
        ('(request)+2624', '(request)+kRequestSurfaces'),
        ('bytes+2624', 'bytes+kRequestSurfaces'),
        ('word(792),word(796),word(800)', 'word(kRequestMasks),word(kRequestMasks+4),word(kRequestMasks+8)'),
        ('const unsigned rectOffsets[]={17,81};', 'const unsigned rectOffsets[]={kRequestRect0,kRequestRect1};'),
        ('offset=804;offset<=812;offset+=4', 'offset=kRequestOptions;offset<=kRequestOptions+8;offset+=4'),
        ('word(816)==width && word(820)==height', 'word(kRequestDims)==width && word(kRequestDims+4)==height'),
        ('PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));',
         'if(!PE_parse_boot_argn("vsync",&hz,sizeof(hz)))PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));'),
        ('PE_parse_boot_argn("sheng-vsync-fixed",&locked,sizeof(locked));',
         'if(!PE_parse_boot_argn("vsync-fixed",&locked,sizeof(locked)))PE_parse_boot_argn("sheng-vsync-fixed",&locked,sizeof(locked));'),
        ('''    void announcePoweredByFirmware() {
        struct StateRequest {uint32_t skipRequested,requested,skipPowered,powered;uint8_t changed;uint8_t pad[11];} request={1,0,0,1,1,{}};
        typedef int (*Setter)(void *,StateRequest *,bool);
        Setter setter=reinterpret_cast<Setter *>(*reinterpret_cast<void **>(this))[354];
        setter(this,&request,false);
    }''',
         '''    void announcePoweredByFirmware() {
        // IOMobileFramebufferLegacy::start() clears +140; platform subclasses
        // (e.g. AppleMobileDispH18P-DCP) set +140=1 so request_power_change
        // (sel 79) dispatches through slot 241 -> slot 365 -> applyStateRequest
        // instead of returning kIOReturnUnsupported.
        reinterpret_cast<unsigned char *>(this)[140]=1;
        OriginalLegacyStateRequest request={0,1,0,1,1,{}};
        int rc=applyStateRequest(&request,false);
        IOLog("DITING_FB: announcePowered rc=%d\\n", rc);
    }'''),
        ('''    LEAF(211)
    LEAF(245)
    LEAF(246)
    LEAF(306)
    LEAF(307)
    LEAF(338)
    LEAF(345)''',
         '''    NATIVE_LEGACY_LEAVES(LEAF)
    int lastPoweredState=-1;
    int applyStateRequest(OriginalLegacyStateRequest *request,bool flag) override {
        int rc=IOMobileFramebufferLegacy::applyStateRequest(request,flag);
        if(request && !request->skipPowered) {
            const int powered=request->powered?1:0;
            if(powered!=lastPoweredState) {
                lastPoweredState=powered;
                IOLog("DITING_FB: power state -> %d (rc=%d)\\n",powered,rc);
                diting_backlight_display_power(powered!=0);
            }
        }
        return rc;
    }
    void create_default_fb_surface() override {
        uint32_t w=0,h=0;getDisplayDimensions(&w,&h);if(w && h)installDefaultSurface(w,h);
    }
    bool isDisplayPowered() override { return lastPoweredState != 0; }
    bool cancelSwapsAfterTimeout() override { return false; }'''),
        ('extern "C" bool native_legacy_gpu_offer(void *display,IOMemoryDescriptor *,uint64_t stride,uint32_t width,uint32_t height);',
         '#include "NativeHalfCodes.h"\nextern "C" void diting_backlight_display_power(bool on);\nextern "C" bool native_legacy_gpu_offer(void *display,IOMemoryDescriptor *,uint64_t stride,uint32_t width,uint32_t height,uint32_t format);'),
        ('''static inline bool nativeBgra(uint32_t format,uint32_t planes) {
    return (format==0x42475241 && !planes) || (format==0x26424741 && planes<=1);
}''',
         '''static inline bool nativeBgra(uint32_t format,uint32_t planes) {
    return (format==0x42475241 && !planes) || (format==0x26424741 && planes<=1);
}
static inline bool nativeRgba16f(uint32_t format,uint32_t planes) {
    return format==0x52476841 && !planes;
}
static inline bool nativeSurfaceFormatOk(uint32_t format,uint32_t planes) {
    return nativeBgra(format,planes) || nativeRgba16f(format,planes);
}
static inline bool nativeSurfaceBoundsForFormat(uint32_t width,uint32_t height,uint32_t format,uint64_t stride,uint64_t allocation,uint64_t length) {
    const uint64_t bpp=format==0x52476841?8ULL:4ULL;
    const uint64_t row=(uint64_t)width*bpp;
    return width && height && stride>=row && allocation<=length &&
           (uint64_t)height<=allocation/stride &&
           stride*((uint64_t)height-1)+row<=allocation;
}
static inline bool nativeHalfEncoded(void) {
    unsigned mode=1;
    if(!PE_parse_boot_argn("fb-half",&mode,sizeof(mode)))PE_parse_boot_argn("sheng-fb-half",&mode,sizeof(mode));
    return mode==2;
}
static bool nativeCopyHalfToBgra(volatile void *dstBase,uint64_t dstBytes,uint64_t dstStride,
                                 const void *srcBase,uint64_t srcBytes,uint64_t srcStride,
                                 uint32_t width,uint32_t height,bool encoded,uint64_t *litOut) {
    static uint8_t *lut[2]={nullptr,nullptr};
    const unsigned idx=encoded?1u:0u;
    if(!lut[idx]) {
        uint8_t *t=static_cast<uint8_t *>(IOMallocData(65536));
        if(!t)return false;
        for(uint32_t bits=0;bits<65536;++bits)t[bits]=native_half::code((uint16_t)bits,encoded);
        lut[idx]=t;
    }
    const uint64_t srcRow=(uint64_t)width*8, dstRow=(uint64_t)width*4;
    if(!dstBase || !srcBase || srcStride<srcRow || dstStride<dstRow ||
       srcStride*((uint64_t)height-1)+srcRow>srcBytes || dstStride*((uint64_t)height-1)+dstRow>dstBytes)return false;
    const uint8_t *table=lut[idx];
    const uint8_t *s=static_cast<const uint8_t *>(srcBase);
    volatile uint8_t *d=static_cast<volatile uint8_t *>(dstBase);
    uint64_t lit=0;
    for(uint32_t y=0;y<height;++y) {
        const uint16_t *sp=reinterpret_cast<const uint16_t *>(s+y*srcStride);
        volatile uint32_t *dp=reinterpret_cast<volatile uint32_t *>(d+y*dstStride);
        for(uint32_t x=0;x<width;++x) {
            const uint32_t r=table[sp[x*4+0]];
            const uint32_t g=table[sp[x*4+1]];
            const uint32_t b=table[sp[x*4+2]];
            const uint32_t px=0xff000000U|(r<<16)|(g<<8)|b;
            if((px&0xffffffU)!=0)++lit;
            dp[x]=px;
        }
    }
    if(litOut)*litOut=lit;
    return true;
}'''),
        ('''        if(!memory || !memory->metaCast("IOMemoryDescriptor") || !native_legacy_surface_bounds(width,height,sourceStride,allocation,memory->getLength()) ||
           native_legacy_surface_width(surface)<width || native_legacy_surface_height(surface)<height ||
           !nativeBgra(native_legacy_surface_format(surface),native_legacy_surface_planes(surface)) ||''',
         '''        const uint32_t surfFmt=native_legacy_surface_format(surface);
        const uint32_t surfPlanes=native_legacy_surface_planes(surface);
        const bool isHalf=nativeRgba16f(surfFmt,surfPlanes);
        const uint32_t ngpFmt=isHalf?(nativeHalfEncoded()?3u:2u):1u;
        if(!memory || !memory->metaCast("IOMemoryDescriptor") || !nativeSurfaceBoundsForFormat(width,height,surfFmt,sourceStride,allocation,memory->getLength()) ||
           native_legacy_surface_width(surface)<width || native_legacy_surface_height(surface)<height ||
           !nativeSurfaceFormatOk(surfFmt,surfPlanes) ||'''),
        ('if(!gpuBypass && native_legacy_gpu_offer(this,memory,sourceStride,width,height)) {',
         'if(!gpuBypass && native_legacy_gpu_offer(this,memory,sourceStride,width,height,ngpFmt)) {'),
        ('''        if(!native_legacy_copy_layout(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
            bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
            source->getLength(),sourceStride,width,height)) {
            OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
        }''',
         '''        if(isHalf) {
            if(!native_legacy_prepare_scanout()) {
                OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
            }
            uint64_t lit=0;
            if(!nativeCopyHalfToBgra(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
                source->getLength(),sourceStride,width,height,ngpFmt==3u,&lit)) {
                OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
            }
            __asm__ volatile("dsb sy" ::: "memory");
            shadowValid=false;
            if(scanBudget){--scanBudget;native_legacy_frame_observed(2,lit,0,0,width-1,height-1);}
            OSSafeReleaseNULL(source);transferred=true;
            native_legacy_transfer_observed(kIOReturnSuccess,request,length);
            finishTransfer(request);
            return;
        }
        if(!native_legacy_copy_layout(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
            bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
            source->getLength(),sourceStride,width,height)) {
            OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
        }'''),
        ('''                    if(native_legacy_surface_width(surface)>=width && native_legacy_surface_height(surface)>=height &&
                       nativeBgra(native_legacy_surface_format(surface),native_legacy_surface_planes(surface)) &&
                       memory && memory->metaCast("IOMemoryDescriptor") &&
                       native_legacy_surface_bounds(width,height,native_legacy_surface_stride(surface),
                           native_legacy_surface_allocation(surface),memory->getLength())) {''',
         '''                    const uint32_t vfmt=native_legacy_surface_format(surface);
                    if(native_legacy_surface_width(surface)>=width && native_legacy_surface_height(surface)>=height &&
                       nativeSurfaceFormatOk(vfmt,native_legacy_surface_planes(surface)) &&
                       memory && memory->metaCast("IOMemoryDescriptor") &&
                       nativeSurfaceBoundsForFormat(width,height,vfmt,native_legacy_surface_stride(surface),
                           native_legacy_surface_allocation(surface),memory->getLength())) {'''),
    ]
    for old, new in legacy_replacements:
        if old not in legacy_src:
            raise RuntimeError(f'NativeLegacyDisplay.cpp expected snippet not found: {old}')
        legacy_src = legacy_src.replace(old, new)
    legacy_cpp.write_text(legacy_src)
    presenter_cpp = out / 'NativeGOPPresenter.cpp'
    presenter_src = (gop_dir / 'NativeGOPPresenter.cpp').read_text()
    presenter_replacements = [
        ('bool native_gop_gpu_offer(IOService *,void *display,IOMemoryDescriptor *,uint64_t stride,uint32_t width,uint32_t height);',
         'bool native_gop_gpu_offer(IOService *,void *display,IOMemoryDescriptor *,uint64_t stride,uint32_t width,uint32_t height,uint32_t format);'),
        ('struct {bool offered,delivered;uint64_t sequence;void *display;IOMemoryDescriptor *memory;IOMemoryMap *map;\n            uint64_t stride;uint32_t width,height;} swap={};',
         'struct {bool offered,delivered;uint64_t sequence;void *display;IOMemoryDescriptor *memory;IOMemoryMap *map;\n            uint64_t stride;uint32_t width,height,format;} swap={};'),
        ('bool offer(void *display,IOMemoryDescriptor *,uint64_t stride,uint32_t width,uint32_t height);',
         'bool offer(void *display,IOMemoryDescriptor *,uint64_t stride,uint32_t width,uint32_t height,uint32_t format);'),
        ('''bool NativeGOPPresenter::offer(void *display,IOMemoryDescriptor *memory,uint64_t stride,uint32_t width,uint32_t height) {
    if(!gpuMode || !memory || !display || !gate)return false;
    IOLockLock(gate);
    bool accepted=online && gpuClient && !swap.offered;
    if(accepted) {
        memory->retain();
        swap={true,false,++swapSequence,display,memory,nullptr,stride,width,height};
        native_gop_gpu_offers=native_gop_gpu_offers+1;
        IOLockWakeup(gate,&swap,false);
    }
    IOLockUnlock(gate);return accepted;
}''',
         '''bool NativeGOPPresenter::offer(void *display,IOMemoryDescriptor *memory,uint64_t stride,uint32_t width,uint32_t height,uint32_t format) {
    if(!gpuMode || !memory || !display || !gate)return false;
    IOLockLock(gate);
    bool accepted=online && gpuClient && !swap.offered;
    uint64_t seq=0;
    if(accepted) {
        memory->retain();
        swap={true,false,++swapSequence,display,memory,nullptr,stride,width,height,format};
        seq=swap.sequence;
        native_gop_gpu_offers=native_gop_gpu_offers+1;
        IOLockWakeup(gate,&swap,false);
    }
    IOLockUnlock(gate);
    if(accepted && seq<=6)IOLog("DITING_FB: gpu scanout offer #%llu fmt=%u %ux%u stride=%llu\\n",(unsigned long long)seq,format,width,height,(unsigned long long)stride);
    return accepted;
}'''),
        ('const uint64_t sequence=swap.sequence,stride=swap.stride;const uint32_t width=swap.width,height=swap.height;',
         'const uint64_t sequence=swap.sequence,stride=swap.stride;const uint32_t width=swap.width,height=swap.height,format=swap.format;'),
        ('out->width=width;out->height=height;out->format=NGP_BGRX8888;out->reserved=0;',
         'out->width=width;out->height=height;out->format=format?format:NGP_BGRX8888;out->reserved=0;'),
        ('else native_gop_gpu_completions=native_gop_gpu_completions+1;',
         'else {native_gop_gpu_completions=native_gop_gpu_completions+1;if(native_gop_gpu_completions<=6)IOLog("DITING_FB: gpu scanout complete #%llu status=0x%x\\n",(unsigned long long)native_gop_gpu_completions,status);}'),
        ('''extern "C" bool native_gop_gpu_offer(IOService *presenter,void *display,IOMemoryDescriptor *memory,uint64_t stride,uint32_t width,uint32_t height) {
    NativeGOPPresenter *p=OSDynamicCast(NativeGOPPresenter,presenter);
    return p && p->offer(display,memory,stride,width,height);
}''',
         '''extern "C" bool native_gop_gpu_offer(IOService *presenter,void *display,IOMemoryDescriptor *memory,uint64_t stride,uint32_t width,uint32_t height,uint32_t format) {
    NativeGOPPresenter *p=OSDynamicCast(NativeGOPPresenter,presenter);
    return p && p->offer(display,memory,stride,width,height,format);
}'''),
    ]
    for old, new in presenter_replacements:
        if old not in presenter_src:
            raise RuntimeError(f'NativeGOPPresenter.cpp expected snippet not found: {old}')
        presenter_src = presenter_src.replace(old, new)
    presenter_cpp.write_text(presenter_src)
    adreno_dir = device / 'drivers/DitingAdreno'
    power_dir = device / 'drivers/DitingPower'
    crypto_dir = device / 'drivers/DitingCrypto'
    wifi_dir = device / 'drivers/DitingWifi'
    wifi_stage = Path('/home/dddqmmx/UniXNU/diting-work/build/services227/ios27-24B156/wifi-ath11k-27')
    ath_shared_inc = device / 'refer/ipados18/Ath12kWifi/include'
    if not ath_shared_inc.is_dir():
        ath_shared_inc = Path('/home/dddqmmx/UniXNU/UniXNU-diting/refer/ipados18/Ath12kWifi/include')
    emit_adreno_registers_header(adreno_dir, out / 'DitingAdrenoRegisters.h')
    adreno_fw_c = emit_adreno_firmware_source(find_adreno_firmware_dir(device), out / 'DitingAdrenoFirmware.generated.c')
    hid_cpp = out / 'NativeHidInput.cpp'
    shutil.copyfile(device / 'drivers/DitingTouch/NativeHidInput27.cpp', hid_cpp)
    sdk_kernel = Path(header_report['sdk']) / 'System/Library/Frameworks/Kernel.framework/Versions/A/Headers'
    includes = ['-I', str(out), '-I', str(out / 'storage-headers/include'),
                '-I', str(device / 'drivers/DitingMetal'),
                '-I', str(gop_dir), '-I', str(aks_dir),
                '-I', str(power_dir),
                '-I', str(device / 'drivers/ArmCpu'),
                '-I', str(crypto_dir),
                '-I', str(wifi_dir),
                '-I', str(wifi_stage),
                '-I', str(ath_shared_inc),
                '-I', str(device / 'drivers/DitingTouch'),
                '-I', str(device / 'drivers/DitingUsb'),
                '-I', str(adreno_dir),
                '-I', str(adreno_dir / 'ArmSmmu4K'),
                '-I', str(adreno_dir / 'submit'),
                '-I', str(helpers / 'package'),
                '-idirafter', str(sdk_kernel)]
    # NativeUfs and DitingPlatformResources are C++17 components. Keep their language contract
    # while using the selected kernel's architecture, deployment and public interfaces.
    extra = ['-I', str(tree / '.cache/xnu-work/osfmk'), '-fno-sanitize=all', '-DXNU_DITING_UFS_DATA=1',
             '-DXNU_DITING_IOS27_RO_SYSTEM=1', '-DDITING_GUEST_24B156=1',
             '-DDITING_ADSP_BUILTIN=0',
             '-DXNU_DITING_RAMOOPS_CONSOLE=1', '-DXNU_DITING_RAMOOPS_BASE=0xa7000000']
    report_path = out / 'report.json'
    report = {'profile': 'ios-27.2.1-24B156', 'target': target.TRIPLE,
              'storage_source_revision': header_report['source_revision'],
              'platform_helpers_revision': helper_revision,
              'storage_binary_abi_qualified': False, 'kernel_link_qualified': False,
              'runtime_qualified': False, 'device_written': False, 'objects': []}
    deferred_names = {'IOBlockStorageDevice.cpp', 'NativeUfs.cpp', 'DitingUfsTransport.cpp',
                      'NativeLegacyDisplay.cpp', 'NativeGOPPresenter.cpp', 'NativeKeyStoreEvents.cpp',
                      'NativeAdrenoAccelerator.cpp', 'NativeHidInput.cpp', 'NativeAES27.cpp',
                      'DitingBattery.cpp', 'DitingWifiNet.cpp', 'DitingBacklight.cpp'}
    adreno_c_sources = [
        adreno_dir / (name + '.c') for name in (
            'AdrenoFirmware', 'DitingAdrenoHfi', 'DitingAdrenoGmu', 'DitingAdrenoSmmu',
            'DitingAdrenoPower', 'DitingAdrenoClocks', 'DitingAdrenoZap', 'submit/AdrenoPM4',
            'ArmSmmu4K/ArmSmmu4K', 'ArmSmmu4K/ArmSmmu4KContext', 'ArmSmmu4K/ArmSmmu4KProgram',
            'ArmSmmu4K/ArmSmmu4KStreams', 'ArmSmmu4K/ArmSmmu4KAudit', 'ArmSmmu4K/ArmSmmu4KLive',
            'ArmSmmu4K/ArmSmmu4KTableMemory', 'ArmSmmu4K/ArmSmmu4KMmio',
        )
    ] + [adreno_fw_c]
    adreno_cpp_sources = [
        adreno_dir / (name + '.cpp') for name in (
            'DitingAdrenoPlatform', 'DitingAdrenoCp', 'DitingAdrenoNativeGlue', 'DitingAdrenoAccelerator',
            'submit/NativeAdrenoBoard', 'submit/NativeAdrenoClient', 'submit/NativeAdrenoPublish',
            'submit/NativeAdrenoRing', 'submit/NativeAdrenoCpInit', 'submit/NativeAdrenoTimer',
            'submit/NativeAdrenoIrq', 'submit/NativeAdrenoPwrup', 'submit/NativeAdrenoGpuContext',
            'submit/NativeAdrenoCpu', 'submit/NativeAdrenoMemory', 'submit/NativeAdrenoSmmuMemory',
            'submit/NativeAdrenoSmmuContext', 'submit/NativeAdrenoVm', 'submit/NativeAdrenoSubmit',
            'submit/NativeAdrenoUserClient', 'submit/NativeAdrenoAccelerator',
        )
    ]
    power_battery_sources = [
        power_dir / (name + '.c') for name in (
            'QcomSmem', 'QcomSmp2p', 'QcomGlink', 'QcomMdt', 'QcomQmp',
            'ShengBattmgr', 'DitingUcsi', 'DitingFirmware',
        )
    ] + [
        power_dir / 'DitingAdsp.cpp',
        power_dir / 'DitingPmicGlink.cpp',
        power_dir / 'DitingBattery.cpp',
        power_dir / 'DitingCpuPolicy27.cpp',
    ]
    wifi_sources = [
        wifi_dir / 'DitingWifiPlatform.cpp',
        wifi_dir / 'DitingWifiNet.cpp',
    ]
    sources = [
        base_cpp,
        device / 'drivers/NativeUfs/NativeUfs.cpp',
        device / 'drivers/NativeUfs/DitingUfsTransport.cpp',
        device / 'drivers/DitingProbe.cpp',
        power_dir / 'DitingPlatformResources.cpp',
        power_dir / 'DitingDisplayTiming.cpp',
        power_dir / 'DitingBacklight.cpp',
        power_dir / 'DitingMemoryBandwidth.cpp',
        runtime_cpp,
        usb_cpp,
        helpers / 'package/QcomRpmh.c',
        power_dir / 'GeniI2c.c',
        legacy_cpp,
        presenter_cpp,
        aks_dir / 'NativeKeyStoreEvents.cpp',
        helpers / 'package/KeyStoreProbe.cpp',
        crypto_dir / 'NativeAES27.cpp',
        crypto_dir / 'DitingSoftwareAes27.cpp',
        hid_cpp,
        helpers / 'package/HidInputProbe.cpp',
        device / 'drivers/DitingTouch/DitingTouch27.cpp',
        device / 'drivers/DitingTouch/DitingButtons27.cpp',
        device / 'drivers/DitingLegacyDisplayProbe27.cpp',
        *power_battery_sources,
        *wifi_sources,
        *adreno_c_sources,
        *adreno_cpp_sources,
    ]
    objcopy = find_objcopy()
    objects = []
    try:
        for source in sources:
            obj = out / (source.stem + '.o')
            cpp = source.suffix != '.c'
            selected_record = record if cpp else c_record
            command = compiler.compile_command(selected_record, source, obj, includes,
                                               extra + (['-std=gnu++17'] if cpp else []))
            if source.name in ('DitingFirmware.c', 'DitingWifiNet.cpp'):
                filtered = []
                skip_next = False
                for i, tok in enumerate(command):
                    if skip_next:
                        skip_next = False
                        continue
                    if tok == '-D' and i + 1 < len(command) and command[i + 1] in ('MACH_KERNEL_PRIVATE', 'MACH_KERNEL'):
                        skip_next = True
                        continue
                    if tok == '-include' and i + 1 < len(command) and command[i + 1] == 'meta_features.h':
                        skip_next = True
                        continue
                    if tok == '-I' and i + 1 < len(command) and (command[i + 1] == '.' or 'xnu-work/osfmk' in command[i + 1]):
                        skip_next = True
                        continue
                    if tok in ('-DMACH_KERNEL_PRIVATE', '-DMACH_KERNEL') or 'xnu-work/osfmk' in tok:
                        continue
                    filtered.append(tok)
                filtered.append('-ULOCK_STATS')
                command = filtered
            if (source == base_cpp or
                source.name in ('NativeLegacyDisplay.cpp', 'NativeGOPPresenter.cpp', 'NativeKeyStoreEvents.cpp', 'NativeHidInput.cpp', 'HidInputProbe.cpp', 'NativeAES27.cpp') or
                adreno_dir in source.parents or power_dir in source.parents or wifi_dir in source.parents):
                command.append('-Wno-error')
            (out / (source.stem + '.command.json')).write_text(json.dumps(command, indent=2) + '\n')
            result = subprocess.run(command, cwd=selected_record['directory'], stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True)
            (out / (source.stem + '.log')).write_text(result.stdout)
            if result.returncode:
                raise RuntimeError(source.name + ': compile failed\n' + result.stdout[-4000:])
            target.check_object(obj.read_bytes(), source.name)
            if source.name == 'NativeLegacyDisplay.cpp':
                for bobj in build_legacy_display_bindings(legacy_abi, obj, out, target.TRIPLE):
                    target.check_object(bobj.read_bytes(), bobj.name)
                    objects.append(bobj)
            elif source.name == 'NativeHidInput.cpp':
                for bobj in build_hid_device_bindings(device / 'configs/hid-device-24B156.json', obj, out, target.TRIPLE):
                    target.check_object(bobj.read_bytes(), bobj.name)
                    objects.append(bobj)
            elif source.name == 'NativeAdrenoAccelerator.cpp':
                for bobj in build_iogpu_accelerator_bindings(device / 'configs/diting-iogpu-24B156.json', out, target.TRIPLE):
                    target.check_object(bobj.read_bytes(), bobj.name)
                    objects.append(bobj)
            elif source.name == 'NativeAES27.cpp':
                for bobj in build_software_aes_bindings(device / 'configs/software-aes-24B156.json', obj, out, target.TRIPLE):
                    target.check_object(bobj.read_bytes(), bobj.name)
                    objects.append(bobj)
            deferred = source.name in deferred_names
            if deferred:
                late = obj.with_suffix('.late.o')
                subprocess.run([objcopy, '--remove-section=__DATA,__mod_init_func',
                                f'--globalize-symbol=__GLOBAL__sub_I_{source.name}',
                                str(obj), str(late)], check=True)
                obj = late
            report['objects'].append({'source': str(source), 'object': str(obj),
                                      'deferred_metaclass': deferred, 'target_check': 'PASS'})
            objects.append(obj)
        ath11k_obj = wifi_stage / 'DitingAth11k-linked.o'
        if not ath11k_obj.is_file():
            raise RuntimeError(f'Missing arm64e ath11k object: {ath11k_obj}')
        target.check_object(ath11k_obj.read_bytes(), ath11k_obj.name)
        objects.append(ath11k_obj)
        report['objects'].append({'source': str(ath11k_obj), 'object': str(ath11k_obj),
                                  'deferred_metaclass': False, 'target_check': 'PASS'})
        # A static kernel has no dynamic linker to resolve absent weak imports.
        # Weak absolute zero symbols preserve NULL-provider semantics and carry
        # no code or hardware resources. Actual provider definitions must not
        # also receive an absolute NULL definition in this static link.
        optional = set().union(*(optional_imports(obj) for obj in objects))
        provided = set().union(*(symbols(obj, '-gU') for obj in objects))
        absent = sorted(optional - provided - native_exports)
        if absent:
            asm = out / 'OptionalProviders.S'
            data_stubs = {'_native_adsp_stats', '_native_adsp_text', '_native_battery_stats'}
            lines = ['/* Absent optional providers: weak non-null kernel-range stubs for arm64e chained fixups. */\n']
            for name in absent:
                if name in data_stubs:
                    lines.append(f'.data\n.align 3\n.globl {name}\n.weak_definition {name}\n{name}:\n.space 128\n')
                elif name in ('_diting_battery_usb_online', '_diting_battery_usb_data_online', '_diting_ucsi_usb_data_online'):
                    lines.append(f'.text\n.align 2\n.globl {name}\n.weak_definition {name}\n{name}:\nmovn w0, #1\nret\n')
                else:
                    lines.append(f'.text\n.align 2\n.globl {name}\n.weak_definition {name}\n{name}:\nmov x0, #0\nret\n')
            asm.write_text(''.join(lines))
            obj = out / 'OptionalProviders.o'
            subprocess.run(['xcrun', 'clang', '-target', target.TRIPLE, '-c', str(asm), '-o', str(obj)], check=True)
            target.check_object(obj.read_bytes(), asm.name)
            objects.append(obj)
            report['objects'].append({'source': str(asm), 'object': str(obj),
                                      'null_optional_providers': absent, 'target_check': 'PASS'})
        report['absent_optional_providers'] = absent
        archive = out / 'libDitingUfs27.a'
        subprocess.run(['xcrun', 'libtool', '-static', '-o', str(archive), *map(str, objects)], check=True)
        imports = symbols(archive, '-u')
        optional = optional_imports(archive)
        exports = symbols(archive, '-gU')
        pending = sorted(imports - exports - native_exports - optional)
        report.update(archive_built=True, archive=str(archive),
                      pending_imports=pending,
                      required_imports=sorted(imports - exports - optional),
                      optional_imports=sorted(optional - exports - native_exports),
                      native_imports=sorted((imports - exports) & native_exports),
                      exports_count=len(exports))
    except Exception as error:
        report.update(archive_built=False, error=str(error))
        raise
    finally:
        report_path.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: value for key, value in report.items() if key != 'native_imports'}, indent=2))


if __name__ == '__main__':
    main()
