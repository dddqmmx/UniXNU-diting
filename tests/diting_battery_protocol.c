/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ShengBattmgr.h"
#include "DitingBatteryInbox.h"
#include "DitingBatteryState.h"

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

int main(int argc, char **argv)
{
    const struct {const char *name; void (*run)(void);} cases[] = {
        {"wire", test_wire}, {"frames", test_frames}, {"ranges", test_ranges}, {"timeouts", test_timeouts},
        {"optional", test_optional}, {"notifications", test_notifications}, {"immutable", test_immutable},
        {"source", test_source}, {"empty", test_empty}, {"inbox", test_inbox}, {"fuzz", test_fuzz},
    };
    unsigned ran = 0;
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) if (argc == 1 || !strcmp(argv[1], cases[i].name)) {
        cases[i].run(); printf("PASS %s\n", cases[i].name); ran++;
    }
    assert(ran);
    return 0;
}
