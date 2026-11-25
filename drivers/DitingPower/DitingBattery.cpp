// SPDX-License-Identifier: BSD-2-Clause-Patent
// Diting's physical battery & Type-C UCSI monitor: PMIC GLINK measurements
// published through the Darwin IOPMPowerSource API. All codec/publication work
// runs on one work loop; DSP callbacks only update locked, bounded inboxes and
// signal it. Charger control writes are disabled by default (diting_charger_ctl=0)
// and strictly whitelisted/bounded when opted in via boot-arg.
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
#include <pexpert/pexpert.h>
#include "DitingBatteryInbox.h"
#include "DitingBatteryState.h"
#include "DitingPmicGlink.h"
#include "DitingUcsi.h"
extern "C" {
#include "DitingAdsp.h"
}

extern "C" ShengAdspChannel *sheng_adsp_channel(const char *, uint32_t, uint32_t, int, const ShengAdspClient *, void *)
    __attribute__((weak_import));
extern "C" int sheng_adsp_send(ShengAdspChannel *, const void *, uint32_t) __attribute__((weak_import));
extern "C" int diting_pmic_glink_register(uint32_t, const DitingPmicGlinkClient *, void *) __attribute__((weak_import));
extern "C" int diting_pmic_glink_send(uint32_t, const void *, uint32_t) __attribute__((weak_import));
extern "C" void diting_pmic_glink_stats(DitingPmicGlinkStats *) __attribute__((weak_import));

// Diagnostic layouts retained: stage, percent, mV, mA, temperature (0.01 C),
// flags, cycles completed, replies, timeouts/errors, notifications, USB type,
// full/design mAh, battery cycle count, dropped/ignored, send refusals.
extern "C" {extern volatile int32_t native_battery_stats[16];}
// Capacity 0.01%, four unused dual-gauge slots, temperature 0.01 C, current
// uA (discharge positive), valid fields, last timeout field, transport epoch.
extern "C" {volatile int32_t native_battery_raw[10];}
// USB online/type/adapter/real type, limit/max mA, mV/mA, battery status,
// charge type, thermal level, charge current mA, watts, authentication bits,
// write ok/err counts | adapter type <<16 | ctlMode <<24, last write result.
extern "C" {volatile int32_t native_charger_stats[16];}

static volatile int32_t ditingUsbCable = -1;
static volatile int32_t ditingUsbDataRole = -1;
static volatile int32_t ditingUcsiDataRole = -1;

extern "C" int diting_battery_usb_online(void) {
    return __atomic_load_n(&ditingUsbCable, __ATOMIC_ACQUIRE);
}
extern "C" int diting_battery_usb_data_online(void) {
    return __atomic_load_n(&ditingUsbDataRole, __ATOMIC_ACQUIRE);
}
extern "C" int diting_ucsi_usb_data_online(void) {
    return __atomic_load_n(&ditingUcsiDataRole, __ATOMIC_ACQUIRE);
}

