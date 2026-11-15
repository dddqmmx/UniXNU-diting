// SPDX-License-Identifier: BSD-2-Clause-Patent
// Diting's physical battery: PMIC GLINK measurements published through the
// Darwin24 IOPMPowerSource API. All codec/publication work runs on one work
// loop; DSP callbacks only update a locked, bounded inbox and signal it.
// Charging control stays with the phone's ADSP firmware.
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include "DitingBatteryInbox.h"
#include "DitingBatteryState.h"
extern "C" {
#include "ShengAdsp.h"
}

extern "C" ShengAdspChannel *sheng_adsp_channel(const char *, uint32_t, uint32_t, int, const ShengAdspClient *, void *) __attribute__((weak_import));
extern "C" int sheng_adsp_send(ShengAdspChannel *, const void *, uint32_t) __attribute__((weak_import));
// Diagnostic layouts retained: stage, percent, mV, mA, temperature (0.01 C),
// flags, cycles completed, replies, timeouts/errors, notifications, USB type,
// full/design mAh, battery cycle count, dropped/ignored, send refusals.
extern "C" {extern volatile int32_t native_battery_stats[16];}
// Capacity 0.01%, four unused dual-gauge slots, temperature 0.01 C, current
// uA (discharge positive), valid fields, last timeout field, transport epoch.
extern "C" {volatile int32_t native_battery_raw[10];}
// USB online/type/adapter/real type, limit/max mA, mV/mA, battery status,
// charge type, thermal level, charge current mA, watts, authentication bits,
// legacy write counters (always zero) | adapter type <<16, legacy result (0).
extern "C" {volatile int32_t native_charger_stats[16];}
static volatile int32_t ditingUsbCable = -1;
extern "C" int diting_battery_usb_online(void) {
    return __atomic_load_n(&ditingUsbCable, __ATOMIC_ACQUIRE);
}

namespace {
constexpr uint32_t kRequestMs = 2000, kRefreshMs = 5000, kSlowEvery = 120, kSubscribeTries = 3;
constexpr uint32_t bit(unsigned field) {return 1u << field;}
constexpr uint32_t kUnusedFields = bit(BmGauge1Remaining) | bit(BmGauge1Full) | bit(BmGauge2Remaining) | bit(BmGauge2Full) |
    bit(BmPowerMax) | bit(BmPdVerified) | bit(BmAuthentic) | bit(BmSlaveAuthentic) | bit(BmInputSuspend);
}

class DitingBattery : public IOPMPowerSource {
    OSDeclareDefaultStructors(DitingBattery)
    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *request = nullptr, *refresh = nullptr;
    IOInterruptEventSource *inboxEvent = nullptr;
    IOSimpleLock *ringLock = nullptr;
    DitingBatteryInbox inbox = {};
    DitingBatteryState power = {};
    uint32_t activeEpoch = 0;
    bool wasUp = false, published = false;
    uint32_t lastPowerFlags = ~0u, refreshes = 0, subscribeTries = 0;
    ShengBattmgr bm;
    ShengAdspChannel *channel = nullptr;

