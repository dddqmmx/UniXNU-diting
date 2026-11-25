// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef DITING_PLATFORM_IDENTITY_H
#define DITING_PLATFORM_IDENTITY_H
#include <stddef.h>

// A board-supplied software UUID, separate from Apple's chip/ECID properties.
// The ADT value has a fixed extent; never scan beyond its terminator.
static inline bool diting_platform_identity_copy(const void *data, size_t bytes,
                                                char output[37])
{
  if (!data || !output || bytes != 37) return false;
  const unsigned char *text = static_cast<const unsigned char *>(data);
  if (text[36] != 0) return false;
  bool nonzero = false;
  for (unsigned i = 0; i < 36; ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (text[i] != '-') return false;
    } else {
      if (!((text[i] >= '0' && text[i] <= '9') ||
            (text[i] >= 'a' && text[i] <= 'f') ||
            (text[i] >= 'A' && text[i] <= 'F'))) return false;
      if (text[i] != '0') nonzero = true;
    }
  }
  if (!nonzero) return false;
  for (unsigned i = 0; i < 37; ++i) output[i] = static_cast<char>(text[i]);
  return true;
}
#endif
