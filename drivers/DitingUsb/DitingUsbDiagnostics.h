/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef DITING_USB_DIAGNOSTICS_H
#define DITING_USB_DIAGNOSTICS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Text uses cached, explicitly qualified hardware reads; no new MMIO. */
unsigned diting_usb_snapshot(char *out, unsigned capacity);
/* Development kernel: no-fault copy of at most 512 bytes of mapped kernel RAM. */
unsigned diting_usb_read_kernel(uint64_t address, void *out, unsigned bytes);
#ifdef __cplusplus
}
#endif
#endif