    static void dspState(void *context, int up) {
        DitingBattery *self = static_cast<DitingBattery *>(context);
        IOSimpleLockLock(self->ringLock);
        diting_battery_inbox_state(&self->inbox, up);
        IOSimpleLockUnlock(self->ringLock);
        self->inboxEvent->interruptOccurred(nullptr, nullptr, 0);
    }
    static void dspReceive(void *context, const uint8_t *data, uint32_t length) {
        DitingBattery *self = static_cast<DitingBattery *>(context);
        IOSimpleLockLock(self->ringLock);
        if (!diting_battery_inbox_push(&self->inbox, data, length))
            native_battery_stats[14] = native_battery_stats[14] + 1;
        IOSimpleLockUnlock(self->ringLock);
        self->inboxEvent->interruptOccurred(nullptr, nullptr, 0);
    }
    bool live() {
        IOSimpleLockLock(ringLock);
        const bool up = inbox.up && inbox.epoch == activeEpoch;
        IOSimpleLockUnlock(ringLock);
        return wasUp && up;
    }
    void number(const char *key, int64_t value) {
        const OSSymbol *symbol = OSSymbol::withCString(key);
        OSNumber *n = OSNumber::withNumber((unsigned long long)value, 32);
        if (symbol && n) setPSProperty(symbol, n);
        OSSafeReleaseNULL(n); OSSafeReleaseNULL(symbol);
    }
    void flag(const char *key, bool value) {
        const OSSymbol *symbol = OSSymbol::withCString(key);
        if (symbol) setPSProperty(symbol, value ? kOSBooleanTrue : kOSBooleanFalse);
        OSSafeReleaseNULL(symbol);
    }
    void optionalNumber(const char *key, bool valid, int value) {
        if (valid) {number(key, value); return;}
        const OSSymbol *symbol = OSSymbol::withCString(key);
        if (symbol && properties->getObject(symbol)) {
            properties->removeObject(symbol);
            removeProperty(symbol);
            settingsChangedSinceUpdate = true;
        }
        OSSafeReleaseNULL(symbol);
    }
    void stale() {
        flag("DitingBatteryStale", true);
        flag("DitingBatterySourcePending", false);
        flag("DitingBatterySourceKnown", false);
        flag("DitingBatteryStatusKnown", false);
        number("DitingBatteryFreshFields", 0);
        number("DitingBatteryValidFields", bm.completed_valid);
        number("DitingBatteryMissingFields", ~kUnusedFields & ~bm.completed_valid);
        number("DitingBatteryTransportEpoch", activeEpoch);
        if (published) updateStatus();
    }
    void subscribe() {
        if (!live() || bm.enabled || subscribeTries >= kSubscribeTries) return;
        uint8_t out[SHENG_BATTMGR_ENABLE_BYTES];
        subscribeTries++;
        if (sheng_adsp_send(channel, out, sheng_battmgr_enable_request(out)))
            native_battery_stats[15] = native_battery_stats[15] + 1;
    }
    void recordRaw(const ShengBatterySnapshot &s) {
        static const int fields[7] = {BmCapacity, BmGauge1Remaining, BmGauge1Full, BmGauge2Remaining, BmGauge2Full, BmTemperature, BmCurrent};
        for (unsigned i = 0; i < 7; ++i)
            native_battery_raw[i] = bm.completed_valid & bit(fields[i]) ? bm.completed_value[fields[i]] : -999999;
        native_battery_raw[7] = (int32_t)bm.completed_valid;
        native_battery_raw[8] = bm.last_timeout;
        native_battery_raw[9] = (int32_t)activeEpoch;
        static const int charger[12] = {BmUsbOnline, BmUsbType, BmUsbAdapter, BmRealType, BmUsbLimit, BmUsbMax, BmUsbVoltage, BmUsbCurrent,
                                        BmStatus, BmChargeType, BmThermalLevel, BmChargeCurrent};
        static const int milli[12] = {0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1};
        for (unsigned i = 0; i < 12; ++i)
            native_charger_stats[i] = bm.completed_valid & bit(charger[i]) ?
                (milli[i] ? bm.completed_value[charger[i]] / 1000 : bm.completed_value[charger[i]]) : -1;
        native_charger_stats[12] = s.external ? s.adapter_watts : 0;
        native_charger_stats[13] = 0;  // no authentication properties requested
        native_charger_stats[14] = bm.completed_valid & bit(BmUsbAdapter) ? (bm.completed_value[BmUsbAdapter] & 0xff) << 16 : 0;
        native_charger_stats[15] = 0;  // no charger writes
    }
    void adapterDetails(const ShengBatterySnapshot &s) {
        // Keep the last coherent adapter during debounce or a missing VBUS
        // reply. The raw decoder's candidate source may differ from s.
        if (s.external && (!s.source_known || power.source_pending)) return;
        OSDictionary *adapter = OSDictionary::withCapacity(4);
        if (!adapter) return;
        const char *keys[] = {kIOPMPSAdapterDetailsFamilyKey, kIOPMPSAdapterDetailsWattsKey,
                             kIOPMPSAdapterDetailsVoltage, kIOPMPSAdapterDetailsAmperageKey};
        const uint32_t values[] = {s.external ? s.adapter_family : 0,
                                  s.external ? (uint32_t)s.adapter_watts : 0,
                                  s.external ? (uint32_t)s.adapter_mv : 0,
                                  s.external ? (uint32_t)s.adapter_ma : 0};
        for (unsigned i = 0; i < 4; ++i) {
            OSNumber *n = OSNumber::withNumber(values[i], 32);
            if (n) {adapter->setObject(keys[i], n); n->release();}
        }
        const OSSymbol *key = OSSymbol::withCString(kIOPMPSAdapterDetailsKey);
        if (key) setPSProperty(key, adapter);
        OSSafeReleaseNULL(key);
        adapter->release();
    }
    void publish() {
        if (!live()) return;
        ShengBatterySnapshot s;
        sheng_battmgr_snapshot(&bm, &s);
        const int cable = s.source_known ? s.external : -1;
        __atomic_store_n(&ditingUsbCable, cable, __ATOMIC_RELEASE);
        recordRaw(s);
        number("DitingBatteryValidFields", bm.completed_valid);
        number("DitingBatteryFreshFields", bm.completed);
        number("DitingBatteryMissingFields", ~kUnusedFields & ~bm.completed_valid);
        number("DitingBatterySampleCycle", s.sample_cycle);
        number("DitingBatteryTransportEpoch", activeEpoch);
        if (!diting_battery_resolve(&power, &s)) {
            // A failed capacity request is not an empty battery. A genuine
            // PRESENT=0 reply can still report removal on a published source.
            if (published && s.present_known && !s.present) {
                setBatteryInstalled(false); setIsCharging(false);
                setAtWarnLevel(false); setAtCriticalLevel(false);
                flag("FullyCharged", false);
                power.present = 0;
                power.charging = power.fully_charged = 0;
            }
            stale();
            return;
        }
        flag("DitingBatteryStale", false);
        flag("DitingBatterySourcePending", power.source_pending);
        flag("DitingBatterySourceKnown", s.source_known);
        flag("DitingBatteryStatusKnown", s.status_known);
        setBatteryInstalled(s.present);
        setExternalConnected(s.external);
        setExternalChargeCapable(s.external);
        setIsCharging(s.charging);
        setAtWarnLevel(s.present && s.source_known && !power.source_pending && !s.external && s.percent <= 10);
        setAtCriticalLevel(s.critical);
        setCurrentCapacity((unsigned)s.percent);
        setMaxCapacity(100);
        optionalNumber(kIOPMPSVoltageKey, bm.completed_valid & bit(BmVoltage), s.voltage_mv);
        optionalNumber(kIOPMPSAmperageKey, bm.completed_valid & bit(BmCurrent), s.current_ma);
        optionalNumber("InstantAmperage", bm.completed_valid & bit(BmCurrent), s.current_ma);
        optionalNumber(kIOPMPSCycleCountKey, bm.completed_valid & bit(BmCycles), s.cycles);
        optionalNumber("Temperature", s.temperature_valid, s.temperature);
        optionalNumber("AppleRawCurrentCapacity", bm.completed_valid & bit(BmCharge), s.raw_mah);
        optionalNumber("AppleRawMaxCapacity", bm.completed_valid & bit(BmFull), s.full_mah);
        optionalNumber("NominalChargeCapacity", bm.completed_valid & bit(BmFull), s.full_mah);
        optionalNumber("DesignCapacity", bm.completed_valid & bit(BmDesign), s.design_mah);
        setTimeRemaining(s.external ? (s.fully_charged ? 0 :
            (s.charging && s.time_to_full_min >= 0 ? s.time_to_full_min : 0xffff)) :
            (s.time_to_empty_min >= 0 ? s.time_to_empty_min : 0xffff));
        flag("FullyCharged", s.fully_charged);
        adapterDetails(s);
        native_battery_stats[1] = s.percent; native_battery_stats[2] = s.voltage_mv; native_battery_stats[3] = s.current_ma;
        native_battery_stats[4] = s.temperature;
        native_battery_stats[5] = s.present | s.external << 1 | s.charging << 2 | s.fully_charged << 3 | bm.enabled << 4;
        native_battery_stats[10] = bm.completed_valid & bit(BmUsbType) ? bm.completed_value[BmUsbType] : -1;
        native_battery_stats[11] = s.full_mah; native_battery_stats[12] = s.design_mah; native_battery_stats[13] = s.cycles;
        const uint32_t flags = s.external | s.charging << 1 | s.fully_charged << 2;
        if (flags != lastPowerFlags) {
            lastPowerFlags = flags;
            IOLog("DitingBattery: source external=%d charging=%d full=%d pending=%d online=%d VBUS=%dmV status=%d subscribed=%u\n",
                s.external, s.charging, s.fully_charged, power.source_pending, native_charger_stats[0], native_charger_stats[6],
                native_charger_stats[8], unsigned(bm.enabled));
        }
        // updateStatus copies the properties to IORegistry before matching
        // can find this service for the first time.
        updateStatus();
        if (!published) {
            published = true;
            native_battery_stats[0] = 4;
            registerService();
            IOLog("DitingBattery: physical power source published %d%% %dmV epoch=%u\n", s.percent, s.voltage_mv, activeEpoch);
        }
    }
    void counters() {
        native_battery_stats[5] = (native_battery_stats[5] & ~16) | bm.enabled << 4;
        native_battery_stats[6] = (int32_t)bm.cycles_done;
        native_battery_stats[7] = (int32_t)bm.responses;
        native_battery_stats[8] = (int32_t)(bm.timeouts | bm.errors << 16);
        native_battery_stats[9] = (int32_t)bm.notifications;
        IOSimpleLockLock(ringLock);
        native_battery_stats[14] = (native_battery_stats[14] & 0xffff) | (int32_t)(bm.ignored << 16);
        IOSimpleLockUnlock(ringLock);
    }
    void pump() {
        // Queue refusal has no firmware response to wait for. Skip it now;
        // keep the work bounded even if the entire send queue is unavailable.
        for (unsigned tries = 0; tries < BmFields && live(); ++tries) {
            uint8_t out[SHENG_BATTMGR_REQUEST_BYTES];
            const uint32_t n = sheng_battmgr_next(&bm, out);
            if (!n) return;
            if (!sheng_adsp_send(channel, out, n)) {
                request->setTimeoutMS(kRequestMs);
                return;
            }
            native_battery_stats[15] = native_battery_stats[15] + 1;
            const int result = sheng_battmgr_send_failed(&bm);
            if (result == BmSnapshotReady || result == BmCycleDone) publish();
            counters();
        }
    }
    void handle(int result) {
        if (result == BmSnapshotReady || result == BmCycleDone) publish();
        counters();
        pump();
    }
    static void inboxAction(OSObject *owner, IOInterruptEventSource *, int) {
        DitingBattery *self = OSDynamicCast(DitingBattery, owner);
        if (!self) return;
        IOSimpleLockLock(self->ringLock);
        const bool up = self->inbox.up;
        const uint32_t epoch = self->inbox.epoch;
        const bool changed = self->inbox.changed;
        self->inbox.changed = 0;
        IOSimpleLockUnlock(self->ringLock);
        if (changed && (epoch != self->activeEpoch || up != self->wasUp)) {
            self->request->cancelTimeout(); self->refresh->cancelTimeout();
            self->activeEpoch = epoch;
            self->wasUp = up;
            self->subscribeTries = self->refreshes = 0;
            sheng_battmgr_init(&self->bm);
            diting_battery_new_transport(&self->power);
            ShengBatterySnapshot unknown;
            sheng_battmgr_snapshot(&self->bm, &unknown);
            self->recordRaw(unknown);
            self->counters();
            __atomic_store_n(&ditingUsbCable, -1, __ATOMIC_RELEASE);
            self->stale();
            native_battery_stats[0] = self->published ? 4 : (up ? 3 : 2);
            IOLog("DitingBattery: PMIC channel %s epoch=%u\n", up ? "up" : "down", epoch);
            if (up) {
                self->subscribe();
                sheng_battmgr_refresh(&self->bm, 1);
                self->pump();
                self->refresh->setTimeoutMS(kRefreshMs);
            }
        }
        for (unsigned handled = 0; handled < kDitingBatteryRing; ++handled) {
            uint8_t data[kDitingBatteryMessage];
            IOSimpleLockLock(self->ringLock);
            const bool current = self->inbox.up && self->inbox.epoch == self->activeEpoch;
            const uint32_t length = current ? diting_battery_inbox_pop(&self->inbox, data) : 0;
            IOSimpleLockUnlock(self->ringLock);
            if (!length) break;
            const int result = sheng_battmgr_receive(&self->bm, data, length);
            if (result == BmAnswer || result == BmSnapshotReady || result == BmCycleDone) self->request->cancelTimeout();
            self->handle(result);
        }
        IOSimpleLockLock(self->ringLock);
        const bool pending = self->inbox.count || self->inbox.changed;
        IOSimpleLockUnlock(self->ringLock);
        if (pending) self->inboxEvent->interruptOccurred(nullptr, nullptr, 0);
    }
    static void requestAction(OSObject *owner, IOTimerEventSource *) {
        DitingBattery *self = OSDynamicCast(DitingBattery, owner);
        if (self && self->live()) self->handle(sheng_battmgr_timeout(&self->bm));
    }
    static void refreshAction(OSObject *owner, IOTimerEventSource *sender) {
        DitingBattery *self = OSDynamicCast(DitingBattery, owner);
        if (!self || !self->live()) return;
        self->subscribe();
        sheng_battmgr_refresh(&self->bm, ++self->refreshes % kSlowEvery == 0);
        self->handle(BmIgnored);
        sender->setTimeoutMS(kRefreshMs);
    }
    void releaseSources() {
        IOTimerEventSource *sources[] = {request, refresh};
        for (IOTimerEventSource *source : sources) if (source) {
            source->cancelTimeout();
            if (loop && source->getWorkLoop() == loop) loop->removeEventSource(source);
        }
        if (loop && inboxEvent && inboxEvent->getWorkLoop() == loop) loop->removeEventSource(inboxEvent);
        OSSafeReleaseNULL(request); OSSafeReleaseNULL(refresh); OSSafeReleaseNULL(inboxEvent);
        OSSafeReleaseNULL(loop);
        if (ringLock) {IOSimpleLockFree(ringLock); ringLock = nullptr;}
    }
    static IOReturn registerChannel(OSObject *owner, void *, void *, void *, void *) {
        DitingBattery *self = OSDynamicCast(DitingBattery, owner);
        if (!self) return kIOReturnBadArgument;
        const ShengAdspClient client = {&DitingBattery::dspState, &DitingBattery::dspReceive};
        self->channel = sheng_adsp_channel("PMIC_RTR_ADSP_APPS", 1024, 4, 0, &client, self);
        if (!self->channel) return kIOReturnNoResources;
        native_battery_stats[0] = 2;
        return kIOReturnSuccess;
    }

public:
    bool startBattery() {
        sheng_battmgr_init(&bm);
        if (!sheng_adsp_channel || !sheng_adsp_send) return false;
        loop = IOWorkLoop::workLoop();
        ringLock = IOSimpleLockAlloc();
        inboxEvent = IOInterruptEventSource::interruptEventSource(this, &DitingBattery::inboxAction);
        request = IOTimerEventSource::timerEventSource(this, &DitingBattery::requestAction);
        refresh = IOTimerEventSource::timerEventSource(this, &DitingBattery::refreshAction);
        if (!loop || !ringLock || !inboxEvent || !request || !refresh || loop->addEventSource(inboxEvent) != kIOReturnSuccess ||
            loop->addEventSource(request) != kIOReturnSuccess || loop->addEventSource(refresh) != kIOReturnSuccess) {
            releaseSources();
            return false;
        }
        native_battery_stats[0] = 1;
        // A fast remote OPEN may signal the inbox before registration returns.
        // runAction holds the gate until the channel pointer is installed.
        if (loop->runAction(&DitingBattery::registerChannel, this) != kIOReturnSuccess) {
            releaseSources();
            return false;
        }
        return true;
    }
    void free() override {
        releaseSources();
        IOPMPowerSource::free();
    }
};
OSDefineMetaClassAndStructors(DitingBattery, IOPMPowerSource)

extern "C" void sheng_battery_start(void) {
    DitingBattery *battery = OSTypeAlloc(DitingBattery);
    if (!battery || !battery->init() || !battery->attach(IOService::getPlatform())) {
        if (battery) battery->release();
        native_battery_stats[0] = -1;
        IOLog("DitingBattery: service allocation/attachment failed\n");
        return;
    }
    if (!battery->startBattery()) {
        native_battery_stats[0] = native_battery_stats[0] > 0 ? -native_battery_stats[0] - 1 : -1;
        IOLog("DitingBattery: startup failed stage=%d\n", native_battery_stats[0]);
        battery->detach(IOService::getPlatform());
        battery->release();
        return;
    }
    battery->release();  // the platform's attachment keeps it
}
