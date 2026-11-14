/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "../DitingWifiBdf.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    assert(diting_wifi_pa(0, 0) == DitingWifiPaUnknown);
    assert(diting_wifi_pa(0, UINT64_MAX) == DitingWifiPaUnknown);
    assert(diting_wifi_pa(1, 1) == DitingWifiPaExternal);
    assert(diting_wifi_pa(1, 3) == DitingWifiPaExternal);
    assert(diting_wifi_pa(1, 0) == DitingWifiPaInternal);
    assert(diting_wifi_pa(1, 2) == DitingWifiPaInternal);
    assert(diting_wifi_pa(1, UINT64_C(1) << 40) == DitingWifiPaInternal);
    puts("PASS: absent, external and internal PA flags");
}