namespace {

constexpr uint32_t kRequestMs = 2000, kRefreshMs = 5000, kSlowEvery = 120, kSubscribeTries = 3;
constexpr uint32_t kUcsiRequestMs = 1500, kUcsiNotifyWaitMs = 400;
constexpr uint32_t bit(unsigned field) {return 1u << field;}
constexpr uint32_t kUnusedFields = bit(BmGauge1Remaining) | bit(BmGauge1Full) | bit(BmGauge2Remaining) | bit(BmGauge2Full) |
    bit(BmPowerMax) | bit(BmPdVerified) | bit(BmAuthentic) | bit(BmSlaveAuthentic) | bit(BmInputSuspend);

struct BatteryDiagSnapshot {
    int stage;
    int valid;
    int percent, voltage_mv, current_ma, temperature, temperature_valid;
    int present, external, charging, full, critical;
    int usb_online, usb_type, usb_adapter, usb_real_type;
    int vbus_mv, ibus_ma, icl_ma, usb_max_ma, chg_cur_ma, watts, thermal_level;
    uint32_t epoch, cycles, responses, notifications, ctl_mode, writes_ok, writes_err;
    int32_t last_write_rc;
    DitingUcsiStatus ucsi;
    uint32_t ucsi_updates, ucsi_notifications, ucsi_errors;
    int usb_data_role;
};

IOSimpleLock *gDiagLock = nullptr;
BatteryDiagSnapshot gDiag = {};

IOSimpleLock *ensureDiagLock() {
    IOSimpleLock *lock = __atomic_load_n(&gDiagLock, __ATOMIC_ACQUIRE);
    if (lock) return lock;
    IOSimpleLock *created = IOSimpleLockAlloc();
    if (!created) return nullptr;
    IOSimpleLock *expected = nullptr;
    if (__atomic_compare_exchange_n(&gDiagLock, &expected, created, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return created;
    IOSimpleLockFree(created);
    return expected;
}

const char *usbTypeName(int type) {
    switch (type) {
    case 0: return "NONE";
    case 1: return "SDP";
    case 2: return "DCP";
    case 3: return "CDP";
    case 4: return "ACA";
    case 5: return "TYPEC";
    case 6: return "PD";
    case 7: return "PD_DRP";
    case 8: return "PD_PPS";
    case 9: return "APPLE";
    case 0x80: return "HVDCP2";
    case 0x81: return "HVDCP3";
    case 0x82: return "HVDCP3P5";
    case 0x83: return "FLOAT";
    case 0x84: return "HVDCP3B";
    default: return "UNK";
    }
}

const char *ucsiOpmodeName(uint8_t opmode) {
    switch (opmode) {
    case DitingUcsiPwrOpmodeNone: return "NONE";
    case DitingUcsiPwrOpmodeDefault: return "DEF";
    case DitingUcsiPwrOpmodeBc: return "BC";
    case DitingUcsiPwrOpmodePd: return "PD";
    case DitingUcsiPwrOpmodeTypeC1_5: return "TC1.5";
    case DitingUcsiPwrOpmodeTypeC3_0: return "TC3.0";
    default: return "UNK";
    }
}

const char *ucsiPartnerName(uint8_t partner) {
    switch (partner) {
    case DitingUcsiPartnerNone: return "NONE";
    case DitingUcsiPartnerDfp: return "DFP";
    case DitingUcsiPartnerUfp: return "UFP";
    case DitingUcsiPartnerCable: return "CABLE";
    case DitingUcsiPartnerCableUfp: return "CABLE+UFP";
    case DitingUcsiPartnerDebug: return "DEBUG";
    case DitingUcsiPartnerAudio: return "AUDIO";
    default: return "UNK";
    }
}

} // namespace

extern "C" unsigned diting_battery_snapshot_text(char *out, unsigned size) {
    if (!out || size == 0) return 0;
    BatteryDiagSnapshot d = {};
    IOSimpleLock *lock = __atomic_load_n(&gDiagLock, __ATOMIC_ACQUIRE);
    if (lock) {
        IOSimpleLockLock(lock);
        d = gDiag;
        IOSimpleLockUnlock(lock);
    } else {
        d.stage = native_battery_stats[0];
        d.usb_data_role = __atomic_load_n(&ditingUsbDataRole, __ATOMIC_ACQUIRE);
    }
    DitingPmicGlinkStats glink = {};
    if (diting_pmic_glink_stats) diting_pmic_glink_stats(&glink);

    const int tempWhole = d.temperature / 100;
    int tempFrac = (d.temperature < 0 ? -d.temperature : d.temperature) % 100;
    const int n = snprintf(out, size,
        "Battery: %d%% %dmV %dmA %d.%02dC ext=%d chg=%d full=%d crit=%d stage=%d epoch=%u cyc=%u\n"
        "USB: online=%d type=%d(%s) adap=%d(%s) real=%d(%s) vbus=%dmV ibus=%dmA icl=%dmA max=%dmA chg_i=%dmA %dW therm=%d\n"
        "UCSI: valid=%d conn=%u op=%u(%s) dir=%u partner=%u(%s) flags=0x%x rdo=0x%08x(%u/%umA) upd=%u ntfy=%u err=%u data_role=%d\n"
        "GLINK: up=%d epoch=%u rx=%u unk=%u tx=%u/%u ctl=%u wr=%u/%u rc=%d\n",
        d.percent, d.voltage_mv, d.current_ma, tempWhole, tempFrac,
        d.external, d.charging, d.full, d.critical, d.stage, d.epoch, d.cycles,
        d.usb_online, d.usb_type, usbTypeName(d.usb_type), d.usb_adapter, usbTypeName(d.usb_adapter),
        d.usb_real_type, usbTypeName(d.usb_real_type), d.vbus_mv, d.ibus_ma, d.icl_ma, d.usb_max_ma,
        d.chg_cur_ma, d.watts, d.thermal_level,
        d.ucsi.valid, unsigned(d.ucsi.connected), unsigned(d.ucsi.pwr_opmode), ucsiOpmodeName(d.ucsi.pwr_opmode),
        unsigned(d.ucsi.pwr_dir), unsigned(d.ucsi.partner_type), ucsiPartnerName(d.ucsi.partner_type),
        unsigned(d.ucsi.partner_flags), d.ucsi.rdo, unsigned(d.ucsi.rdo_op_ma), unsigned(d.ucsi.rdo_max_ma),
        d.ucsi_updates, d.ucsi_notifications, d.ucsi_errors, d.usb_data_role,
        glink.up, glink.epoch, glink.rx_routed, glink.rx_unknown_owner, glink.tx_ok, glink.tx_err,
        d.ctl_mode, d.writes_ok, d.writes_err, d.last_write_rc);
    if (n <= 0) return 0;
    return (unsigned)n < size ? (unsigned)n : size - 1;
}

class DitingBattery : public IOPMPowerSource {
    OSDeclareDefaultStructors(DitingBattery)
    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *request = nullptr, *refresh = nullptr, *ucsiTimer = nullptr;
    IOInterruptEventSource *inboxEvent = nullptr, *ucsiEvent = nullptr;
    IOSimpleLock *ringLock = nullptr, *ucsiLock = nullptr;
    DitingBatteryInbox inbox = {}, ucsiInbox = {};
    DitingBatteryState power = {};
    uint32_t activeEpoch = 0, ucsiEpoch = 0;
    bool wasUp = false, ucsiWasUp = false, published = false, usePmicGlink = false, ucsiRegistered = false;
    uint32_t lastPowerFlags = ~0u, refreshes = 0, subscribeTries = 0;
    uint32_t chargerCtlMode = 0;
    uint32_t currentThermalLevel = 0, pendingThermalLevel = 0;
    bool pendingThermalWrite = false;
    uint32_t lastThermalWriteTick = 0, lastIclWriteTick = 0;
    int lastLoggedUsbType = -2, lastLoggedAdapter = -2, lastLoggedRealType = -2, lastLoggedIclMa = -2;
    int lastCurrentSign = 0; // -1 draining, +1 charging, 0 unset
    int lastUcsiSig = -1;
    int lastBattUsbClass = DitingUsbClassUnknown;
    int lastResolvedDataRole = -2;
    DitingUcsiStatus latestUcsi = {};
    ShengBattmgr bm;
    DitingUcsi ucsi;
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
    static void ucsiState(void *context, int up) {
        DitingBattery *self = static_cast<DitingBattery *>(context);
        IOSimpleLockLock(self->ucsiLock);
        diting_battery_inbox_state(&self->ucsiInbox, up);
        IOSimpleLockUnlock(self->ucsiLock);
        self->ucsiEvent->interruptOccurred(nullptr, nullptr, 0);
    }
    static void ucsiReceive(void *context, const uint8_t *data, uint32_t length) {
        DitingBattery *self = static_cast<DitingBattery *>(context);
        IOSimpleLockLock(self->ucsiLock);
        diting_battery_inbox_push(&self->ucsiInbox, data, length);
        IOSimpleLockUnlock(self->ucsiLock);
        self->ucsiEvent->interruptOccurred(nullptr, nullptr, 0);
    }

    int sendBattmgr(const void *data, uint32_t length) {
        if (usePmicGlink && diting_pmic_glink_send)
            return diting_pmic_glink_send(DITING_PMIC_GLINK_OWNER_BATTMGR, data, length);
        if (channel && sheng_adsp_send) return sheng_adsp_send(channel, data, length);
        return -1;
    }
    int sendUcsi(const void *data, uint32_t length) {
        if (usePmicGlink && diting_pmic_glink_send)
            return diting_pmic_glink_send(DITING_PMIC_GLINK_OWNER_UCSI, data, length);
        return -1;
    }
    bool live() {
        IOSimpleLockLock(ringLock);
        const bool up = inbox.up && inbox.epoch == activeEpoch;
        IOSimpleLockUnlock(ringLock);
        return wasUp && up;
    }
    bool ucsiLive() {
        if (!ucsiRegistered) return false;
        IOSimpleLockLock(ucsiLock);
        const bool up = ucsiInbox.up && ucsiInbox.epoch == ucsiEpoch;
        IOSimpleLockUnlock(ucsiLock);
        return ucsiWasUp && up;
    }
    void updateUsbDataRole() {
        const int ucsiConn = latestUcsi.valid ? (latestUcsi.connected ? 1 : 0) : -1;
        const int ucsiData = diting_ucsi_status_usb_data_online(&latestUcsi);
        const int ucsiPdUsb = diting_ucsi_status_is_pd_usb_host(&latestUcsi);
        __atomic_store_n(&ditingUcsiDataRole, ucsiData, __ATOMIC_RELEASE);
        const int resolved = diting_usb_resolve_data_online(ucsiConn, ucsiData, ucsiPdUsb, lastBattUsbClass);
        __atomic_store_n(&ditingUsbDataRole, resolved, __ATOMIC_RELEASE);
        if (resolved != lastResolvedDataRole) {
            lastResolvedDataRole = resolved;
            IOLog("DitingBattery: usb_data_role=%d (batt_class=%d ucsi_conn=%d ucsi_data=%d ucsi_pd_usb=%d)\n",
                  resolved, lastBattUsbClass, ucsiConn, ucsiData, ucsiPdUsb);
        }
        IOSimpleLock *lock = ensureDiagLock();
        if (lock) {
            IOSimpleLockLock(lock);
            gDiag.ucsi = latestUcsi;
            gDiag.ucsi_updates = ucsi.updates;
            gDiag.ucsi_notifications = ucsi.notifications;
            gDiag.ucsi_errors = ucsi.errors + ucsi.timeouts;
            gDiag.usb_data_role = resolved;
            IOSimpleLockUnlock(lock);
        }
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
        if (sendBattmgr(out, sheng_battmgr_enable_request(out)))
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
        const uint32_t adap = bm.completed_valid & bit(BmUsbAdapter) ? (uint32_t)(bm.completed_value[BmUsbAdapter] & 0xff) : 0u;
        native_charger_stats[14] = (int32_t)((bm.writes_ok & 0xffu) | ((bm.writes_err & 0xffu) << 8) |
                                             (adap << 16) | ((chargerCtlMode & 0xffu) << 24));
        native_charger_stats[15] = bm.last_write_rc;
    }
    void updateDiagSnapshot(const ShengBatterySnapshot &s) {
        IOSimpleLock *lock = ensureDiagLock();
        if (!lock) return;
        IOSimpleLockLock(lock);
        gDiag.stage = native_battery_stats[0];
        gDiag.valid = s.charge_known;
        gDiag.percent = s.percent;
        gDiag.voltage_mv = s.voltage_mv;
        gDiag.current_ma = s.current_ma;
        gDiag.temperature = s.temperature;
        gDiag.temperature_valid = s.temperature_valid;
        gDiag.present = s.present;
        gDiag.external = s.external;
        gDiag.charging = s.charging;
        gDiag.full = s.fully_charged;
        gDiag.critical = s.critical;
        gDiag.usb_online = native_charger_stats[0];
        gDiag.usb_type = native_charger_stats[1];
        gDiag.usb_adapter = native_charger_stats[2];
        gDiag.usb_real_type = native_charger_stats[3];
        gDiag.icl_ma = native_charger_stats[4];
        gDiag.usb_max_ma = native_charger_stats[5];
        gDiag.vbus_mv = native_charger_stats[6];
        gDiag.ibus_ma = native_charger_stats[7];
        gDiag.thermal_level = native_charger_stats[10];
        gDiag.chg_cur_ma = native_charger_stats[11];
        gDiag.watts = native_charger_stats[12];
        gDiag.epoch = activeEpoch;
        gDiag.cycles = bm.cycles_done;
        gDiag.responses = bm.responses;
        gDiag.notifications = bm.notifications;
        gDiag.ctl_mode = chargerCtlMode;
        gDiag.writes_ok = bm.writes_ok;
        gDiag.writes_err = bm.writes_err;
        gDiag.last_write_rc = bm.last_write_rc;
        gDiag.ucsi = latestUcsi;
        gDiag.ucsi_updates = ucsi.updates;
        gDiag.ucsi_notifications = ucsi.notifications;
        gDiag.ucsi_errors = ucsi.errors + ucsi.timeouts;
        gDiag.usb_data_role = __atomic_load_n(&ditingUsbDataRole, __ATOMIC_ACQUIRE);
        IOSimpleLockUnlock(lock);
    }
    void evaluateChargerControl(const ShengBatterySnapshot &s) {
        if (!chargerCtlMode || !live() || !s.external || !bm.enabled) return;
        if (bm.write_pending || bm.write_in_flight) return;

        // Mode >= 1: Thermal mitigation control (BC_BATTERY_STATUS_SET 0x31, prop 10, level 0..3).
        // Matches PixelOS qti_battery_charger.c: track curr_thermal_level in software because
        // ADSP GET on BATT_CHG_CTRL_LIM may return FCC in uA rather than the 0..3 index.
        if (s.temperature_valid) {
            const uint32_t current = currentThermalLevel;
            const uint32_t desired = sheng_battmgr_thermal_level(s.temperature, current);
            const bool emergency = s.temperature >= 4500 && desired > current;
            if (desired != current && (emergency || refreshes >= lastThermalWriteTick + 2)) {
                if (sheng_battmgr_queue_write(&bm, SHENG_BATTMGR_OP_BATTERY_SET, SHENG_BATTMGR_PROP_BATT_THERMAL_LEVEL, desired)) {
                    pendingThermalLevel = desired;
                    pendingThermalWrite = true;
                    lastThermalWriteTick = refreshes;
                    IOLog("DitingBattery: charger_ctl thermal temp=%d.%02dC level %u->%u%s\n",
                          s.temperature / 100, (s.temperature < 0 ? -s.temperature : s.temperature) % 100,
                          current, desired, emergency ? " EMERGENCY" : "");
                    return;
                }
            }
        }

        // Mode >= 2: Bounded USB ICL coordination (BC_USB_STATUS_SET 0x33, prop 5) on SDP/CDP/PD only.
        if (chargerCtlMode >= 2 && s.temperature_valid && s.temperature < 4200 &&
            (bm.completed_valid & bit(BmUsbAdapter)) && (bm.completed_valid & bit(BmUsbLimit)) &&
            (bm.completed_valid & bit(BmUsbMax)) && refreshes >= lastIclWriteTick + 6) {
            const int adap = bm.completed_value[BmUsbAdapter];
            uint32_t capUa = 0;
            if (adap == 1) capUa = 500000u;                                 // SDP
            else if (adap == 3) capUa = 1500000u;                           // CDP
            else if (adap == 6 || adap == 7 || adap == 8) capUa = 3000000u; // PD / PD_DRP / PD_PPS
            if (capUa > 0) {
                const uint32_t usbMax = (uint32_t)(bm.completed_value[BmUsbMax] > 0 ? bm.completed_value[BmUsbMax] : 0);
                const uint32_t curIcl = (uint32_t)(bm.completed_value[BmUsbLimit] > 0 ? bm.completed_value[BmUsbLimit] : 0);
                uint32_t target = usbMax < capUa ? usbMax : capUa;
                if (target > SHENG_BATTMGR_ICL_MAX_UA) target = SHENG_BATTMGR_ICL_MAX_UA;
                if (target >= 500000u && target > curIcl + 100000u) {
                    if (sheng_battmgr_queue_write(&bm, SHENG_BATTMGR_OP_USB_SET, SHENG_BATTMGR_PROP_USB_ICL, target)) {
                        lastIclWriteTick = refreshes;
                        IOLog("DitingBattery: charger_ctl icl adap=%d(%s) %u->%u uA (max=%u)\n",
                              adap, usbTypeName(adap), curIcl, target, usbMax);
                    }
                }
            }
        }
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
        lastBattUsbClass = diting_battery_classify_usb_data(
            s.external, s.source_known,
            (bm.completed_valid & bit(BmUsbAdapter)) != 0, bm.completed_value[BmUsbAdapter],
            (bm.completed_valid & bit(BmUsbType)) != 0, bm.completed_value[BmUsbType],
            (bm.completed_valid & bit(BmRealType)) != 0, bm.completed_value[BmRealType]);
        updateUsbDataRole();
        recordRaw(s);
        number("DitingBatteryValidFields", bm.completed_valid);
        number("DitingBatteryFreshFields", bm.completed);
        number("DitingBatteryMissingFields", ~kUnusedFields & ~bm.completed_valid);
        number("DitingBatterySampleCycle", s.sample_cycle);
        number("DitingBatteryTransportEpoch", activeEpoch);
        number("DitingChargerCtlMode", chargerCtlMode);
        number("DitingChargerWritesOk", bm.writes_ok);
        number("DitingChargerWritesErr", bm.writes_err);
        number("DitingUsbDataOnline", __atomic_load_n(&ditingUsbDataRole, __ATOMIC_ACQUIRE));
        if (latestUcsi.valid) {
            flag("DitingUcsiConnected", latestUcsi.connected != 0);
            number("DitingUcsiOpmode", latestUcsi.pwr_opmode);
            number("DitingUcsiPartnerType", latestUcsi.partner_type);
            number("DitingUcsiPartnerFlags", latestUcsi.partner_flags);
        }
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
            updateDiagSnapshot(s);
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
        const bool weakDrainWarn = s.external && s.current_ma < 0 && s.percent <= 5;
        setAtWarnLevel(s.present && s.source_known && !power.source_pending &&
                       ((!s.external && s.percent <= 10) || weakDrainWarn));
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

        // Concise ramoops-friendly transition logging for wall-charger post-mortem.
        const uint32_t flags = s.external | s.charging << 1 | s.fully_charged << 2;
        const bool sourceChanged = flags != lastPowerFlags;
        if (sourceChanged) {
            lastPowerFlags = flags;
            IOLog("DitingBattery: source ext=%d chg=%d full=%d crit=%d pending=%d online=%d VBUS=%dmV batt=%d%%/%dmV/%dmA status=%d sub=%u\n",
                s.external, s.charging, s.fully_charged, s.critical, power.source_pending, native_charger_stats[0],
                native_charger_stats[6], s.percent, s.voltage_mv, s.current_ma, native_charger_stats[8], unsigned(bm.enabled));
            if (ucsiLive()) {
                diting_ucsi_refresh(&ucsi);
                ucsiPump();
            }
        }
        if (native_charger_stats[1] != lastLoggedUsbType || native_charger_stats[2] != lastLoggedAdapter ||
            native_charger_stats[3] != lastLoggedRealType || native_charger_stats[4] != lastLoggedIclMa) {
            lastLoggedUsbType = native_charger_stats[1];
            lastLoggedAdapter = native_charger_stats[2];
            lastLoggedRealType = native_charger_stats[3];
            lastLoggedIclMa = native_charger_stats[4];
            IOLog("DitingBattery: usb type=%d(%s) adap=%d(%s) real=%d(%s) icl=%dmA max=%dmA vbus=%dmV ibus=%dmA chg_i=%dmA batt_i=%dmA\n",
                lastLoggedUsbType, usbTypeName(lastLoggedUsbType),
                lastLoggedAdapter, usbTypeName(lastLoggedAdapter),
                lastLoggedRealType, usbTypeName(lastLoggedRealType),
                lastLoggedIclMa, native_charger_stats[5], native_charger_stats[6], native_charger_stats[7],
                native_charger_stats[11], s.current_ma);
        }
        if (s.external && (bm.completed_valid & bit(BmCurrent))) {
            const int sign = s.current_ma >= 20 ? 1 : (s.current_ma <= -20 ? -1 : lastCurrentSign);
            if (sign != 0 && sign != lastCurrentSign) {
                lastCurrentSign = sign;
                IOLog("DitingBattery: net current %s (%dmA) %d%% %dmV vbus=%dmV ibus=%dmA icl=%dmA\n",
                    sign > 0 ? "CHARGING" : "DRAINING", s.current_ma, s.percent, s.voltage_mv,
                    native_charger_stats[6], native_charger_stats[7], native_charger_stats[4]);
            }
        } else if (!s.external) {
            lastCurrentSign = 0;
        }

        evaluateChargerControl(s);
        updateDiagSnapshot(s);

        // updateStatus copies the properties to IORegistry before matching
        // can find this service for the first time.
        updateStatus();
        if (!published) {
            published = true;
            native_battery_stats[0] = 4;
            updateDiagSnapshot(s);
            registerService();
            IOLog("DitingBattery: physical power source published %d%% %dmV epoch=%u ctl=%u\n",
                  s.percent, s.voltage_mv, activeEpoch, chargerCtlMode);
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
            if (!sendBattmgr(out, n)) {
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
        if (result == BmWriteDone) {
            if (pendingThermalWrite) {
                if (bm.last_write_rc == 0) currentThermalLevel = pendingThermalLevel;
                pendingThermalWrite = false;
            }
            IOLog("DitingBattery: charger write done rc=%d ok=%u err=%u therm_lvl=%u\n",
                  bm.last_write_rc, bm.writes_ok, bm.writes_err, currentThermalLevel);
        }
        if (result == BmSnapshotReady || result == BmCycleDone) publish();
        counters();
        pump();
    }
    void ucsiPump() {
        if (!ucsiLive()) return;
        for (unsigned tries = 0; tries < 8 && ucsiLive(); ++tries) {
            uint8_t out[DITING_UCSI_WRITE_REQ_BYTES];
            const uint32_t n = diting_ucsi_next(&ucsi, out);
            if (!n) return;
            if (!sendUcsi(out, n)) {
                ucsiTimer->setTimeoutMS(kUcsiRequestMs);
                return;
            }
            diting_ucsi_send_failed(&ucsi);
        }
    }
    void ucsiArmWaitTimer() {
        if (!ucsiLive()) return;
        // If UCSI is waiting for a NOTIFY_IND after WRITE_RESP, use a shorter fallback timer.
        // State indices 3 (UcsiWaitEnableNotify) and 10 (UcsiWaitStatusNotify) fall through to READ_BUF_REQ.
        if (ucsi.state == 3 || ucsi.state == 10) ucsiTimer->setTimeoutMS(kUcsiNotifyWaitMs);
        else if (ucsi.state != 0) ucsiTimer->setTimeoutMS(kUcsiRequestMs);
        else ucsiTimer->cancelTimeout();
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
            self->currentThermalLevel = self->pendingThermalLevel = 0;
            self->pendingThermalWrite = false;
            self->lastThermalWriteTick = self->lastIclWriteTick = 0;
            self->lastBattUsbClass = DitingUsbClassUnknown;
            sheng_battmgr_init(&self->bm);
            diting_battery_new_transport(&self->power);
            ShengBatterySnapshot unknown;
            sheng_battmgr_snapshot(&self->bm, &unknown);
            self->recordRaw(unknown);
            self->counters();
            __atomic_store_n(&ditingUsbCable, -1, __ATOMIC_RELEASE);
            self->updateUsbDataRole();
            self->updateDiagSnapshot(unknown);
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
            if (result == BmAnswer || result == BmSnapshotReady || result == BmCycleDone || result == BmWriteDone)
                self->request->cancelTimeout();
            self->handle(result);
        }
        IOSimpleLockLock(self->ringLock);
        const bool pending = self->inbox.count || self->inbox.changed;
        IOSimpleLockUnlock(self->ringLock);
        if (pending) self->inboxEvent->interruptOccurred(nullptr, nullptr, 0);
    }
    static void ucsiAction(OSObject *owner, IOInterruptEventSource *, int) {
        DitingBattery *self = OSDynamicCast(DitingBattery, owner);
        if (!self || !self->ucsiRegistered) return;
        IOSimpleLockLock(self->ucsiLock);
        const bool up = self->ucsiInbox.up;
        const uint32_t epoch = self->ucsiInbox.epoch;
        const bool changed = self->ucsiInbox.changed;
        self->ucsiInbox.changed = 0;
        IOSimpleLockUnlock(self->ucsiLock);
        if (changed && (epoch != self->ucsiEpoch || up != self->ucsiWasUp)) {
            self->ucsiTimer->cancelTimeout();
            self->ucsiEpoch = epoch;
            self->ucsiWasUp = up;
            diting_ucsi_init(&self->ucsi);
            diting_ucsi_snapshot(&self->ucsi, &self->latestUcsi);
            self->lastUcsiSig = -1;
            self->updateUsbDataRole();
            if (up) {
                diting_ucsi_start(&self->ucsi);
                self->ucsiPump();
                self->ucsiArmWaitTimer();
            }
        }
        for (unsigned handled = 0; handled < kDitingBatteryRing; ++handled) {
            uint8_t data[kDitingBatteryMessage];
            IOSimpleLockLock(self->ucsiLock);
            const bool current = self->ucsiInbox.up && self->ucsiInbox.epoch == self->ucsiEpoch;
            const uint32_t length = current ? diting_battery_inbox_pop(&self->ucsiInbox, data) : 0;
            IOSimpleLockUnlock(self->ucsiLock);
            if (!length) break;
            const int result = diting_ucsi_receive(&self->ucsi, data, length);
            if (result == UcsiStatusUpdated) {
                diting_ucsi_snapshot(&self->ucsi, &self->latestUcsi);
                const int sig = (self->latestUcsi.connected << 16) | (self->latestUcsi.pwr_opmode << 12) |
                                (self->latestUcsi.partner_type << 8) | self->latestUcsi.partner_flags;
                if (sig != self->lastUcsiSig) {
                    self->lastUcsiSig = sig;
                    IOLog("DitingUcsi: conn=%u op=%u(%s) dir=%u partner=%u(%s) flags=0x%x rdo=0x%08x(%u/%umA)\n",
                          unsigned(self->latestUcsi.connected), unsigned(self->latestUcsi.pwr_opmode),
                          ucsiOpmodeName(self->latestUcsi.pwr_opmode), unsigned(self->latestUcsi.pwr_dir),
                          unsigned(self->latestUcsi.partner_type), ucsiPartnerName(self->latestUcsi.partner_type),
                          unsigned(self->latestUcsi.partner_flags), self->latestUcsi.rdo,
                          unsigned(self->latestUcsi.rdo_op_ma), unsigned(self->latestUcsi.rdo_max_ma));
                }
                self->updateUsbDataRole();
            }
            if (result != UcsiIgnored) {
                self->ucsiTimer->cancelTimeout();
                self->ucsiPump();
                self->ucsiArmWaitTimer();
            }
        }
        IOSimpleLockLock(self->ucsiLock);
        const bool pending = self->ucsiInbox.count || self->ucsiInbox.changed;
        IOSimpleLockUnlock(self->ucsiLock);
        if (pending) self->ucsiEvent->interruptOccurred(nullptr, nullptr, 0);
    }
    static void requestAction(OSObject *owner, IOTimerEventSource *) {
        DitingBattery *self = OSDynamicCast(DitingBattery, owner);
        if (self && self->live()) self->handle(sheng_battmgr_timeout(&self->bm));
    }
    static void ucsiTimerAction(OSObject *owner, IOTimerEventSource *) {
        DitingBattery *self = OSDynamicCast(DitingBattery, owner);
        if (!self || !self->ucsiLive()) return;
        diting_ucsi_timeout(&self->ucsi);
        self->ucsiPump();
        self->ucsiArmWaitTimer();
    }
    static void refreshAction(OSObject *owner, IOTimerEventSource *sender) {
        DitingBattery *self = OSDynamicCast(DitingBattery, owner);
        if (!self || !self->live()) return;
        self->subscribe();
        sheng_battmgr_refresh(&self->bm, ++self->refreshes % kSlowEvery == 0);
        self->handle(BmIgnored);
        if (self->ucsiLive() && (self->refreshes % 2 == 0)) {
            diting_ucsi_refresh(&self->ucsi);
            self->ucsiPump();
            self->ucsiArmWaitTimer();
        }
        sender->setTimeoutMS(kRefreshMs);
    }
    void releaseSources() {
        IOTimerEventSource *sources[] = {request, refresh, ucsiTimer};
        for (IOTimerEventSource *source : sources) if (source) {
            source->cancelTimeout();
            if (loop && source->getWorkLoop() == loop) loop->removeEventSource(source);
        }
        if (loop && inboxEvent && inboxEvent->getWorkLoop() == loop) loop->removeEventSource(inboxEvent);
        if (loop && ucsiEvent && ucsiEvent->getWorkLoop() == loop) loop->removeEventSource(ucsiEvent);
        OSSafeReleaseNULL(request); OSSafeReleaseNULL(refresh); OSSafeReleaseNULL(ucsiTimer);
        OSSafeReleaseNULL(inboxEvent); OSSafeReleaseNULL(ucsiEvent);
        OSSafeReleaseNULL(loop);
        if (ringLock) {IOSimpleLockFree(ringLock); ringLock = nullptr;}
        if (ucsiLock) {IOSimpleLockFree(ucsiLock); ucsiLock = nullptr;}
    }
    static IOReturn registerChannel(OSObject *owner, void *, void *, void *, void *) {
        DitingBattery *self = OSDynamicCast(DitingBattery, owner);
        if (!self) return kIOReturnBadArgument;
        if (diting_pmic_glink_register && diting_pmic_glink_send) {
            const DitingPmicGlinkClient battClient = {&DitingBattery::dspState, &DitingBattery::dspReceive};
            if (diting_pmic_glink_register(DITING_PMIC_GLINK_OWNER_BATTMGR, &battClient, self) != 0)
                return kIOReturnNoResources;
            self->usePmicGlink = true;
            const DitingPmicGlinkClient ucsiClient = {&DitingBattery::ucsiState, &DitingBattery::ucsiReceive};
            if (diting_pmic_glink_register(DITING_PMIC_GLINK_OWNER_UCSI, &ucsiClient, self) == 0) {
                self->ucsiRegistered = true;
            } else {
                IOLog("DitingBattery: UCSI owner registration failed (continuing with Battmgr only)\n");
            }
            native_battery_stats[0] = 2;
            return kIOReturnSuccess;
        }
        const ShengAdspClient client = {&DitingBattery::dspState, &DitingBattery::dspReceive};
        self->channel = sheng_adsp_channel("PMIC_RTR_ADSP_APPS", 1024, 4, 0, &client, self);
        if (!self->channel) return kIOReturnNoResources;
        native_battery_stats[0] = 2;
        return kIOReturnSuccess;
    }

public:
    bool startBattery() {
        sheng_battmgr_init(&bm);
        diting_ucsi_init(&ucsi);
        chargerCtlMode = 0;
        PE_parse_boot_argn("diting_charger_ctl", &chargerCtlMode, sizeof(chargerCtlMode));
        if (chargerCtlMode > 2) chargerCtlMode = 2;
        if ((!diting_pmic_glink_register || !diting_pmic_glink_send) && (!sheng_adsp_channel || !sheng_adsp_send))
            return false;
        loop = IOWorkLoop::workLoop();
        ringLock = IOSimpleLockAlloc();
        ucsiLock = IOSimpleLockAlloc();
        inboxEvent = IOInterruptEventSource::interruptEventSource(this, &DitingBattery::inboxAction);
        ucsiEvent = IOInterruptEventSource::interruptEventSource(this, &DitingBattery::ucsiAction);
        request = IOTimerEventSource::timerEventSource(this, &DitingBattery::requestAction);
        refresh = IOTimerEventSource::timerEventSource(this, &DitingBattery::refreshAction);
        ucsiTimer = IOTimerEventSource::timerEventSource(this, &DitingBattery::ucsiTimerAction);
        if (!loop || !ringLock || !ucsiLock || !inboxEvent || !ucsiEvent || !request || !refresh || !ucsiTimer ||
            loop->addEventSource(inboxEvent) != kIOReturnSuccess ||
            loop->addEventSource(ucsiEvent) != kIOReturnSuccess ||
            loop->addEventSource(request) != kIOReturnSuccess ||
            loop->addEventSource(refresh) != kIOReturnSuccess ||
            loop->addEventSource(ucsiTimer) != kIOReturnSuccess) {
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
