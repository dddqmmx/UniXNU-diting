/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "DitingBattmgr.h"
#include "DitingBatteryInbox.h"
#include "DitingBatteryState.h"
#include "DitingPmicGlink.h"
#include "DitingUcsi.h"

static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void response(uint8_t *out, unsigned opcode, unsigned property, int32_t value, unsigned result)
{
    put32(out, 32778); put32(out + 4, 1); put32(out + 8, opcode);
    put32(out + 12, property); put32(out + 16, (uint32_t)value); put32(out + 20, result);
}

/* Literal wire IDs and units from Xiaomi's public qti_battery_charger
 * contract. Tests do not obtain expected IDs from the driver's table. */
typedef struct {unsigned opcode, property; int32_t value;} Reading;
static const Reading readings[] = {
    {0x30, 0, 1}, {0x30, 2, 1}, {0x30, 4, 7345}, {0x30, 7, 4020000},
    {0x30, 9, -1234000}, {0x30, 13, 2750}, {0x32, 0, 1}, {0x32, 1, 9000000},
    {0x30, 15, 3421}, {0x30, 21, 3600}, {0x30, 20, 42}, {0x30, 18, 4567000},
    {0x30, 17, 5000000}, {0x30, 16, 87}, {0x32, 7, 8}, {0x32, 6, 9},
    {0x32, 3, 1800000}, {0x32, 5, 2000000}, {0x32, 4, 3000000},
    {0x30, 3, 3}, {0x30, 10, 0}, {0x30, 12, 2000000}, {0x32, 11, 9},
};

static Reading reading_for(const uint8_t *request)
{
    assert(get32(request) == 32778 && get32(request + 4) == 1);
    assert(get32(request + 12) == 0 && get32(request + 20) == 0);
    assert(get32(request + 8) == 0x30 || get32(request + 8) == 0x32);
    for (unsigned i = 0; i < sizeof readings / sizeof readings[0]; ++i)
        if (readings[i].opcode == get32(request + 8) && readings[i].property == get32(request + 16)) return readings[i];
    assert(!"Unexpected property, board extension, or charger write");
    return readings[0];
}

static int answer(ShengBattmgr *b, Reading r, unsigned result)
{
    uint8_t request[24], extra[24], reply[24];
    assert(sheng_battmgr_next(b, request) == 24);
    Reading actual = reading_for(request);
    assert(actual.opcode == r.opcode && actual.property == r.property);
    assert(sheng_battmgr_next(b, extra) == 0); /* one request in flight */
    response(reply, r.opcode, r.property, r.value, result);
    return sheng_battmgr_receive(b, reply, sizeof reply);
}

/* Complete one cycle, optionally replacing a literal wire reading. */
static unsigned cycle(ShengBattmgr *b, int slow, unsigned opcode, unsigned property, int32_t value, unsigned error)
{
    sheng_battmgr_refresh(b, slow);
    unsigned count = 0;
    for (; count < 32; ++count) {
        uint8_t request[24], reply[24];
        assert(sheng_battmgr_next(b, request) == 24);
        Reading r = reading_for(request);
        if (r.opcode == opcode && r.property == property) r.value = value;
        response(reply, r.opcode, r.property, r.value,
            r.opcode == opcode && r.property == property ? error : 0);
        const int result = sheng_battmgr_receive(b, reply, sizeof reply);
        if (result == BmCycleDone) return count + 1;
        assert(result == BmAnswer || result == BmSnapshotReady);
    }
    assert(!"Unbounded cycle");
    return count;
}

static void test_wire(void)
{
    ShengBattmgr b; ShengBatterySnapshot s;
    uint8_t enable[28];
    sheng_battmgr_init(&b);
    sheng_battmgr_snapshot(&b, &s);
    assert(!s.charge_known && !s.present && !s.critical);
    assert(sheng_battmgr_enable_request(enable) == 28);
    assert(get32(enable) == 32778 && get32(enable + 4) == 2 && get32(enable + 8) == 4);
    for (unsigned i = 12; i < 28; ++i) assert(enable[i] == 0);
    sheng_battmgr_refresh(&b, 1);
    for (unsigned i = 0; i < 8; ++i) {
        const int result = answer(&b, readings[i], 0);
        assert(result == (i == 7 ? BmSnapshotReady : BmAnswer));
    }
    sheng_battmgr_snapshot(&b, &s);
    assert(s.charge_known && s.present && s.percent == 73 && s.external && s.charging);
    assert(s.voltage_mv == 4020 && s.current_ma == 1234 && s.temperature == 2750 && s.temperature_valid);
    assert(s.full_mah == 0 && s.time_to_full_min == -1 && b.cycles_done == 0);
    for (unsigned i = 8; i < sizeof readings / sizeof readings[0]; ++i) {
        const int result = answer(&b, readings[i], 0);
        assert(result == (i + 1 == sizeof readings / sizeof readings[0] ? BmCycleDone : BmAnswer));
    }
    sheng_battmgr_snapshot(&b, &s);
    assert(s.raw_mah == 3421 && s.full_mah == 4567 && s.design_mah == 5000 && s.cycles == 87);
    assert(s.time_to_full_min == 42 && s.time_to_empty_min == 60);
    assert(s.adapter_family == 0xe000400a && s.adapter_mv == 9000 && s.adapter_ma == 2000 && s.adapter_watts == 18);
    assert(s.sample_cycle == 1 && b.cycles_done == 1);
    uint8_t request[24];
    assert(sheng_battmgr_next(&b, request) == 0);
    assert(!(b.valid & ((1u << BmGauge1Remaining) | (1u << BmPowerMax) | (1u << BmPdVerified))));
}

