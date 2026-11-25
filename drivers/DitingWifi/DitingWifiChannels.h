/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef DITING_WIFI_CHANNELS_H
#define DITING_WIFI_CHANNELS_H
#include <stdint.h>

/* Diting's owned extension to the pinned station ABI. These are restrictions
 * and widths from wiphy, not Apple's opaque per-channel bitmap. */
enum {
  DITING_CHANNEL_PASSIVE = 1u << 0,
  DITING_CHANNEL_DFS = 1u << 1,
  DITING_CHANNEL_INDOOR = 1u << 2,
  DITING_CHANNEL_HT40_ABOVE = 1u << 3,
  DITING_CHANNEL_HT40_BELOW = 1u << 4,
  DITING_CHANNEL_VHT80 = 1u << 5,
};
struct DitingWifiChannelInfo { uint32_t frequency_mhz, flags; };
#ifdef __cplusplus
extern "C" {
#endif
unsigned nw_wifi_channel_info(struct DitingWifiChannelInfo *, unsigned capacity);
unsigned nw_wifi_channel_list(unsigned *, unsigned capacity);
unsigned nw_wifi_radio_count(void);
#ifdef __cplusplus
}
#endif
#endif
