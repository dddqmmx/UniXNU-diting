/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef DITING_PLATFORM_RESOURCES_H
#define DITING_PLATFORM_RESOURCES_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void diting_rpmh_init(void);
bool diting_scm_call_lock(void);
void diting_scm_call_unlock(void);
bool diting_rpmh_vote(uint32_t address, uint32_t value);
/* One atomic active-only request under the APSS controller's shared lock. */
bool diting_rpmh_vote_many(const uint32_t *addresses, const uint32_t *values, unsigned count);
/* Serialize the APSS GCC 0x62008 vote word across touch/WLAN/USB/audio.
 * Only the caller's declared bits change; false includes failed readback. */
bool diting_gcc_vote_update(uint32_t set_mask, uint32_t clear_mask);
/* Shared AOSS link/channel/message owner. 64-byte CNSS or 100-byte ADSP
 * messages are zero-padded. Timeout retains the unconsumed mailbox. */
bool diting_aop_send(const char *message, unsigned wire_bytes);
bool diting_smmu_lease_lock(void);
void diting_smmu_lease_unlock(void);
#ifdef __cplusplus
}
#endif
#endif
