/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef DITING_BATTERY_STATE_H
#define DITING_BATTERY_STATE_H
#include "DitingBattmgr.h"

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
    /* Apply warning policy after source resolution, including debounce.
     * Under weak-USB net drain (external=1, current_ma<0), still warn/critical
     * so userspace can save state before the pack hits undervoltage. */
    {
        const int weak_drain = s->external && s->current_ma < 0 &&
            (s->percent <= 1 || (s->voltage_mv > 0 && s->voltage_mv < 3450));
        s->critical = s->present && s->source_known && !state->source_pending &&
            ((!s->external && s->percent <= 2) || weak_drain);
    }
    state->external = s->external;
    state->present = s->present;
    state->charging = s->charging;
    state->fully_charged = s->fully_charged;
    state->published = 1;
    return 1;
}

/* POWER_SUPPLY_USB_TYPE + Xiaomi HVDCP classification for USB gadget gating.
 * 1 = SDP (data), 2 = DCP (pure charger), 3 = CDP (data), 4 = ACA,
 * 5 = TYPE_C, 6 = PD, 7 = PD_DRP, 8 = PD_PPS, 9 = APPLE_BRICK_ID,
 * 0x80..0x84 = Xiaomi HVDCP/FLOAT (pure chargers). */
enum {
    DitingUsbClassUnknown = -1,
    DitingUsbClassCharger = 0,
    DitingUsbClassDataHost = 1,
    DitingUsbClassPdOrTypeC = 2,
};

static inline int diting_battery_classify_usb_type(int usb_type)
{
    switch (usb_type) {
    case 1: /* SDP */
    case 3: /* CDP */
        return DitingUsbClassDataHost;
    case 2: /* DCP */
    case 4: /* ACA */
    case 9: /* APPLE_BRICK_ID */
    case 0x80: /* HVDCP2 */
    case 0x81: /* HVDCP3 */
    case 0x82: /* HVDCP3P5 */
    case 0x83: /* USB_FLOAT */
    case 0x84: /* HVDCP3_CLASSB */
        return DitingUsbClassCharger;
    case 5: /* TYPE_C */
    case 6: /* PD */
    case 7: /* PD_DRP */
    case 8: /* PD_PPS */
        return DitingUsbClassPdOrTypeC;
    default:
        return DitingUsbClassUnknown;
    }
}

static inline int diting_battery_classify_usb_data(int external, int source_known,
                                                   int adapter_valid, int adapter_type,
                                                   int usb_type_valid, int usb_type,
                                                   int real_type_valid, int real_type)
{
    if (source_known && !external) return DitingUsbClassCharger;
    const int c_real = real_type_valid ? diting_battery_classify_usb_type(real_type) : DitingUsbClassUnknown;
    const int c_adap = adapter_valid ? diting_battery_classify_usb_type(adapter_type) : DitingUsbClassUnknown;
    const int c_type = usb_type_valid ? diting_battery_classify_usb_type(usb_type) : DitingUsbClassUnknown;
    /* Any explicit wall charger reading (DCP / HVDCP / APPLE_BRICK_ID) wins immediately. */
    if (c_real == DitingUsbClassCharger || c_adap == DitingUsbClassCharger || c_type == DitingUsbClassCharger)
        return DitingUsbClassCharger;
    /* Explicit BC1.2 data host (SDP or CDP). */
    if (c_real == DitingUsbClassDataHost || c_adap == DitingUsbClassDataHost || c_type == DitingUsbClassDataHost)
        return DitingUsbClassDataHost;
    /* USB-C / PD: ambiguous until UCSI confirms USB Communications Capable. */
    if (c_real == DitingUsbClassPdOrTypeC || c_adap == DitingUsbClassPdOrTypeC || c_type == DitingUsbClassPdOrTypeC)
        return DitingUsbClassPdOrTypeC;
    return DitingUsbClassUnknown;
}

/* Combines UCSI connector status with Battmgr APSD classification into the
 * final USB gadget data-host decision: 1 = enable gadget, 0 = leave PHY alone,
 * -1 = still unknown / pending. */
static inline int diting_usb_resolve_data_online(int ucsi_connected, int ucsi_usb_data, int ucsi_pd_usb, int batt_class)
{
    if (ucsi_connected == 0) return 0;
    if (batt_class == DitingUsbClassCharger) return 0;
    if (batt_class == DitingUsbClassDataHost) {
        /* Battmgr APSD detected real SDP/CDP on D+/D-. Allow USB gadget even
         * if UCSI omits the PD USB-communications flag on non-PD hubs/ports. */
        return 1;
    }
    if (batt_class == DitingUsbClassPdOrTypeC) {
        if (ucsi_pd_usb == 1 || ucsi_usb_data == 1) return 1;
        return 0;
    }
    /* Battmgr classification unknown/pending: accept explicit UCSI disconnect,
     * non-USB PD charger, or confirmed PD USB host; otherwise wait for APSD. */
    if (ucsi_usb_data == 0) return 0;
    if (ucsi_pd_usb == 1) return 1;
    return -1;
}
#endif
