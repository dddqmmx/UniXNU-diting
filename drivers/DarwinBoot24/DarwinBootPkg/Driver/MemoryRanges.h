// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef DARWINBOOT_MEMORY_RANGES_H
#define DARWINBOOT_MEMORY_RANGES_H

static inline int DarwinRamRangeValid(unsigned long long base, unsigned long long size) {
  return size && base <= ~0ULL - size && !((base | size) & 0xfffULL);
}

static inline int DarwinRamDescriptorEnd(unsigned long long base, unsigned long long pages,
                                         unsigned long long *end) {
  if (!pages || (base & 0xfffULL) || pages > (~0ULL - base) / 4096ULL) { return 0; }
  *end = base + pages * 4096ULL;
  return 1;
}

// Preserve the Mach-O's offset within a 2 MiB block, without rounding past
// the first suitable page or overflowing the end of either input range.
// A framebuffer mistakenly described as conventional RAM remains excluded.
static inline int DarwinRamWindowCandidate(unsigned long long start, unsigned long long end,
                                           unsigned long long kernel, unsigned long long size,
                                           unsigned long long video, unsigned long long videoBytes,
                                           unsigned long long *candidate) {
  unsigned long long address, videoEnd = 0;
  unsigned int attempt;
  *candidate = 0;
  if (start >= end || !DarwinRamRangeValid(start, end - start) ||
      !size || (size & 0xfffULL) || (kernel & 0xfffULL)) { return 0; }
  if (videoBytes) {
    if (video > ~0ULL - videoBytes) { return 0; }
    videoEnd = video + videoBytes;
  }
  for (attempt = 0; attempt < 2; attempt++) {
    address = (start & ~0x1fffffULL) + (kernel & 0x1fffffULL);
    if (address < start) {
      if (address > ~0ULL - 0x200000ULL) { return 0; }
      address += 0x200000ULL;
    }
    if (address > end || size > end - address) { return 0; }
    if (!videoBytes || address >= videoEnd || video >= address + size) {
      *candidate = address;
      return 1;
    }
    if (videoEnd > ~0ULL - 0xfffULL) { return 0; }
    start = (videoEnd + 0xfffULL) & ~0xfffULL;
    if (start >= end) { return 0; }
  }
  return 0;
}
#endif