static void test_frames(void)
{
    ShengBattmgr b; uint8_t request[24], good[64], bad[64];
    sheng_battmgr_init(&b); sheng_battmgr_refresh(&b, 1);
    assert(sheng_battmgr_next(&b, request) == 24);
    memset(good, 0, sizeof good); response(good, 0x30, 0, 1, 0);
    for (unsigned length = 0; length <= 64; ++length) if (length != 24) {
        assert(sheng_battmgr_receive(&b, good, length) == BmIgnored);
        assert(b.in_flight && !b.valid && !b.responses);
    }
    assert(sheng_battmgr_receive(&b, NULL, 24) == BmIgnored);
    const unsigned offsets[] = {0, 4, 8, 12};
    const uint32_t wrong[] = {32779, 2, 0x32, 4};
    for (unsigned i = 0; i < 4; ++i) {
        memcpy(bad, good, sizeof good); put32(bad + offsets[i], wrong[i]);
        assert(sheng_battmgr_receive(&b, bad, 24) == BmIgnored);
        assert(b.in_flight && !b.valid);
    }
    assert(sheng_battmgr_receive(&b, good, 24) == BmAnswer);
    assert(sheng_battmgr_receive(&b, good, 24) == BmIgnored); /* duplicate */
    uint8_t ack[28] = {0}; put32(ack, 32778); put32(ack + 4, 1); put32(ack + 8, 4);
    assert(sheng_battmgr_receive(&b, ack, 11) == BmIgnored);
    assert(sheng_battmgr_receive(&b, ack, 24) == BmIgnored);
    assert(sheng_battmgr_receive(&b, ack, 12) == BmEnabled && b.enabled);
}

static void test_ranges(void)
{
    const struct {unsigned opcode, property, field; int32_t invalid;} bad[] = {
        {0x30, 4, BmCapacity, -1}, {0x30, 4, BmCapacity, INT_MAX}, {0x30, 4, BmCapacity, 10001},
        {0x30, 13, BmTemperature, -4001}, {0x30, 13, BmTemperature, 9001},
        {0x30, 9, BmCurrent, INT_MIN}, {0x30, 7, BmVoltage, -1},
        {0x30, 15, BmCharge, INT_MAX}, {0x30, 18, BmFull, -1}, {0x30, 18, BmFull, 0},
        {0x30, 20, BmTimeToFull, 1093}, {0x30, 21, BmTimeToEmpty, -1},
        {0x32, 0, BmUsbOnline, -1}, {0x32, 0, BmUsbOnline, 2}, {0x32, 1, BmUsbVoltage, INT_MAX},
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        ShengBattmgr b; ShengBatterySnapshot s; sheng_battmgr_init(&b);
        cycle(&b, 1, bad[i].opcode, bad[i].property, bad[i].invalid, 0);
        assert(!(b.completed_valid & (1u << bad[i].field)) && b.errors == 1);
        sheng_battmgr_snapshot(&b, &s);
        if (bad[i].field == BmCapacity) assert(!s.charge_known && !s.critical);
    }
    const int temperatures[] = {-4000, -250, 0, 9000};
    for (unsigned i = 0; i < sizeof temperatures / sizeof temperatures[0]; ++i) {
        ShengBattmgr b; ShengBatterySnapshot s; sheng_battmgr_init(&b);
        cycle(&b, 1, 0x30, 13, temperatures[i], 0);
        sheng_battmgr_snapshot(&b, &s);
        assert(s.temperature_valid && s.temperature == temperatures[i]);
    }
    ShengBattmgr b; ShengBatterySnapshot s; sheng_battmgr_init(&b);
    cycle(&b, 1, 0x30, 9, 1234000, 0); sheng_battmgr_snapshot(&b, &s);
    assert(s.current_ma == -1234);
    cycle(&b, 1, 0x30, 4, 7350, 0); sheng_battmgr_snapshot(&b, &s);
    assert(s.percent == 74);
    cycle(&b, 1, 0x32, 7, 9, 0); sheng_battmgr_snapshot(&b, &s);
    assert(s.adapter_family == 0xe0004004 && (b.completed_valid & (1u << BmUsbAdapter)));
    cycle(&b, 1, 0x30, 3, 5, 0);
    assert(b.completed_valid & (1u << BmChargeType)); /* adaptive charging */
}

static void test_timeouts(void)
{
    ShengBattmgr b; uint8_t request[24], late[24]; sheng_battmgr_init(&b);
    assert(sheng_battmgr_timeout(&b) == BmIgnored);
    sheng_battmgr_refresh(&b, 1);
    assert(answer(&b, readings[0], 0) == BmAnswer);
    assert(answer(&b, readings[1], 0) == BmAnswer);
    assert(sheng_battmgr_next(&b, request) == 24 && get32(request + 16) == 4);
    assert(sheng_battmgr_timeout(&b) == BmAnswer && b.last_timeout == BmCapacity && b.timeouts == 1);
    assert(sheng_battmgr_next(&b, request) == 24 && get32(request + 16) == 7);
    response(late, 0x30, 4, 8000, 0);
    assert(sheng_battmgr_receive(&b, late, 24) == BmIgnored && b.in_flight);
    assert(sheng_battmgr_send_failed(&b) == BmAnswer && b.errors == 1 && b.timeouts == 1);
    assert(sheng_battmgr_send_failed(&b) == BmIgnored);
    for (unsigned i = 4; i < 8; ++i) answer(&b, readings[i], 0);
    ShengBatterySnapshot s; sheng_battmgr_snapshot(&b, &s);
    assert(!s.charge_known && !(b.completed_valid & (1u << BmVoltage)) && !s.critical);
}

