/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef DITING_BATTERY_STATE_H
#define DITING_BATTERY_STATE_H
#include "ShengBattmgr.h"

/* Shared by the IOKit publisher and host tests. A core snapshot and its
 * optional metadata update are one sample, not two source confirmations. */
typedef struct {
    uint32_t source_cycle;
    int published, present, external, charging, fully_charged;
    int candidate, candidate_valid, source_pending;
} DitingBatteryState;

static inline void diting_battery_new_transport(DitingBatteryState *state)
{
    state->source_cycle = 0;
    state->candidate_valid = state->source_pending = 0;
    /* Preserve the last measured source/status across a transport loss. */
}

static inline int diting_battery_resolve(DitingBatteryState *state, ShengBatterySnapshot *s)
{
    state->source_pending = 0;
    if (!s->charge_known) {
        state->candidate_valid = 0;
        return 0;
    }
    if (state->published) {
        if (!s->present_known) s->present = state->present;
        if (!s->source_known) {
            state->candidate_valid = 0;
            s->external = state->external;
            s->charging = state->charging;
            s->fully_charged = state->fully_charged;
        } else if (s->external != state->external) {
            if (s->sample_cycle == state->source_cycle || !state->candidate_valid || state->candidate != s->external) {
                state->candidate = s->external;
                state->candidate_valid = 1;
                state->source_pending = 1;
                s->external = state->external;
                s->charging = state->charging;
                s->fully_charged = state->fully_charged;
            } else state->candidate_valid = 0;
        } else state->candidate_valid = 0;
        if (!s->status_known) {
            s->charging = state->charging;
            s->fully_charged = state->fully_charged;
        }
    }
    state->source_cycle = s->sample_cycle;
    if (!s->present || !s->external) s->charging = s->fully_charged = 0;
    /* Apply warning policy after source resolution, including debounce. */
    s->critical = s->present && s->source_known && !state->source_pending && !s->external && s->percent <= 2;
    state->external = s->external;
    state->present = s->present;
    state->charging = s->charging;
    state->fully_charged = s->fully_charged;
    state->published = 1;
    return 1;
}
#endif
