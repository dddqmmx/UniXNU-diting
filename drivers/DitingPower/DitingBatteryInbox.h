/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef DITING_BATTERY_INBOX_H
#define DITING_BATTERY_INBOX_H
#include <stdint.h>

enum {kDitingBatteryRing = 16, kDitingBatteryMessage = 64};

/* The caller holds its inbox lock for every access. State changes and
 * messages share one generation, including a down/up pair before dispatch. */
typedef struct {
    struct {uint32_t length; uint8_t data[kDitingBatteryMessage];} ring[kDitingBatteryRing];
    uint32_t head, count, epoch;
    int up, changed;
} DitingBatteryInbox;

static inline void diting_battery_inbox_state(DitingBatteryInbox *inbox, int up)
{
    if (!up || !inbox->up) {
        inbox->epoch++;
        inbox->head = inbox->count = 0;
    }
    inbox->up = up != 0;
    inbox->changed = 1;
}

static inline int diting_battery_inbox_push(DitingBatteryInbox *inbox, const uint8_t *data, uint32_t length)
{
    if (!inbox->up || !data || length < 12 || length > kDitingBatteryMessage || inbox->count == kDitingBatteryRing) return 0;
    const unsigned slot = (inbox->head + inbox->count) % kDitingBatteryRing;
    for (unsigned i = 0; i < length; ++i) inbox->ring[slot].data[i] = data[i];
    inbox->ring[slot].length = length;
    inbox->count++;
    return 1;
}

static inline uint32_t diting_battery_inbox_pop(DitingBatteryInbox *inbox, uint8_t *data)
{
    if (!inbox->count) return 0;
    const uint32_t length = inbox->ring[inbox->head].length;
    for (unsigned i = 0; i < length; ++i) data[i] = inbox->ring[inbox->head].data[i];
    inbox->head = (inbox->head + 1) % kDitingBatteryRing;
    inbox->count--;
    return length;
}
#endif
