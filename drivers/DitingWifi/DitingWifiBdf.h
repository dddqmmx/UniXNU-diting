/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* QCA6490's BDF response, not the existence of a DT supply, selects the PA.
 * Nothing CNSS2 qmi.c / power.c at c402fcdb: HW_XPA is host_bdf_data bit 0.
 * An internal PA is configured through wlan_pdc AFTER the BDF download.
 */
#ifndef DITING_WIFI_BDF_H
#define DITING_WIFI_BDF_H
#include <stdint.h>

enum DitingWifiPa { DitingWifiPaUnknown, DitingWifiPaExternal, DitingWifiPaInternal };
static inline enum DitingWifiPa diting_wifi_pa(unsigned valid, uint64_t host_bdf_data) {
    if (!valid) return DitingWifiPaUnknown;
    return (host_bdf_data & UINT64_C(1)) ? DitingWifiPaExternal : DitingWifiPaInternal;
}

#ifdef __cplusplus
extern "C" {
#endif
/* Called only after the final successful BDF response; errors stop WLAN
 * initialization. No direct APSS enable vote is issued for the IPA rail. */
int diting_wifi_bdf_power_config(unsigned valid, uint64_t host_bdf_data);
#ifdef __cplusplus
}
#endif
#endif
