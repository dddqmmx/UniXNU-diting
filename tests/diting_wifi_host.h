/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * libc declarations for a host fixture compiled with the wireless shim's
 * headers. Mixing glibc/Linux kernel typedefs would change that build ABI.
 */
#include <stddef.h>
int printf(const char *, ...);
void *malloc(size_t);
void *calloc(size_t, size_t);
void free(void *);
char *getenv(const char *);
void abort(void) __attribute__((noreturn));