static void test_optional(void)
{
    ShengBattmgr b; ShengBatterySnapshot s; sheng_battmgr_init(&b);
    assert(cycle(&b, 1, 0x30, 18, 0, 1) == 23); /* unsupported FULL */
    sheng_battmgr_snapshot(&b, &s);
    assert(s.charge_known && s.percent == 73 && s.full_mah == 0);
    assert(b.deferred & (1u << BmFull));
    assert(cycle(&b, 0, 0, 0, 0, 0) == 20); /* no repeated slow queries */
    assert(cycle(&b, 1, 0, 0, 0, 0) == 23); /* explicit retry */
    assert(!(b.deferred & (1u << BmFull)));
    cycle(&b, 0, 0x30, 20, 0, 1); /* unsupported optional TTF */
    assert(b.deferred & (1u << BmTimeToFull));
    assert(cycle(&b, 0, 0, 0, 0, 0) == 19);
}

static void notification(uint8_t out[16], unsigned reason)
{
    put32(out, 32778); put32(out + 4, 2); put32(out + 8, 7); put32(out + 12, reason);
}
static void test_notifications(void)
{
    ShengBattmgr b; uint8_t n[16], request[24]; sheng_battmgr_init(&b);
    sheng_battmgr_refresh(&b, 1); assert(sheng_battmgr_next(&b, request) == 24);
    sheng_battmgr_refresh(&b, 1);
    notification(n, 0x32);
    for (unsigned i = 0; i < 100; ++i) assert(sheng_battmgr_receive(&b, n, 16) == BmNotified);
    assert(b.in_flight && b.refresh_wanted == 2 && b.notifications == 100);
    notification(n, 0x79); assert(sheng_battmgr_receive(&b, n, 16) == BmIgnored);
    notification(n, 0x30); assert(sheng_battmgr_receive(&b, n, 15) == BmIgnored);
    put32(n + 4, 1); assert(sheng_battmgr_receive(&b, n, 16) == BmIgnored);
    uint8_t reply[24]; response(reply, 0x30, 0, 1, 0);
    assert(sheng_battmgr_receive(&b, reply, 24) == BmAnswer);
    for (unsigned i = 1; i < sizeof readings / sizeof readings[0]; ++i) answer(&b, readings[i], 0);
    assert(b.cycles_done == 1 && b.slow_due && !b.refresh_wanted && !b.in_flight);
    assert(sheng_battmgr_next(&b, request) == 24); /* exactly one deferred cycle */
}

static void test_immutable(void)
{
    ShengBattmgr b; ShengBatterySnapshot before, after; sheng_battmgr_init(&b);
    cycle(&b, 1, 0, 0, 0, 0); sheng_battmgr_snapshot(&b, &before);
    sheng_battmgr_refresh(&b, 0);
    Reading changed = readings[0]; changed.value = 2; answer(&b, changed, 0);
    changed = readings[1]; answer(&b, changed, 0);
    changed = readings[2]; changed.value = 1200; answer(&b, changed, 0);
    sheng_battmgr_snapshot(&b, &after);
    assert(memcmp(&before, &after, sizeof before) == 0);
    for (unsigned i = 3; i < 8; ++i) answer(&b, readings[i], 0);
    sheng_battmgr_snapshot(&b, &after);
    assert(after.percent == 12 && after.sample_cycle == 2 && before.percent == 73);
}

static ShengBatterySnapshot source_sample(unsigned cycle_id, int percent, int external, int charging, int full)
{
    ShengBatterySnapshot s = {0};
    s.sample_cycle = cycle_id; s.percent = percent;
    s.present = s.present_known = s.charge_known = s.source_known = s.status_known = 1;
    s.external = external; s.charging = charging; s.fully_charged = full;
    return s;
}
static void test_source(void)
{
    DitingBatteryState state = {0};
    ShengBatterySnapshot s = source_sample(1, 50, 1, 1, 0);
    assert(diting_battery_resolve(&state, &s) && s.external && s.charging);
    s = source_sample(2, 1, 0, 0, 0);
    assert(diting_battery_resolve(&state, &s) && state.source_pending && s.external && !s.critical);
    s = source_sample(2, 1, 0, 0, 0); /* optional update of same cycle */
    diting_battery_resolve(&state, &s);
    assert(state.source_pending && s.external && !s.critical);
    s = source_sample(3, 1, 0, 0, 0); diting_battery_resolve(&state, &s);
    assert(!state.source_pending && !s.external && !s.charging && s.critical);
    s = source_sample(4, 1, 1, 1, 0); diting_battery_resolve(&state, &s);
    assert(state.source_pending && !s.critical && !s.external);
    s = source_sample(5, 1, 0, 0, 0); s.source_known = 0; diting_battery_resolve(&state, &s);
    assert(!state.candidate_valid && !s.critical && !s.external);
    s = source_sample(6, 1, 1, 1, 0); diting_battery_resolve(&state, &s);
    assert(state.source_pending && !s.external); /* failed read broke confirmations */
    s = source_sample(7, 100, 1, 1, 0); diting_battery_resolve(&state, &s);
    assert(s.external && s.charging && !s.fully_charged);
    s = source_sample(8, 100, 1, 0, 1); diting_battery_resolve(&state, &s);
    assert(s.external && !s.charging && s.fully_charged);
    s = source_sample(9, 99, 0, 0, 0); s.source_known = s.status_known = 0;
    diting_battery_resolve(&state, &s);
    assert(s.external && s.fully_charged && !s.charging);
    diting_battery_new_transport(&state);
    assert(state.external && state.fully_charged && !state.candidate_valid);
    s = source_sample(1, 99, 1, 0, 0); s.present = 0;
    diting_battery_resolve(&state, &s);
    assert(!state.present && !s.charging && !s.fully_charged);
    s = source_sample(2, 99, 1, 1, 0); s.present_known = 0;
    diting_battery_resolve(&state, &s);
    assert(!s.present && !s.charging); /* missing PRESENT is not reinsertion */
}

