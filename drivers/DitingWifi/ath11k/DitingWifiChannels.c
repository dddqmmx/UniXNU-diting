/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "nw_wifi.h"
#include "nw_mac80211.h"
#include <net/mac80211.h>
#include "../DitingWifiChannels.h"

unsigned nw_wifi_radio_count(void)
{
    /* The shared station registers one physical ieee80211_hw on this board. */
    struct ieee80211_hw *hw = nw_wifi_hw();
    return hw && hw->wiphy ? 1 : 0;
}

static uint32_t channel_flags(const struct ieee80211_channel *c,
                             const struct ieee80211_supported_band *band)
{
    uint32_t flags = 0;
    if (c->flags & IEEE80211_CHAN_NO_IR) flags |= DITING_CHANNEL_PASSIVE;
    if (c->flags & IEEE80211_CHAN_RADAR) flags |= DITING_CHANNEL_DFS;
    if (c->flags & IEEE80211_CHAN_INDOOR_ONLY) flags |= DITING_CHANNEL_INDOOR;
    if (band->ht_cap.ht_supported &&
        (band->ht_cap.cap & IEEE80211_HT_CAP_SUP_WIDTH_20_40) &&
        !(c->flags & IEEE80211_CHAN_NO_OFDM)) {
        if (!(c->flags & IEEE80211_CHAN_NO_HT40PLUS)) flags |= DITING_CHANNEL_HT40_ABOVE;
        if (!(c->flags & IEEE80211_CHAN_NO_HT40MINUS)) flags |= DITING_CHANNEL_HT40_BELOW;
    }
    if (band->vht_cap.vht_supported &&
        !(c->flags & (IEEE80211_CHAN_NO_80MHZ | IEEE80211_CHAN_NO_OFDM)))
        flags |= DITING_CHANNEL_VHT80;
    return flags;
}

static unsigned channels(struct DitingWifiChannelInfo *info, unsigned *frequencies,
                         unsigned capacity)
{
    struct ieee80211_hw *hw = nw_wifi_hw();
    unsigned n = 0;
    if (!hw || !hw->wiphy || (!info && !frequencies)) return 0;
    for (unsigned b = 0; b < NUM_NL80211_BANDS; b++) {
        /* Match Diting's current station/scan policy. No 6 GHz/SAE claim. */
        if (b != NL80211_BAND_2GHZ && b != NL80211_BAND_5GHZ) continue;
        const struct ieee80211_supported_band *band = hw->wiphy->bands[b];
        if (!band || !band->channels) continue;
        for (int i = 0; i < band->n_channels && n < capacity; i++) {
            const struct ieee80211_channel *c = &band->channels[i];
            if (c->flags & IEEE80211_CHAN_DISABLED) continue;
            if (info) {
                info[n].frequency_mhz = c->center_freq;
                info[n].flags = channel_flags(c, band);
            }
            if (frequencies) frequencies[n] = c->center_freq;
            n++;
        }
    }
    return n;
}

unsigned nw_wifi_channel_info(struct DitingWifiChannelInfo *out, unsigned capacity)
{ return channels(out, NULL, capacity); }

unsigned nw_wifi_channel_list(unsigned *out, unsigned capacity)
{ return channels(NULL, out, capacity); }
