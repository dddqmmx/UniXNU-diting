// SPDX-License-Identifier: BSD-2-Clause-Patent
// Shared PMIC GLINK ("PMIC_RTR_ADSP_APPS") channel multiplexer for Diting.
// Owns the single ADSP GLINK channel registration and routes callbacks by
// 32-bit PMIC GLINK owner without holding spinlocks across client callbacks.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include "DitingPmicGlink.h"
extern "C" {
#include "DitingAdsp.h"
}

extern "C" ShengAdspChannel *sheng_adsp_channel(const char *, uint32_t, uint32_t, int, const ShengAdspClient *, void *)
    __attribute__((weak_import));
extern "C" int sheng_adsp_send(ShengAdspChannel *, const void *, uint32_t) __attribute__((weak_import));

namespace {

IOSimpleLock *gLock = nullptr;
DitingPmicGlinkRouter gRouter = {};
ShengAdspChannel *gChannel = nullptr;
bool gChannelOpening = false;
bool gChannelOpened = false;

IOSimpleLock *ensureLock()
{
    IOSimpleLock *lock = __atomic_load_n(&gLock, __ATOMIC_ACQUIRE);
    if (lock) return lock;
    IOSimpleLock *created = IOSimpleLockAlloc();
    if (!created) return nullptr;
    IOSimpleLock *expected = nullptr;
    if (__atomic_compare_exchange_n(&gLock, &expected, created, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return created;
    IOSimpleLockFree(created);
    return expected;
}

void adspStateCallback(void *, int up)
{
    IOSimpleLock *lock = ensureLock();
    if (!lock) return;
    DitingPmicGlinkSlot slots[DITING_PMIC_GLINK_MAX_CLIENTS];
    IOSimpleLockLock(lock);
    const unsigned count = diting_pmic_glink_router_state(&gRouter, up, slots, DITING_PMIC_GLINK_MAX_CLIENTS);
    const uint32_t epoch = gRouter.epoch;
    IOSimpleLockUnlock(lock);
    IOLog("DitingPmicGlink: channel %s epoch=%u clients=%u\n", up ? "up" : "down", epoch, count);
    for (unsigned i = 0; i < count && i < DITING_PMIC_GLINK_MAX_CLIENTS; ++i) {
        if (slots[i].client.state) slots[i].client.state(slots[i].context, up ? 1 : 0);
    }
}

void adspReceiveCallback(void *, const uint8_t *data, uint32_t length)
{
    IOSimpleLock *lock = ensureLock();
    if (!lock) return;
    DitingPmicGlinkSlot slot = {};
    IOSimpleLockLock(lock);
    const int matched = diting_pmic_glink_router_lookup(&gRouter, data, length, &slot);
    IOSimpleLockUnlock(lock);
    if (matched && slot.client.receive) slot.client.receive(slot.context, data, length);
}

} // namespace

extern "C" int diting_pmic_glink_register(uint32_t owner, const DitingPmicGlinkClient *client, void *context)
{
    if (!sheng_adsp_channel || !sheng_adsp_send) return -1;
    IOSimpleLock *lock = ensureLock();
    if (!lock) return -1;

    bool shouldOpen = false;
    int alreadyUp = 0;

    IOSimpleLockLock(lock);
    if (diting_pmic_glink_router_register(&gRouter, owner, client, context) != 0) {
        IOSimpleLockUnlock(lock);
        return -1;
    }
    if (!gChannelOpened && !gChannelOpening) {
        gChannelOpening = true;
        shouldOpen = true;
    } else if (gChannelOpened) {
        alreadyUp = gRouter.up;
    }
    IOSimpleLockUnlock(lock);

    if (shouldOpen) {
        const ShengAdspClient adspClient = {&adspStateCallback, &adspReceiveCallback};
        ShengAdspChannel *ch = sheng_adsp_channel(DITING_PMIC_GLINK_CHANNEL, 1024, 4, 0, &adspClient, nullptr);
        IOSimpleLockLock(lock);
        gChannelOpening = false;
        if (ch) {
            gChannel = ch;
            gChannelOpened = true;
        } else {
            diting_pmic_glink_router_unregister(&gRouter, owner);
        }
        IOSimpleLockUnlock(lock);
        if (!ch) {
            IOLog("DitingPmicGlink: failed to open %s\n", DITING_PMIC_GLINK_CHANNEL);
            return -1;
        }
        return 0;
    }

    if (alreadyUp && client && client->state) client->state(context, 1);
    return 0;
}

extern "C" int diting_pmic_glink_send(uint32_t owner, const void *data, uint32_t length)
{
    if (!sheng_adsp_send) return -2;
    if (!diting_pmic_glink_validate_tx(owner, data, length, SHENG_ADSP_MAX_MESSAGE)) return -2;
    IOSimpleLock *lock = ensureLock();
    if (!lock) return -1;

    IOSimpleLockLock(lock);
    ShengAdspChannel *ch = gChannel;
    const int up = gRouter.up;
    IOSimpleLockUnlock(lock);

    if (!ch || !up) {
        IOSimpleLockLock(lock);
        gRouter.tx_err++;
        IOSimpleLockUnlock(lock);
        return -1;
    }
    const int rc = sheng_adsp_send(ch, data, length);
    IOSimpleLockLock(lock);
    if (rc == 0) gRouter.tx_ok++;
    else gRouter.tx_err++;
    IOSimpleLockUnlock(lock);
    return rc;
}

extern "C" void diting_pmic_glink_stats(DitingPmicGlinkStats *out)
{
    if (!out) return;
    for (unsigned i = 0; i < sizeof *out; ++i) ((uint8_t *)out)[i] = 0;
    IOSimpleLock *lock = __atomic_load_n(&gLock, __ATOMIC_ACQUIRE);
    if (!lock) return;
    IOSimpleLockLock(lock);
    out->epoch = gRouter.epoch;
    out->rx_routed = gRouter.rx_routed;
    out->rx_unknown_owner = gRouter.rx_unknown_owner;
    out->rx_short = gRouter.rx_short;
    out->tx_ok = gRouter.tx_ok;
    out->tx_err = gRouter.tx_err;
    out->up = gRouter.up;
    IOSimpleLockUnlock(lock);
}