static void test_empty(void)
{
    ShengBattmgr b; ShengBatterySnapshot s; DitingBatteryState state = {0};
    sheng_battmgr_init(&b); sheng_battmgr_refresh(&b, 1);
    for (unsigned i = 0; i < 8; ++i) {
        Reading r = readings[i];
        if (i == 0) r.value = 2;
        if (i == 2 || i == 6 || i == 7) r.value = 0;
        answer(&b, r, 0);
    }
    sheng_battmgr_snapshot(&b, &s);
    assert(s.charge_known && s.present && s.percent == 0 && s.critical && !s.external);
    assert(diting_battery_resolve(&state, &s) && s.critical);
    sheng_battmgr_init(&b); sheng_battmgr_snapshot(&b, &s);
    assert(!diting_battery_resolve(&state, &s)); /* unknown is not empty */
    sheng_battmgr_refresh(&b, 1);
    for (unsigned i = 0; i < 8; ++i) {
        Reading r = readings[i];
        if (i == 2) r.value = 10000;
        if (i == 6) r.value = 0;
        answer(&b, r, 0);
    }
    sheng_battmgr_snapshot(&b, &s);
    assert(s.external && s.charging && !s.fully_charged); /* rounded 100 is not FULL */
}

static void test_inbox(void)
{
    DitingBatteryInbox inbox = {0}; uint8_t data[65], out[64];
    memset(data, 0x42, sizeof data);
    assert(!diting_battery_inbox_push(&inbox, data, 24));
    diting_battery_inbox_state(&inbox, 1);
    const uint32_t old = inbox.epoch;
    assert(diting_battery_inbox_push(&inbox, data, 24));
    diting_battery_inbox_state(&inbox, 0); diting_battery_inbox_state(&inbox, 1);
    assert(inbox.up && inbox.changed && inbox.epoch != old && !inbox.count);
    assert(!diting_battery_inbox_pop(&inbox, out));
    assert(!diting_battery_inbox_push(&inbox, data, 0));
    assert(!diting_battery_inbox_push(&inbox, data, 11));
    assert(!diting_battery_inbox_push(&inbox, data, 65));
    assert(!diting_battery_inbox_push(&inbox, NULL, 24));
    for (unsigned i = 0; i < 16; ++i) {data[0] = (uint8_t)i; assert(diting_battery_inbox_push(&inbox, data, 64));}
    assert(!diting_battery_inbox_push(&inbox, data, 24));
    for (unsigned i = 0; i < 8; ++i) {assert(diting_battery_inbox_pop(&inbox, out) == 64); assert(out[0] == i);}
    for (unsigned i = 16; i < 24; ++i) {data[0] = (uint8_t)i; assert(diting_battery_inbox_push(&inbox, data, 24));}
    for (unsigned i = 8; i < 24; ++i) {assert(diting_battery_inbox_pop(&inbox, out) == (i < 16 ? 64u : 24u)); assert(out[0] == i);}
    assert(!diting_battery_inbox_pop(&inbox, out));
}

static void test_fuzz(void)
{
    uint32_t seed = 12345;
    for (unsigned iteration = 0; iteration < 5000; ++iteration) {
        ShengBattmgr b; uint8_t request[24], frame[64]; sheng_battmgr_init(&b);
        sheng_battmgr_refresh(&b, 1); sheng_battmgr_next(&b, request);
        for (unsigned i = 0; i < sizeof frame; ++i) {seed = seed * 1664525u + 1013904223u; frame[i] = (uint8_t)(seed >> 24);}
        if (!(iteration % 3)) put32(frame, 32778);
        if (!(iteration % 5)) {put32(frame + 4, 1); put32(frame + 8, 0x30); put32(frame + 12, 0);}
        const int result = sheng_battmgr_receive(&b, frame, iteration % 65);
        assert(result >= BmIgnored && result <= BmSnapshotReady);
        ShengBatterySnapshot s; sheng_battmgr_snapshot(&b, &s);
        assert(!s.charge_known && !s.critical);
    }
}

