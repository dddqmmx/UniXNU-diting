/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "nw_wifi.h"
#include "nw_mac80211.h"
#include <net/mac80211.h>
#include "../drivers/DitingWifi/DitingWifiChannels.h"
static unsigned checks;
static struct ieee80211_hw *registered;
#define CHECK(x) do { checks++; if (!(x)) { printf("line %d: %s\n", __LINE__, #x); abort(); } } while (0)
struct ieee80211_hw *nw_wifi_hw(void) { return registered; }

int main(void)
{
    struct DitingWifiChannelInfo out[8];
    unsigned frequencies[8];
    CHECK(nw_wifi_radio_count() == 0 && nw_wifi_channel_info(out, 8) == 0);
    struct ieee80211_hw hw = {0}; registered = &hw;
    CHECK(nw_wifi_radio_count() == 0 && nw_wifi_channel_list(frequencies, 8) == 0);
    struct wiphy *wiphy = calloc(1, sizeof(*wiphy)); CHECK(wiphy != NULL); hw.wiphy = wiphy;
    struct ieee80211_channel two[] = {
        {.center_freq = 2412, .flags = IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_NO_HT40MINUS},
        {.center_freq = 2437, .flags = IEEE80211_CHAN_DISABLED},
        {.center_freq = 2484, .flags = IEEE80211_CHAN_NO_OFDM},
    };
    struct ieee80211_channel five[] = {
        {.center_freq = 5180, .flags = IEEE80211_CHAN_NO_HT40PLUS},
        {.center_freq = 5260, .flags = IEEE80211_CHAN_RADAR | IEEE80211_CHAN_INDOOR_ONLY | IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_NO_80MHZ},
    };
    struct ieee80211_channel six[] = {{.center_freq = 5955}};
    struct ieee80211_supported_band bands[3] = {
        {.channels = two, .n_channels = 3}, {.channels = five, .n_channels = 2}, {.channels = six, .n_channels = 1}
    };
    for (unsigned i = 0; i < 3; i++) {
        bands[i].ht_cap.ht_supported = true; bands[i].ht_cap.cap = IEEE80211_HT_CAP_SUP_WIDTH_20_40;
        bands[i].vht_cap.vht_supported = true;
    }
    wiphy->bands[NL80211_BAND_2GHZ] = &bands[0]; wiphy->bands[NL80211_BAND_5GHZ] = &bands[1]; wiphy->bands[NL80211_BAND_6GHZ] = &bands[2];
    CHECK(nw_wifi_radio_count() == 1);
    memset(out, 0xa5, sizeof(out)); memset(frequencies, 0xa5, sizeof(frequencies));
    CHECK(nw_wifi_channel_info(out, 8) == 4 && nw_wifi_channel_list(frequencies, 8) == 4);
    const unsigned expected[] = {2412, 2484, 5180, 5260};
    for (unsigned i = 0; i < 4; i++) CHECK(out[i].frequency_mhz == expected[i] && frequencies[i] == expected[i]);
    CHECK(out[0].flags == (DITING_CHANNEL_PASSIVE | DITING_CHANNEL_HT40_ABOVE | DITING_CHANNEL_VHT80));
    CHECK(out[1].flags == 0);
    CHECK(out[2].flags == (DITING_CHANNEL_HT40_BELOW | DITING_CHANNEL_VHT80));
    CHECK(out[3].flags == (DITING_CHANNEL_DFS | DITING_CHANNEL_INDOOR | DITING_CHANNEL_PASSIVE | DITING_CHANNEL_HT40_ABOVE | DITING_CHANNEL_HT40_BELOW));
    CHECK(out[4].frequency_mhz == 0xa5a5a5a5 && frequencies[4] == 0xa5a5a5a5);
    memset(out, 0xa5, sizeof(out));
    CHECK(nw_wifi_channel_info(out, 2) == 2 && out[2].frequency_mhz == 0xa5a5a5a5);
    CHECK(nw_wifi_channel_info(out, 0) == 0 && nw_wifi_channel_list(frequencies, 0) == 0);
    CHECK(nw_wifi_channel_info(NULL, 8) == 0 && nw_wifi_channel_list(NULL, 8) == 0);
    bands[0].ht_cap.ht_supported = false; bands[0].vht_cap.vht_supported = false;
    bands[1].ht_cap.cap = 0; bands[1].vht_cap.vht_supported = false;
    CHECK(nw_wifi_channel_info(out, 8) == 4);
    CHECK(out[0].flags == DITING_CHANNEL_PASSIVE && out[2].flags == 0);
    CHECK(out[3].flags == (DITING_CHANNEL_PASSIVE | DITING_CHANNEL_DFS | DITING_CHANNEL_INDOOR));
    bands[1].channels = NULL; CHECK(nw_wifi_channel_info(out, 8) == 2);
    wiphy->bands[NL80211_BAND_2GHZ] = NULL; CHECK(nw_wifi_channel_info(out, 8) == 0);
    registered = NULL; CHECK(nw_wifi_radio_count() == 0);
    free(wiphy);
    printf("PASS: %u actual wiphy channel metadata checks, ASan/UBSan\n", checks);
    return 0;
}
