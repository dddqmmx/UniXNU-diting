/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <stdarg.h>

// Clamp the stored length, including when vsnprintf reports a larger required
// length. The caller reserves room for the mandatory END command frame.
static inline void diting_usb_reply_add(char *out, unsigned capacity, unsigned *used,
    const char *format, ...) {
    if (!out || !used || *used >= capacity || capacity - *used <= 1) return;
    va_list args; va_start(args, format);
    const int n = vsnprintf(out + *used, capacity - *used, format, args);
    va_end(args);
    if (n > 0) *used += unsigned(n) < capacity - *used ? unsigned(n) : capacity - *used - 1;
}