static void test_usb_data_role(void)
{
    /* USB-A PC: SDP(1) and CDP(3) -> data host even if UCSI opmode is default/BC (-1). */
    int cls = diting_battery_classify_usb_data(1, 1, 1, 1, 1, 1, 0, 0);
    assert(cls == DitingUsbClassDataHost);
    assert(diting_usb_resolve_data_online(1, -1, 0, cls) == 1);
    cls = diting_battery_classify_usb_data(1, 1, 1, 3, 1, 3, 0, 0);
    assert(cls == DitingUsbClassDataHost);
    assert(diting_usb_resolve_data_online(-1, -1, 0, cls) == 1);

    /* USB-A wall charger: DCP(2) and HVDCP(0x81) -> MUST return 0 even if UCSI sees 56k Rp DFP. */
    cls = diting_battery_classify_usb_data(1, 1, 1, 2, 1, 2, 0, 0);
    assert(cls == DitingUsbClassCharger);
    assert(diting_usb_resolve_data_online(1, 1, 0, cls) == 0);
    cls = diting_battery_classify_usb_data(1, 1, 0, 0, 1, 5, 1, 0x81);
    assert(cls == DitingUsbClassCharger);
    assert(diting_usb_resolve_data_online(1, 1, 1, cls) == 0);

    /* USB-C PD laptop: PD(6) + UCSI connected + DFP + USB flag -> 1. */
    cls = diting_battery_classify_usb_data(1, 1, 1, 6, 1, 6, 0, 0);
    assert(cls == DitingUsbClassPdOrTypeC);
    assert(diting_usb_resolve_data_online(1, 1, 1, cls) == 1);

    /* USB-C PD wall brick: PD_PPS(8) + UCSI connected + DFP without USB flag -> 0. */
    cls = diting_battery_classify_usb_data(1, 1, 1, 8, 1, 8, 0, 0);
    assert(cls == DitingUsbClassPdOrTypeC);
    assert(diting_usb_resolve_data_online(1, 0, 0, cls) == 0);
    /* PD brick when UCSI not yet ready -> safe default 0. */
    assert(diting_usb_resolve_data_online(-1, -1, 0, cls) == 0);

    /* Unplugged (either UCSI disconnected or VBUS off) -> 0. */
    assert(diting_usb_resolve_data_online(0, 0, 0, DitingUsbClassDataHost) == 0);
    cls = diting_battery_classify_usb_data(0, 1, 0, 0, 0, 0, 0, 0);
    assert(cls == DitingUsbClassCharger);
    assert(diting_usb_resolve_data_online(-1, -1, 0, cls) == 0);

    /* Pending both -> -1. */
    cls = diting_battery_classify_usb_data(1, 1, 0, 0, 0, 0, 0, 0);
    assert(cls == DitingUsbClassUnknown);
    assert(diting_usb_resolve_data_online(-1, -1, 0, cls) == -1);
}

typedef struct {
    unsigned state_calls, rx_calls;
    int last_up;
    uint32_t last_owner, last_len;
} RouterSink;

static void sink_state(void *ctx, int up)
{
    RouterSink *s = (RouterSink *)ctx;
    s->state_calls++;
    s->last_up = up;
}
static void sink_rx(void *ctx, const uint8_t *data, uint32_t len)
{
    RouterSink *s = (RouterSink *)ctx;
    s->rx_calls++;
    s->last_owner = get32(data);
    s->last_len = len;
}

static void test_pmic_glink(void)
{
    DitingPmicGlinkRouter r;
    diting_pmic_glink_router_init(&r);
    RouterSink batt = {0}, ucsi = {0};
    DitingPmicGlinkClient cb = {sink_state, sink_rx};
    assert(diting_pmic_glink_router_register(&r, DITING_PMIC_GLINK_OWNER_BATTMGR, &cb, &batt) == 0);
    assert(diting_pmic_glink_router_register(&r, DITING_PMIC_GLINK_OWNER_UCSI, &cb, &ucsi) == 0);
    /* Reject duplicate owner. */
    assert(diting_pmic_glink_router_register(&r, DITING_PMIC_GLINK_OWNER_BATTMGR, &cb, &batt) == -1);

    /* Down channel drops messages. */
    uint8_t msg[24] = {0};
    put32(msg, DITING_PMIC_GLINK_OWNER_BATTMGR);
    assert(!diting_pmic_glink_router_dispatch(&r, msg, sizeof msg));
    assert(batt.rx_calls == 0 && r.rx_short == 1);

    /* Bring channel up -> epoch increments, both clients retrieved. */
    DitingPmicGlinkSlot slots[DITING_PMIC_GLINK_MAX_CLIENTS];
    unsigned n = diting_pmic_glink_router_state(&r, 1, slots, DITING_PMIC_GLINK_MAX_CLIENTS);
    assert(n == 2 && r.up == 1 && r.epoch == 1);
    for (unsigned i = 0; i < n; ++i) slots[i].client.state(slots[i].context, 1);
    assert(batt.state_calls == 1 && batt.last_up == 1 && ucsi.state_calls == 1 && ucsi.last_up == 1);

    /* Route 32778 to batt, 32779 to ucsi, unknown 32780 dropped, short dropped. */
    assert(diting_pmic_glink_router_dispatch(&r, msg, 24) == 1);
    assert(batt.rx_calls == 1 && batt.last_owner == 32778 && ucsi.rx_calls == 0);
    put32(msg, DITING_PMIC_GLINK_OWNER_UCSI);
    assert(diting_pmic_glink_router_dispatch(&r, msg, 16) == 1);
    assert(ucsi.rx_calls == 1 && ucsi.last_owner == 32779 && ucsi.last_len == 16);
    put32(msg, 32780);
    assert(diting_pmic_glink_router_dispatch(&r, msg, 16) == 0 && r.rx_unknown_owner == 1);
    assert(diting_pmic_glink_router_dispatch(&r, msg, 11) == 0 && r.rx_short == 2);

    /* TX owner validation. */
    put32(msg, DITING_PMIC_GLINK_OWNER_UCSI);
    assert(diting_pmic_glink_validate_tx(DITING_PMIC_GLINK_OWNER_UCSI, msg, 16, 512) == 1);
    assert(diting_pmic_glink_validate_tx(DITING_PMIC_GLINK_OWNER_BATTMGR, msg, 16, 512) == 0);
    assert(diting_pmic_glink_validate_tx(DITING_PMIC_GLINK_OWNER_UCSI, msg, 11, 512) == 0);

    /* Down/up bumps epoch twice. */
    diting_pmic_glink_router_state(&r, 0, NULL, 0);
    assert(!r.up && r.epoch == 2);
    diting_pmic_glink_router_state(&r, 1, NULL, 0);
    assert(r.up && r.epoch == 3);
}

