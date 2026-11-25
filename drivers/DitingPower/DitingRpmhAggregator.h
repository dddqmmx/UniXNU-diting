/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Allocation-free shared RPMh consumer aggregator for Diting APSS DRV2.
 * Each subsystem (Floor, GPU, ADSP, WLAN, USB) submits its own corner/level
 * requirement per RPMh resource address; the aggregator computes max() across
 * all active consumers and emits hardware RPMh writes only when the aggregate
 * changes, committing state strictly after hardware acknowledgment. */
#ifndef DITING_RPMH_AGGREGATOR_H
#define DITING_RPMH_AGGREGATOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DitingRpmhFloor = 0,
    DitingRpmhGpu = 1,
    DitingRpmhAdsp = 2,
    DitingRpmhWlan = 3,
    DitingRpmhUsb = 4,
    DitingRpmhConsumerCount = 5,
} DitingRpmhConsumer;

#define DITING_RPMH_AGG_MAX_RESOURCES 16u
#define DITING_RPMH_AGG_MAX_BATCH 4u

typedef struct {
    uint32_t address;
    uint32_t votes[DitingRpmhConsumerCount];
    uint32_t active;
    uint8_t used;
    uint8_t committed;
} DitingRpmhAggEntry;

typedef struct {
    DitingRpmhAggEntry entries[DITING_RPMH_AGG_MAX_RESOURCES];
    uint32_t updates;
    uint32_t hw_writes;
    uint32_t skipped;
} DitingRpmhAggregator;

typedef struct {
    int slot[DITING_RPMH_AGG_MAX_BATCH];
    uint32_t consumer_value[DITING_RPMH_AGG_MAX_BATCH];
    uint32_t next_aggregate[DITING_RPMH_AGG_MAX_BATCH];
    uint32_t hw_addresses[DITING_RPMH_AGG_MAX_BATCH];
    uint32_t hw_values[DITING_RPMH_AGG_MAX_BATCH];
    unsigned count;
    unsigned hw_count;
    DitingRpmhConsumer consumer;
} DitingRpmhAggPlan;

static inline void diting_rpmh_agg_init(DitingRpmhAggregator *agg)
{
    if (!agg) return;
    for (unsigned i = 0; i < sizeof *agg; ++i) ((uint8_t *)agg)[i] = 0;
}

static inline int diting_rpmh_agg_find_or_alloc(DitingRpmhAggregator *agg, uint32_t address, bool allocate)
{
    int free_slot = -1;
    for (unsigned i = 0; i < DITING_RPMH_AGG_MAX_RESOURCES; ++i) {
        if (agg->entries[i].used) {
            if (agg->entries[i].address == address) return (int)i;
        } else if (free_slot < 0) {
            free_slot = (int)i;
        }
    }
    if (!allocate || free_slot < 0) return -1;
    agg->entries[free_slot].address = address;
    for (unsigned c = 0; c < DitingRpmhConsumerCount; ++c) agg->entries[free_slot].votes[c] = 0;
    agg->entries[free_slot].active = 0;
    agg->entries[free_slot].used = 1;
    agg->entries[free_slot].committed = 0;
    return free_slot;
}

/* Prepares an atomic consumer vote batch without mutating committed votes.
 * Duplicate addresses inside a single batch are rejected. Returns false if
 * arguments are invalid or the resource table is full. */
static inline bool diting_rpmh_agg_prepare(DitingRpmhAggregator *agg, DitingRpmhConsumer consumer,
                                           const uint32_t *addresses, const uint32_t *values,
                                           unsigned count, DitingRpmhAggPlan *plan)
{
    if (!agg || !plan || !addresses || !values || !count || count > DITING_RPMH_AGG_MAX_BATCH ||
        (unsigned)consumer >= (unsigned)DitingRpmhConsumerCount) {
        return false;
    }
    for (unsigned i = 0; i < count; ++i) {
        if (!addresses[i]) return false;
        for (unsigned j = 0; j < i; ++j)
            if (addresses[j] == addresses[i]) return false;
    }
    plan->count = count;
    plan->hw_count = 0;
    plan->consumer = consumer;

    /* Check table capacity before allocating any new slots. */
    unsigned needed = 0;
    unsigned free_slots = 0;
    for (unsigned i = 0; i < DITING_RPMH_AGG_MAX_RESOURCES; ++i)
        if (!agg->entries[i].used) free_slots++;
    for (unsigned i = 0; i < count; ++i)
        if (diting_rpmh_agg_find_or_alloc(agg, addresses[i], false) < 0) needed++;
    if (needed > free_slots) return false;

    for (unsigned i = 0; i < count; ++i) {
        const int idx = diting_rpmh_agg_find_or_alloc(agg, addresses[i], true);
        if (idx < 0) return false;
        const DitingRpmhAggEntry *e = &agg->entries[idx];
        uint32_t agg_val = values[i];
        for (unsigned c = 0; c < (unsigned)DitingRpmhConsumerCount; ++c) {
            if (c == (unsigned)consumer) continue;
            if (e->votes[c] > agg_val) agg_val = e->votes[c];
        }
        plan->slot[i] = idx;
        plan->consumer_value[i] = values[i];
        plan->next_aggregate[i] = agg_val;
        if (!e->committed || agg_val != e->active) {
            plan->hw_addresses[plan->hw_count] = addresses[i];
            plan->hw_values[plan->hw_count] = agg_val;
            plan->hw_count++;
        }
    }
    return true;
}

/* Commits a prepared plan after hardware RPMh submission succeeded (or had
 * hw_count == 0). Never call if hardware submission failed. */
static inline void diting_rpmh_agg_commit(DitingRpmhAggregator *agg, const DitingRpmhAggPlan *plan)
{
    if (!agg || !plan) return;
    agg->updates++;
    if (plan->hw_count) agg->hw_writes++;
    else agg->skipped++;
    for (unsigned i = 0; i < plan->count; ++i) {
        DitingRpmhAggEntry *e = &agg->entries[plan->slot[i]];
        e->votes[plan->consumer] = plan->consumer_value[i];
        e->active = plan->next_aggregate[i];
        e->committed = 1;
    }
}

/* Returns the currently committed aggregate for address, or 0 if untracked. */
static inline uint32_t diting_rpmh_agg_active(const DitingRpmhAggregator *agg, uint32_t address)
{
    if (!agg || !address) return 0;
    for (unsigned i = 0; i < DITING_RPMH_AGG_MAX_RESOURCES; ++i)
        if (agg->entries[i].used && agg->entries[i].committed && agg->entries[i].address == address)
            return agg->entries[i].active;
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif
