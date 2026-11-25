/* SPDX-License-Identifier: MIT
 * Bounded read-only ABI for completed native keyboard calls. Published only
 * by the requested keyboard diagnostic; no text or key contents are stored.
 */
#ifndef DITING_KEYBOARD_NATIVE_H
#define DITING_KEYBOARD_NATIVE_H
#include <stdint.h>
enum { DitingKeyboardNativeMagic=0x4b544944, DitingKeyboardNativeMethods=12 };
struct DitingKeyboardNativeCall {
    uint64_t calls, total_ns, max_ns, last_start_ns, last_end_ns;
};
struct DitingKeyboardNativeDiagnostic {
    uint32_t magic, version, bytes, methods, writers, epoch;
    struct DitingKeyboardNativeCall calls[DitingKeyboardNativeMethods];
};
#ifdef __cplusplus
static_assert(sizeof(DitingKeyboardNativeDiagnostic)==504,"keyboard native read ABI");
#else
_Static_assert(sizeof(struct DitingKeyboardNativeDiagnostic)==504,"keyboard native read ABI");
#endif
#endif