static void ucsi_write_resp(uint8_t out[16], uint32_t rc)
{
    put32(out, DITING_UCSI_OWNER);
    put32(out + 4, DITING_UCSI_TYPE_REQ_RESP);
    put32(out + 8, DITING_UCSI_OP_WRITE_BUF);
    put32(out + 12, rc);
}

static void ucsi_notify_ind(uint8_t out[24], uint32_t cci)
{
    memset(out, 0, 24);
    put32(out, DITING_UCSI_OWNER);
    put32(out + 4, DITING_UCSI_TYPE_REQ_RESP);
    put32(out + 8, DITING_UCSI_OP_NOTIFY_IND);
    put32(out + 12, cci);
}

static void ucsi_read_resp(uint8_t out[64], uint32_t cci, uint16_t change, uint16_t flags, uint32_t rdo, uint8_t bc, uint32_t rc)
{
    memset(out, 0, 64);
    put32(out, DITING_UCSI_OWNER);
    put32(out + 4, DITING_UCSI_TYPE_REQ_RESP);
    put32(out + 8, DITING_UCSI_OP_READ_BUF);
    uint8_t *buf = out + 12;
    buf[0] = 0x20; buf[1] = 0x01; /* UCSI v1.2 */
    put32(buf + DITING_UCSI_OFF_CCI, cci);
    buf[16] = (uint8_t)change; buf[17] = (uint8_t)(change >> 8);
    buf[18] = (uint8_t)flags; buf[19] = (uint8_t)(flags >> 8);
    put32(buf + 20, rdo);
    buf[24] = bc;
    put32(out + 12 + DITING_UCSI_BUF_SIZE, rc);
}

