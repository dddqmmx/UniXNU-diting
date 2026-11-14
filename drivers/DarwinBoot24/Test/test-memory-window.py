#!/usr/bin/env python3
"""Exercise the actual primary allocator with host EFI map/allocation mocks."""
import argparse
from pathlib import Path
import re
import subprocess

DRIVER = Path(__file__).resolve().parents[1] / 'DarwinBootPkg/Driver'


def function(source, name):
    match = re.search(r'\nSTATIC\n[^\n]+\n' + re.escape(name) + r'\s*\(', source)
    if not match:
        raise ValueError(f'{name}: actual allocator function not found')
    begin = source.index('{', match.end())
    depth = 1
    end = begin + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


PREAMBLE = r'''
#include <Uefi.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "MemoryRanges.h"
#define DEBUG(Args) ((void)0)
#define BLOCK_SIZE 0x200000ULL
#define DITING_RAM_MAX 64
typedef struct { UINT64 Base; } KERNEL;
static UINTN mRamWireOffset, mRamAllowedOffset, mRamAllowedCount;
static UINTN mRamPhysicalOffset, mRamPhysicalCount;
static EFI_MEMORY_DESCRIPTOR Input[4];
static UINTN InputCount, InputStride, ExtraBytes, Allocations, Releases, MapReads;
static EFI_STATUS AllocationStatus;
static int ChangeAddress;
static UINT8 Tree[1024];
static UINT64 U64(CONST VOID *P) { UINT64 V; memcpy(&V, P, 8); return V; }
static VOID FreePool(VOID *P) { free(P); }
static UINTN UnicodeSPrint(CHAR16 *S, UINTN N, CONST CHAR16 *F, ...) {
  (VOID)S; (VOID)N; (VOID)F; return 0;
}
static VOID DiagDraw(CONST CHAR16 *S, UINTN Row) { (VOID)S; (VOID)Row; }
static EFI_STATUS GetMap(EFI_MEMORY_DESCRIPTOR **Map, UINTN *Capacity, UINTN *Size,
                         UINTN *Key, UINTN *Stride) {
  MapReads++;
  *Map = malloc(sizeof(Input));
  assert(*Map);
  memcpy(*Map, Input, sizeof(Input));
  *Capacity = sizeof(Input); *Size = InputCount * InputStride + ExtraBytes;
  *Key = 1; *Stride = InputStride;
  return EFI_SUCCESS;
}
static EFI_STATUS Allocate(EFI_ALLOCATE_TYPE Kind, EFI_MEMORY_TYPE Type, UINTN Pages,
                            EFI_PHYSICAL_ADDRESS *Address) {
  assert(Kind == AllocateAddress && Type == EfiLoaderCode && Pages == 0x100000);
  Allocations++;
  if (ChangeAddress) { *Address += 0x200000; }
  return AllocationStatus;
}
static EFI_STATUS Release(EFI_PHYSICAL_ADDRESS Address, UINTN Pages) {
  assert(Address && Pages == 0x100000); Releases++; return EFI_SUCCESS;
}
static struct { EFI_STATUS (*AllocatePages)(EFI_ALLOCATE_TYPE, EFI_MEMORY_TYPE, UINTN, EFI_PHYSICAL_ADDRESS *);
                EFI_STATUS (*FreePages)(EFI_PHYSICAL_ADDRESS, UINTN); } BootServices = { Allocate, Release };
static __typeof__(BootServices) *gBS = &BootServices;
static VOID Wire(UINTN Offset, UINT64 Base, UINT64 Size) {
  memcpy(Tree + Offset, &Base, 8); memcpy(Tree + Offset + 8, &Size, 8);
}
static VOID Descriptor(UINTN Index, UINT64 Base, UINT64 Bytes, EFI_MEMORY_TYPE Type, UINT64 Attributes) {
  Input[Index] = (EFI_MEMORY_DESCRIPTOR){ .Type=Type, .PhysicalStart=Base,
                                         .NumberOfPages=Bytes / 4096, .Attribute=Attributes };
}
static VOID Reset(VOID) {
  memset(Tree, 0, sizeof(Tree)); memset(Input, 0, sizeof(Input));
  mRamPhysicalOffset=0; mRamPhysicalCount=1;
  mRamAllowedOffset=256; mRamAllowedCount=1; mRamWireOffset=512;
  Wire(0, 0x840000000ULL, 0x140000000ULL);
  Wire(256, 0x840000000ULL, 0x140000000ULL);
  Descriptor(0, 0x840000000ULL, 0x140000000ULL, EfiConventionalMemory, EFI_MEMORY_WB);
  InputCount=1; InputStride=sizeof(EFI_MEMORY_DESCRIPTOR); ExtraBytes=0;
  Allocations=Releases=MapReads=0; AllocationStatus=EFI_SUCCESS; ChangeAddress=0;
}
'''

