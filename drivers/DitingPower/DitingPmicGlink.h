/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Shared PMIC GLINK ("PMIC_RTR_ADSP_APPS") demultiplexer for Diting.
 * Routes messages by 32-bit PMIC GLINK owner (e.g. 32778 battery charger,
 * 32779 UCSI) while maintaining a monotonic transport epoch across down/up
 * transitions. The pure router helpers below have no IOKit dependencies so
 * host tests can verify multi-owner routing and epoch behavior directly. */
#ifndef DITING_PMIC_GLINK_H
#define DITING_PMIC_GLINK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DITING_PMIC_GLINK_CHANNEL "PMIC_RTR_ADSP_APPS"
#define DITING_PMIC_GLINK_OWNER_BATTMGR 32778u
#define DITING_PMIC_GLINK_OWNER_UCSI 32779u
#define DITING_PMIC_GLINK_MAX_CLIENTS 4u

typedef struct {
    void (*state)(void *context, int up);
    void (*receive)(void *context, const uint8_t *data, uint32_t length);
} DitingPmicGlinkClient;

typedef struct {
    uint32_t owner;
    DitingPmicGlinkClient client;
    void *context;
    int active;
} DitingPmicGlinkSlot;

typedef struct {
    DitingPmicGlinkSlot slots[DITING_PMIC_GLINK_MAX_CLIENTS];
    uint32_t epoch;
    int up;
    uint32_t rx_routed;
    uint32_t rx_unknown_owner;
    uint32_t rx_short;
    uint32_t tx_ok;
    uint32_t tx_err;
} DitingPmicGlinkRouter;

typedef struct {
    uint32_t epoch;
    uint32_t rx_routed;
    uint32_t rx_unknown_owner;
    uint32_t rx_short;
    uint32_t tx_ok;
    uint32_t tx_err;
    int up;
} DitingPmicGlinkStats;

static inline uint32_t diting_pmic_glink_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void diting_pmic_glink_router_init(DitingPmicGlinkRouter *r)
{
    for (unsigned i = 0; i < sizeof *r; ++i) ((uint8_t *)r)[i] = 0;
}

static inline int diting_pmic_glink_router_register(DitingPmicGlinkRouter *r, uint32_t owner,
                                                    const DitingPmicGlinkClient *client, void *context)
{
    if (!r || !owner || !client || (!client->state && !client->receive)) return -1;
    int free_slot = -1;
    for (unsigned i = 0; i < DITING_PMIC_GLINK_MAX_CLIENTS; ++i) {
        if (r->slots[i].active) {
            if (r->slots[i].owner == owner) return -1;
        } else if (free_slot < 0) {
            free_slot = (int)i;
        }
    }
    if (free_slot < 0) return -1;
    r->slots[free_slot].owner = owner;
    r->slots[free_slot].client = *client;
    r->slots[free_slot].context = context;
    r->slots[free_slot].active = 1;
    return 0;
}

static inline void diting_pmic_glink_router_unregister(DitingPmicGlinkRouter *r, uint32_t owner)
{
    if (!r || !owner) return;
    for (unsigned i = 0; i < DITING_PMIC_GLINK_MAX_CLIENTS; ++i) {
        if (r->slots[i].active && r->slots[i].owner == owner) {
            r->slots[i].active = 0;
            r->slots[i].owner = 0;
            r->slots[i].client.state = 0;
            r->slots[i].client.receive = 0;
            r->slots[i].context = 0;
        }
    }
}

/* Updates link state and monotonic epoch. Returns the number of active slots
 * copied into out_slots (if non-null) so callers can notify outside a lock. */
static inline unsigned diting_pmic_glink_router_state(DitingPmicGlinkRouter *r, int up,
                                                      DitingPmicGlinkSlot *out_slots, unsigned max_slots)
{
    if (!r) return 0;
    const int normalized = up != 0;
    if (!normalized || !r->up) r->epoch++;
    r->up = normalized;
    unsigned n = 0;
    for (unsigned i = 0; i < DITING_PMIC_GLINK_MAX_CLIENTS; ++i) {
        if (!r->slots[i].active) continue;
        if (out_slots && n < max_slots) out_slots[n] = r->slots[i];
        n++;
    }
    return n;
}

/* Matches an incoming PMIC GLINK message to its registered owner slot.
 * Returns 1 if matched (and writes *out_slot), 0 if dropped. */
static inline int diting_pmic_glink_router_lookup(DitingPmicGlinkRouter *r, const uint8_t *data,
                                                  uint32_t length, DitingPmicGlinkSlot *out_slot)
{
    if (!r || !r->up || !data || length < 12u) {
        if (r) r->rx_short++;
        return 0;
    }
    const uint32_t owner = diting_pmic_glink_le32(data);
    for (unsigned i = 0; i < DITING_PMIC_GLINK_MAX_CLIENTS; ++i) {
        if (r->slots[i].active && r->slots[i].owner == owner) {
            r->rx_routed++;
            if (out_slot) *out_slot = r->slots[i];
            return 1;
        }
    }
    r->rx_unknown_owner++;
    return 0;
}

/* Direct single-threaded dispatch convenience helper for host unit tests. */
static inline int diting_pmic_glink_router_dispatch(DitingPmicGlinkRouter *r, const uint8_t *data, uint32_t length)
{
    DitingPmicGlinkSlot slot;
    if (!diting_pmic_glink_router_lookup(r, data, length, &slot)) return 0;
    if (slot.client.receive) slot.client.receive(slot.context, data, length);
    return 1;
}

static inline int diting_pmic_glink_validate_tx(uint32_t owner, const void *data, uint32_t length, uint32_t max_length)
{
    if (!owner || !data || length < 12u || length > max_length) return 0;
    return diting_pmic_glink_le32((const uint8_t *)data) == owner;
}

/* Kernel runtime API (implemented in DitingPmicGlink.cpp). */
int diting_pmic_glink_register(uint32_t owner, const DitingPmicGlinkClient *client, void *context);
int diting_pmic_glink_send(uint32_t owner, const void *data, uint32_t length);
void diting_pmic_glink_stats(DitingPmicGlinkStats *out);

#ifdef __cplusplus
}
#endif

#endif