static void test_ucsi(void)
{
    DitingUcsi u;
    DitingUcsiStatus st;
    uint8_t tx[DITING_UCSI_WRITE_REQ_BYTES];
    uint8_t wresp[16], nind[24], rresp[64];

    diting_ucsi_init(&u);
    diting_ucsi_snapshot(&u, &st);
    assert(!st.valid && diting_ucsi_status_usb_data_online(&st) == -1);

    /* Start -> SET_NOTIFICATION_ENABLE */
    diting_ucsi_start(&u);
    assert(diting_ucsi_next(&u, tx) == 64);
    assert(get32(tx) == DITING_UCSI_OWNER && get32(tx + 8) == DITING_UCSI_OP_WRITE_BUF);
    assert(tx[12 + DITING_UCSI_OFF_CONTROL] == DITING_UCSI_CMD_SET_NOTIFICATION_ENABLE);
    assert(diting_ucsi_next(&u, tx) == 0); /* wait for write resp */

    /* Simulate silent PPM: WRITE_RESP then timeout on WaitEnableNotify -> falls through to READ_BUF_REQ */
    ucsi_write_resp(wresp, 0);
    assert(diting_ucsi_receive(&u, wresp, 16) == UcsiProgress);
    assert(diting_ucsi_timeout(&u) == UcsiProgress && u.timeouts == 0);
    assert(diting_ucsi_next(&u, tx) == 12 && get32(tx + 8) == DITING_UCSI_OP_READ_BUF);
    ucsi_read_resp(rresp, DITING_UCSI_CCI_COMMAND_COMPLETE, 0, 0, 0, 0, 0);
    assert(diting_ucsi_receive(&u, rresp, 64) == UcsiProgress && u.enabled);

    /* ACK_CC_CI for enable */
    assert(diting_ucsi_next(&u, tx) == 64 && tx[12 + DITING_UCSI_OFF_CONTROL] == DITING_UCSI_CMD_ACK_CC_CI);
    assert(diting_ucsi_receive(&u, wresp, 16) == UcsiProgress);

    /* GET_CONNECTOR_STATUS(1) with out-of-order NOTIFY_IND arriving BEFORE WRITE_RESP */
    assert(diting_ucsi_next(&u, tx) == 64);
    assert(tx[12 + DITING_UCSI_OFF_CONTROL] == DITING_UCSI_CMD_GET_CONNECTOR_STATUS);
    assert(tx[12 + DITING_UCSI_OFF_CONTROL + 2] == 1); /* connector 1 */
    ucsi_notify_ind(nind, DITING_UCSI_CCI_COMMAND_COMPLETE);
    assert(diting_ucsi_receive(&u, nind, 24) == UcsiNotified);
    assert(diting_ucsi_receive(&u, wresp, 16) == UcsiProgress);
    /* Jumps straight to READ_BUF_REQ without waiting again */
    assert(diting_ucsi_next(&u, tx) == 12 && get32(tx + 8) == DITING_UCSI_OP_READ_BUF);

    /* Reply: PD laptop (connected=1, opmode=PD(3), dir=0 sink, partner_flags=USB(1), partner=DFP(1), RDO=3000mA) */
    uint16_t pd_laptop_flags = (uint16_t)(DitingUcsiPwrOpmodePd | (1u << 3) | (DITING_UCSI_PARTNER_FLAG_USB << 5) |
                                          (DitingUcsiPartnerDfp << 13));
    uint32_t rdo = (1u << 28) | (300u << 10) | 300u; /* 300 * 10 mA = 3000 mA */
    ucsi_read_resp(rresp, DITING_UCSI_CCI_COMMAND_COMPLETE | (9u << 8), 0x4000, pd_laptop_flags, rdo, 1, 0);
    assert(diting_ucsi_receive(&u, rresp, 64) == UcsiStatusUpdated);
    diting_ucsi_snapshot(&u, &st);
    assert(st.valid && st.connected == 1 && st.pwr_opmode == DitingUcsiPwrOpmodePd);
    assert(st.partner_type == DitingUcsiPartnerDfp && st.partner_flags == DITING_UCSI_PARTNER_FLAG_USB);
    assert(st.rdo_op_ma == 3000 && st.rdo_max_ma == 3000);
    assert(diting_ucsi_status_is_pd_usb_host(&st) == 1 && diting_ucsi_status_usb_data_online(&st) == 1);

    /* ACK_CC_CI includes both COMMAND_COMPLETE and CONNECTOR_CHANGE */
    assert(diting_ucsi_next(&u, tx) == 64);
    assert(tx[12 + DITING_UCSI_OFF_CONTROL] == DITING_UCSI_CMD_ACK_CC_CI);
    assert((tx[12 + DITING_UCSI_OFF_CONTROL + 2] & 0x03) == 0x03);
    assert(diting_ucsi_receive(&u, wresp, 16) == UcsiProgress);
    assert(diting_ucsi_next(&u, tx) == 0); /* back to idle */

    /* Connector change notification -> PD wall charger (no USB flag) -> data_online == 0 */
    ucsi_notify_ind(nind, (1u << 1)); /* connector 1 changed */
    assert(diting_ucsi_receive(&u, nind, 24) == UcsiNotified);
    assert(diting_ucsi_next(&u, tx) == 64 && tx[12 + DITING_UCSI_OFF_CONTROL] == DITING_UCSI_CMD_GET_CONNECTOR_STATUS);
    assert(diting_ucsi_receive(&u, wresp, 16) == UcsiProgress);
    ucsi_notify_ind(nind, DITING_UCSI_CCI_COMMAND_COMPLETE);
    assert(diting_ucsi_receive(&u, nind, 24) == UcsiProgress);
    assert(diting_ucsi_next(&u, tx) == 12);
    uint16_t pd_brick_flags = (uint16_t)(DitingUcsiPwrOpmodePd | (1u << 3) | (DitingUcsiPartnerDfp << 13));
    ucsi_read_resp(rresp, DITING_UCSI_CCI_COMMAND_COMPLETE | (9u << 8), 0x4000, pd_brick_flags, rdo, 1, 0);
    assert(diting_ucsi_receive(&u, rresp, 64) == UcsiStatusUpdated);
    diting_ucsi_snapshot(&u, &st);
    assert(st.valid && st.connected == 1 && diting_ucsi_status_is_pd_usb_host(&st) == 0);
    assert(diting_ucsi_status_usb_data_online(&st) == 0);

    /* Non-PD Type-C 1.5A/3.0A PC port (no PD USB flag) -> defer to APSD (-1), not forced 0. */
    DitingUcsiStatus tc = st;
    tc.pwr_opmode = DitingUcsiPwrOpmodeTypeC1_5;
    assert(diting_ucsi_status_usb_data_online(&tc) == -1);
    tc.pwr_opmode = DitingUcsiPwrOpmodeTypeC3_0;
    assert(diting_ucsi_status_usb_data_online(&tc) == -1);
    assert(diting_usb_resolve_data_online(1, 0, 0, DitingUsbClassDataHost) == 1);
}