CASES = r'''
static UINTN Cases;
static VOID Expect(EFI_STATUS Wanted, UINT64 AddressWanted, UINTN Calls,
                   UINT64 VideoBase, UINT64 VideoBytes) {
  KERNEL Kernel = { .Base=0xfffffff000004000ULL };
  UINT64 Video[6] = { VideoBase, 0, VideoBytes, 0, VideoBytes ? 1 : 0, 32 };
  EFI_PHYSICAL_ADDRESS Address=0;
  EFI_STATUS Status=AllocateWindow(Tree, &Kernel, 0x100000000ULL, Video, &Address);
  if (Status != Wanted || Address != AddressWanted || Allocations != Calls) {
    fprintf(stderr, "case %llu: status=%llx address=%llx calls=%llu\n",
            (unsigned long long)Cases, (unsigned long long)Status,
            (unsigned long long)Address, (unsigned long long)Allocations);
    abort();
  }
  Cases++;
}
int main(void) {
  // The actual bank fits 4 GiB and preserves the Mach-O's block offset.
  Reset(); Expect(EFI_SUCCESS, 0x840004000ULL, 1, 0, 0);
  // The old firmware's 5 GiB declaration cannot create RAM above 0x980000000.
  Reset(); Descriptor(0, 0x900000000ULL, 0x140000000ULL, EfiConventionalMemory, EFI_MEMORY_WB);
  Expect(EFI_OUT_OF_RESOURCES, 0, 0, 0, 0);
  // A stale allocatable whitelist itself fails before the first EFI map read.
  Reset(); Wire(256, 0x900000000ULL, 0x140000000ULL);
  Expect(EFI_COMPROMISED_DATA, 0, 0, 0, 0); assert(MapReads == 0);
  // Adjacent WB descriptors form one run; an uncached or firmware-owned piece cannot.
  Reset(); InputCount=2;
  Descriptor(0, 0x840000000ULL, 0x80000000ULL, EfiConventionalMemory, EFI_MEMORY_WB);
  Descriptor(1, 0x8c0000000ULL, 0xc0000000ULL, EfiConventionalMemory, EFI_MEMORY_WB);
  Expect(EFI_SUCCESS, 0x840004000ULL, 1, 0, 0);
  Reset(); InputCount=2;
  Descriptor(0, 0x840000000ULL, 0x80000000ULL, EfiConventionalMemory, EFI_MEMORY_WB);
  Descriptor(1, 0x8c0000000ULL, 0xc0000000ULL, EfiConventionalMemory, EFI_MEMORY_UC);
  Expect(EFI_OUT_OF_RESOURCES, 0, 0, 0, 0);
  Input[1].Type=EfiRuntimeServicesData; Input[1].Attribute=EFI_MEMORY_WB;
  Expect(EFI_OUT_OF_RESOURCES, 0, 0, 0, 0);
  // Contradictory EFI descriptors, malformed stride/size and physical overflow fail closed.
  Reset(); InputCount=2;
  Descriptor(1, 0x8c0000000ULL, 0x200000ULL, EfiReservedMemoryType, EFI_MEMORY_WB);
  Expect(EFI_COMPROMISED_DATA, 0, 0, 0, 0);
  Reset(); InputStride=8; Expect(EFI_COMPROMISED_DATA, 0, 0, 0, 0);
  Reset(); ExtraBytes=1; Expect(EFI_COMPROMISED_DATA, 0, 0, 0, 0);
  Reset(); Descriptor(0, 0xfffffffffffff000ULL, 0x2000, EfiConventionalMemory, EFI_MEMORY_WB);
  Expect(EFI_COMPROMISED_DATA, 0, 0, 0, 0);
  Reset(); mRamPhysicalCount=2; Wire(16, 0x900000000ULL, 0x10000000);
  Expect(EFI_COMPROMISED_DATA, 0, 0, 0, 0);
  Reset(); mRamAllowedCount=2; Wire(272, 0x900000000ULL, 0x10000000);
  Expect(EFI_COMPROMISED_DATA, 0, 0, 0, 0);
  Reset(); mRamPhysicalOffset=MAX_UINTN; Expect(EFI_COMPROMISED_DATA, 0, 0, 0, 0);
  Reset(); Input[0].NumberOfPages=0; Expect(EFI_OUT_OF_RESOURCES, 0, 0, 0, 0);
  // Framebuffer pages stay outside the window even if firmware labels them free.
  Reset(); Expect(EFI_SUCCESS, 0x840204000ULL, 1, 0x840005000ULL, 0x10000);
  Reset(); Expect(EFI_OUT_OF_RESOURCES, 0, 0, 0x880000000ULL, 0x800000);
  Reset(); Expect(EFI_COMPROMISED_DATA, 0, 0, 0xfffffffffffff000ULL, 0x2000);
  // A failed lease is never published; AllocateAddress must return the requested address.
  Reset(); AllocationStatus=EFI_OUT_OF_RESOURCES; Expect(EFI_OUT_OF_RESOURCES, 0, 1, 0, 0);
  Reset(); ChangeAddress=1; Expect(EFI_COMPROMISED_DATA, 0, 1, 0, 0); assert(Releases == 1);
  puts("PASS: 19 cases against the actual primary allocator; no device or image access");
  return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--edk2', required=True, type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--cc', default='clang')
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    source = (DRIVER / 'DarwinBoot.c').read_text()
    names = ['ValidateRamList', 'ValidateDitingRam', 'ValidateMemoryMap',
             'UsableRamDescriptor', 'RunEnd', 'AllocateWindow']
    harness = args.output_dir / 'memory-window.c'
    harness.write_text(PREAMBLE + '\n'.join(function(source, name) for name in names) + CASES)
    binary = args.output_dir / 'memory-window'
    subprocess.run([args.cc, '-std=gnu11', '-fshort-wchar', '-Wall', '-Wextra', '-Werror',
                    '-I', str(args.edk2 / 'MdePkg/Include'),
                    '-I', str(args.edk2 / 'MdePkg/Include/X64'), '-I', str(DRIVER),
                    str(harness), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
