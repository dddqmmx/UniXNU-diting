/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef DITING_WIFI_CONTROL_DIAGNOSTIC_H
#define DITING_WIFI_CONTROL_DIAGNOSTIC_H
#include <stdint.h>

/* Bounded RAM history readable through the existing managed-RAM diagnostic.
 * No payload, SSID, address, key or password is stored here. */
enum { DITING_WIFI_CONTROL_NATIVE = 1, DITING_WIFI_CONTROL_SOCKET = 2,
       DITING_WIFI_CONTROL_EVENT = 3, DITING_WIFI_CONTROL_LINK = 4 };
struct DitingWifiControlRecord {
    uint32_t sequence, path, selector;
    int32_t type, result;
    uint32_t input_bytes, output_bytes, flags; /* SET=1, power on=2, failed=4 */
};
struct DitingWifiControlDiagnostic {
    uint32_t version, byte_size, revision, total_records;
    uint32_t native_requests, socket_requests, failed_records, event_posts;
    uint32_t selectors[8], native_types[8], socket_types[8];
    struct DitingWifiControlRecord first[32], recent[32], failures[16];
};
#ifdef __cplusplus
static_assert(sizeof(DitingWifiControlRecord) == 32 &&
              __builtin_offsetof(DitingWifiControlDiagnostic, first) == 128 &&
              sizeof(DitingWifiControlDiagnostic) == 2688, "Wi-Fi diagnostic ABI");
extern "C" {
#endif
extern struct DitingWifiControlDiagnostic diting_wifi_control_diagnostic;
#ifdef __cplusplus
}
#endif
#endif