static void test_writes(void)
{
    /* Thermal level hysteresis curve (0..3). */
    assert(sheng_battmgr_thermal_level(3500, 0) == 0);
    assert(sheng_battmgr_thermal_level(3800, 0) == 1);
    assert(sheng_battmgr_thermal_level(3750, 1) == 1); /* hysteresis holds 1 */
    assert(sheng_battmgr_thermal_level(3699, 1) == 0); /* drops at < 37.0C */
    assert(sheng_battmgr_thermal_level(4200, 1) == 2);
    assert(sheng_battmgr_thermal_level(4150, 2) == 2);
    assert(sheng_battmgr_thermal_level(4099, 2) == 1);
    assert(sheng_battmgr_thermal_level(4500, 0) == 3); /* immediate emergency clamp */
    assert(sheng_battmgr_thermal_level(4450, 3) == 3);
    assert(sheng_battmgr_thermal_level(4399, 3) == 2);

    /* Strict whitelist enforcement. */
    ShengBattmgr b;
    uint8_t req[24], reply[24];
    sheng_battmgr_init(&b);
    assert(!sheng_battmgr_queue_write(&b, 0x36, 0, 1)); /* ship mode forbidden */
    assert(!sheng_battmgr_queue_write(&b, 0x31, 10, 4)); /* level > 3 forbidden */
    assert(!sheng_battmgr_queue_write(&b, 0x31, 9, 1));  /* wrong battery prop forbidden */
    assert(!sheng_battmgr_queue_write(&b, 0x33, 5, 50000)); /* ICL < 100mA forbidden */
    assert(!sheng_battmgr_queue_write(&b, 0x33, 5, 3500000)); /* ICL > 3A forbidden */
    assert(!sheng_battmgr_queue_write(&b, 0x51, 0, 1)); /* Xiaomi OEM write forbidden */

    /* Valid thermal write shares single in-flight slot and triggers readback refresh. */
    assert(sheng_battmgr_queue_write(&b, SHENG_BATTMGR_OP_BATTERY_SET, SHENG_BATTMGR_PROP_BATT_THERMAL_LEVEL, 2));
    assert(!sheng_battmgr_queue_write(&b, SHENG_BATTMGR_OP_USB_SET, SHENG_BATTMGR_PROP_USB_ICL, 500000)); /* busy */
    assert(sheng_battmgr_next(&b, req) == 24);
    assert(get32(req) == 32778 && get32(req + 4) == 1 && get32(req + 8) == 0x31);
    assert(get32(req + 12) == 0 && get32(req + 16) == 10 && get32(req + 20) == 2);
    assert(sheng_battmgr_next(&b, req) == 0); /* single in-flight */
    /* Wrong opcode/prop reply ignored while write in-flight. */
    response(reply, 0x30, 10, 2, 0);
    assert(sheng_battmgr_receive(&b, reply, 24) == BmIgnored && b.write_in_flight);
    /* Exact write ACK completes write and starts readback refresh. */
    response(reply, 0x31, 10, 2, 0);
    assert(sheng_battmgr_receive(&b, reply, 24) == BmWriteDone);
    assert(!b.write_in_flight && b.writes_ok == 1 && b.writes_err == 0 && b.last_write_rc == 0);
    assert(sheng_battmgr_next(&b, req) == 24 && get32(req + 8) == 0x30); /* readback started */

    /* Write timeout & firmware error handling. */
    sheng_battmgr_init(&b);
    assert(sheng_battmgr_queue_write(&b, SHENG_BATTMGR_OP_USB_SET, SHENG_BATTMGR_PROP_USB_ICL, 1500000));
    assert(sheng_battmgr_next(&b, req) == 24 && get32(req + 8) == 0x33 && get32(req + 16) == 5 && get32(req + 20) == 1500000);
    assert(sheng_battmgr_timeout(&b) == BmWriteDone && b.writes_err == 1 && b.last_write_rc == -110);
    assert(sheng_battmgr_queue_write(&b, SHENG_BATTMGR_OP_BATTERY_SET, SHENG_BATTMGR_PROP_BATT_THERMAL_LEVEL, 3));
    assert(sheng_battmgr_next(&b, req) == 24);
    response(reply, 0x31, 10, 3, 22);
    assert(sheng_battmgr_receive(&b, reply, 24) == BmWriteDone && b.writes_err == 2 && b.last_write_rc == 22);
}

static void test_weak_drain(void)
{
    DitingBatteryState state = {0};
    /* Normal charging at 1% (positive current_ma) is NOT critical. */
    ShengBatterySnapshot s = source_sample(1, 1, 1, 1, 0);
    s.voltage_mv = 3600; s.current_ma = 500;
    assert(diting_battery_resolve(&state, &s) && !s.critical);

    /* External connected but net draining (current_ma < 0) at 1% -> CRITICAL! */
    s = source_sample(2, 1, 1, 1, 0);
    s.voltage_mv = 3550; s.current_ma = -420;
    assert(diting_battery_resolve(&state, &s) && s.critical);

    /* External connected, net draining at 4% but undervoltage (< 3450 mV) -> CRITICAL! */
    s = source_sample(3, 4, 1, 1, 0);
    s.voltage_mv = 3420; s.current_ma = -300;
    assert(diting_battery_resolve(&state, &s) && s.critical);

    /* External connected, net draining at 8% and healthy voltage (3700 mV) -> NOT critical. */
    s = source_sample(4, 8, 1, 1, 0);
    s.voltage_mv = 3700; s.current_ma = -300;
    assert(diting_battery_resolve(&state, &s) && !s.critical);
}

int main(int argc, char **argv)
{
    const struct {const char *name; void (*run)(void);} cases[] = {
        {"wire", test_wire}, {"frames", test_frames}, {"ranges", test_ranges}, {"timeouts", test_timeouts},
        {"optional", test_optional}, {"notifications", test_notifications}, {"immutable", test_immutable},
        {"source", test_source}, {"empty", test_empty}, {"inbox", test_inbox}, {"fuzz", test_fuzz},
        {"usb_data_role", test_usb_data_role}, {"pmic_glink", test_pmic_glink}, {"ucsi", test_ucsi},
        {"writes", test_writes}, {"weak_drain", test_weak_drain},
    };
    unsigned ran = 0;
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) if (argc == 1 || !strcmp(argv[1], cases[i].name)) {
        cases[i].run(); printf("PASS %s\n", cases[i].name); ran++;
    }
    assert(ran);
    return 0;
}
