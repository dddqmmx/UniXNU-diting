/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef DITING_PLATFORM_RESOURCES_H
#define DITING_PLATFORM_RESOURCES_H
#include <stdbool.h>
#include <stdint.h>
#include "DitingRpmhAggregator.h"
#ifdef __cplusplus
extern "C" {
#endif
void diting_rpmh_init(void);
bool diting_scm_call_lock(void);
void diting_scm_call_unlock(void);
bool diting_rpmh_vote(uint32_t address, uint32_t value);
/* One atomic active-only request under the APSS controller's shared lock. */
bool diting_rpmh_vote_many(const uint32_t *addresses, const uint32_t *values, unsigned count);
/* Consumer-aggregated active-only RPMh voting (max across Floor/GPU/ADSP/WLAN/USB). */
bool diting_rpmh_consumer_vote(DitingRpmhConsumer consumer, uint32_t address, uint32_t value);
bool diting_rpmh_consumer_vote_many(DitingRpmhConsumer consumer, const uint32_t *addresses,
                                    const uint32_t *values, unsigned count);
uint32_t diting_rpmh_consumer_active(uint32_t address);
/* Serialize the APSS GCC 0x62008 vote word across touch/WLAN/USB/audio.
 * Only the caller's declared bits change; false includes failed readback. */
bool diting_gcc_vote_update(uint32_t set_mask, uint32_t clear_mask);
/* Shared AOSS link/channel/message owner. 64-byte CNSS or 100-byte ADSP
 * messages are zero-padded. Timeout retains the unconsumed mailbox. */
bool diting_aop_send(const char *message, unsigned wire_bytes);
bool diting_smmu_lease_lock(void);
void diting_smmu_lease_unlock(void);
/* Lightweight cross-driver power coordination (display power & touch boost). */
void diting_power_set_display_on(int on);
int diting_power_display_on(void);
void diting_power_note_touch(void);
bool diting_power_touch_recent(uint64_t window_us);
#ifdef __cplusplus
}
#endif
#endif
