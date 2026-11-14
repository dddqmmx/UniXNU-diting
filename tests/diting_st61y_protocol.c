/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "DitingSt61yProtocol.h"

static uint8_t wire_tx[1032], wire_rx[1032];
static unsigned wire_length;
static bool wire_ok = true;
static bool transfer(const uint8_t *tx, uint8_t *rx, uint32_t length) {
    wire_length = length;
    memcpy(wire_tx, tx, length);
    memcpy(rx, wire_rx, length);
    return wire_ok;
}
static void test_wire(void) {
    uint8_t tx[1032], rx[1032], data[1024];
    for (unsigned i = 0; i < sizeof(wire_rx); ++i) wire_rx[i] = (uint8_t)i;
    assert(diting_st61y_read_wire(transfer, 0xfb, 4, 0x20000000, data, 2, tx, rx, sizeof(tx)));
    assert(wire_length == 8);
    assert(!memcmp(wire_tx, (uint8_t[]){0xfb, 0x20, 0, 0, 0, 0, 0, 0}, 8));
    assert(data[0] == 6 && data[1] == 7);
    assert(diting_st61y_read_wire(transfer, 0xa7, 2, 0x1234, data, 208, tx, rx, sizeof(tx)));
    assert(wire_length == 212 && !memcmp(wire_tx, (uint8_t[]){0xa7, 0x12, 0x34, 0}, 4));
    assert(data[0] == 4 && data[207] == 211);
    assert(diting_st61y_read_wire(transfer, 0x87, 0, 0, data, 256, tx, rx, sizeof(tx)));
    assert(wire_length == 258 && wire_tx[0] == 0x87 && wire_tx[1] == 0 && data[0] == 2);
    assert(!diting_st61y_read_wire(transfer, 0xfb, 2, 0, data, 2, tx, rx, sizeof(tx)));
    assert(!diting_st61y_read_wire(transfer, 0xa7, 2, 0x10000, data, 2, tx, rx, sizeof(tx)));
    assert(!diting_st61y_read_wire(transfer, 0x87, 0, 1, data, 2, tx, rx, sizeof(tx)));
    assert(!diting_st61y_read_wire(transfer, 0x87, 0, 0, data, 8, tx, rx, 9));
    assert(!diting_st61y_read_wire(transfer, 0x87, 0, 0, data, 1025, tx, rx, sizeof(tx)));
    memset(data, 0x5a, sizeof(data)); wire_ok = false;
    assert(!diting_st61y_read_wire(transfer, 0x87, 0, 0, data, 8, tx, rx, sizeof(tx)));
    assert(data[0] == 0x5a); wire_ok = true;
}

static void sysinfo(uint8_t data[208]) {
    memset(data, 0, 208);
    data[0] = 0xa5; data[1] = 1; data[16] = 7; data[20] = 0x34; data[21] = 0x12;
    data[31] = 6; data[80] = 0x97; data[81] = 0x0a; /* 2711, sensor axes */
    data[82] = 0xc3; data[83] = 4; data[84] = 42; data[85] = 18;
}
static void test_sysinfo(void) {
    uint8_t data[208]; DitingSt61yInfo info;
    sysinfo(data);
    assert(diting_st61y_sysinfo(data, sizeof(data), &info));
    assert(info.firmware == 7 && info.config == 0x1234 && info.protocol == 6);
    assert(info.sensor_x == 2711 && info.sensor_y == 1219 && info.tx == 42 && info.rx == 18);
    const unsigned invalid[] = {0, 1, 16, 31, 80, 81, 82, 83, 84, 85};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        sysinfo(data); data[invalid[i]] = 0xff;
        if (invalid[i] >= 80 && invalid[i] <= 83) {
            unsigned axis = invalid[i] & ~1u; data[axis] = data[axis + 1] = 0xff;
        } else if (invalid[i] == 16) data[17] = 0xff;
        assert(!diting_st61y_sysinfo(data, sizeof(data), &info));
    }
    sysinfo(data); data[84] = 0;
    assert(!diting_st61y_sysinfo(data, sizeof(data), &info));
    assert(!diting_st61y_sysinfo(data, 207, &info));
    assert(!diting_st61y_sysinfo(NULL, 208, &info));
}

enum fault {NoFault, ResetFault, ReadyTimeout, ReadyError, IdFault, InfoFault,
    FlushFault, EchoTimeout, EchoError, EchoReset, ScanFault, ReadFault};
typedef struct {
    enum fault fault;
    uint64_t now;
    unsigned phase, writes, reads, resets, ready_delay, echo_delay;
    uint8_t fifo[32][8]; unsigned fifo_head, fifo_count, fail_read;
} Mock;
static bool read_io(void *context, uint8_t opcode, unsigned address_bytes,
                    uint32_t address, uint8_t *data, unsigned length) {
    Mock *m = context;
    ++m->reads;
    if (m->fault == ReadFault || (m->fail_read && m->reads == m->fail_read)) return false;
    memset(data, 0, length);
    if (opcode == 0xfb) {
        assert(address_bytes == 4 && address == 0x20000000 && length == 2);
        data[0] = 0x36; data[1] = m->fault == IdFault ? 0x48 : 0x40;
    } else if (opcode == 0xa7) {
        assert(address_bytes == 2 && address == 0 && length == 208);
        sysinfo(data); if (m->fault == InfoFault) data[31] = 5;
    } else {
        assert(opcode == 0x87 && address_bytes == 0 && address == 0 && length % 8 == 0);
        if (m->fifo_count) {
            assert(length / 8 <= m->fifo_count);
            memcpy(data, m->fifo[m->fifo_head], length);
            m->fifo_head += length / 8; m->fifo_count -= length / 8;
        } else if (m->phase == 1) {
            assert(length == 8);
            if (m->fault == ReadyError) data[0] = 0xf3;
            else if (m->fault != ReadyTimeout && m->now >= m->ready_delay) {data[0] = 3; m->phase = 2;}
            else memset(data, 0xff, length); /* a resetting bus is not READY */
        } else if (m->phase == 3) {
            assert(length == 8);
            if (m->fault == EchoError) data[0] = 0xf3;
            else if (m->fault == EchoReset) data[0] = 3;
            else if (m->fault != EchoTimeout && m->now >= m->echo_delay) {
                memcpy(data, (uint8_t[]){0x43, 1, 0xa4, 0, 1}, 5); m->phase = 4;
            }
        }
    }
    return true;
}
static bool write_io(void *context, const uint8_t *data, unsigned length) {
    Mock *m = context; ++m->writes;
    assert(length == 3);
    if (data[0] == 0xa4) {
        assert(!memcmp(data, (uint8_t[]){0xa4, 0, 1}, 3));
        assert(m->phase == 2); m->phase = 3;
        return m->fault != FlushFault;
    }
    assert(!memcmp(data, (uint8_t[]){0xa0, 0, 1}, 3) && m->phase == 4);
    return m->fault != ScanFault;
}
static bool reset_io(void *context) {
    Mock *m = context; ++m->resets; m->phase = 1;
    return m->fault != ResetFault;
}
static uint64_t now_io(void *context) { return ((Mock *)context)->now; }
static void sleep_io(void *context, unsigned ms) { ((Mock *)context)->now += ms; }
static DitingSt61yIo mock_io(Mock *m) {
    return (DitingSt61yIo){m, read_io, write_io, reset_io, now_io, sleep_io};
}
static void test_start(void) {
    const enum fault faults[] = {NoFault, ResetFault, ReadyTimeout, ReadyError, IdFault,
        InfoFault, FlushFault, EchoTimeout, EchoError, EchoReset, ScanFault, ReadFault};
    const DitingSt61yResult expected[] = {DitingSt61yOk, DitingSt61yBus,
        DitingSt61yReadyTimeout, DitingSt61yControllerError, DitingSt61yIdentity,
        DitingSt61ySysInfo, DitingSt61yBus, DitingSt61yEchoTimeout,
        DitingSt61yControllerError, DitingSt61yUnexpectedReset, DitingSt61yScanWrite, DitingSt61yBus};
    for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
        Mock m = {.fault = faults[i], .ready_delay = 10, .echo_delay = 1200};
        DitingSt61yIo io = mock_io(&m); DitingSt61yInfo info = {0}; uint8_t last[8];
        assert(diting_st61y_start(&io, &info, last) == expected[i]);
        assert(m.resets == 1);
        if (i == 0) assert(info.firmware == 7 && m.writes == 2 && m.now >= 1200);
        if (faults[i] == ReadyTimeout) assert(m.now >= 100 && m.now <= 102 && m.writes == 0);
        if (faults[i] == EchoTimeout) assert(m.now >= 10000 && m.now <= 10012 && m.writes == 1);
        if (i > 0 && faults[i] != ScanFault) assert(m.writes <= 1);
    }
    Mock m = {0}; DitingSt61yIo io = mock_io(&m); io.now_ms = NULL;
    DitingSt61yInfo info; uint8_t last[8];
    assert(diting_st61y_start(&io, &info, last) == DitingSt61yBus);
    for (unsigned read = 2; read <= 4; ++read) {
        m = (Mock){.fail_read = read}; io = mock_io(&m);
        assert(diting_st61y_start(&io, &info, last) == DitingSt61yBus);
        assert(m.writes == (read == 4 ? 1u : 0u));
    }
}

typedef struct { unsigned type, id; uint32_t x, y; } Emitted;
static Emitted emitted[128]; static unsigned emit_count;
static void emit(void *context, unsigned type, unsigned id, uint32_t x, uint32_t y) {
    (void)context; assert(emit_count < 128);
    emitted[emit_count++] = (Emitted){type, id, x, y};
}
static DitingSt61yState state(void) {
    return (DitingSt61yState){.max_x = 12200, .max_y = 27120};
}
static void event(uint8_t out[8], unsigned kind, unsigned shape, unsigned id,
                  unsigned type, uint16_t x, uint16_t y) {
    memcpy(out, (uint8_t[]){(uint8_t)(kind << 4 | shape | 3), (uint8_t)(id << 4 | type),
        (uint8_t)x, (uint8_t)(x >> 8), (uint8_t)y, (uint8_t)(y >> 8), 0, 0}, 8);
}
static void test_contacts(void) {
    DitingSt61yState s = state(); uint8_t e[8]; emit_count = 0;
    event(e, 1, 12, 0, 1, 6100, 13560);
    assert(diting_st61y_event(&s, e, 8, emit, NULL) == DitingSt61yContactEvent);
    assert(emitted[0].type == DitingSt61yDown && emitted[0].x == 32767 && emitted[0].y == 32767);
    event(e, 1, 4, 1, 2, 0, 27120);
    diting_st61y_event(&s, e, 8, emit, NULL);
    assert(emitted[1].id == 1 && emitted[1].x == 0 && emitted[1].y == 65535);
    memset(e, 0, 8); diting_st61y_event(&s, e, 8, emit, NULL);
    assert(emit_count == 2 && s.contact[0].down && s.contact[1].down);
    event(e, 2, 8, 0, 1, 6100, 13560); diting_st61y_event(&s, e, 8, emit, NULL);
    assert(emit_count == 2); /* stationary samples produce no redundant move */
    event(e, 2, 0, 0, 1, 12201, 65535); diting_st61y_event(&s, e, 8, emit, NULL);
    assert(emitted[2].type == DitingSt61yMove && emitted[2].x == 65535 && emitted[2].y == 65535);
    event(e, 3, 0, 0, 1, 0, 0); diting_st61y_event(&s, e, 8, emit, NULL);
    assert(emitted[3].type == DitingSt61yUp && emitted[3].x == 65535 && s.contact[1].down);
    diting_st61y_event(&s, e, 8, emit, NULL); assert(emit_count == 4);
    event(e, 2, 12, 0, 4, 0, 0); diting_st61y_event(&s, e, 8, emit, NULL);
    assert(emitted[4].type == DitingSt61yDown); /* a missed enter is repaired by actual motion */
    event(e, 2, 0, 0, 5, 0, 0); diting_st61y_event(&s, e, 8, emit, NULL);
    assert(emitted[5].type == DitingSt61yUp); /* hover cannot hold a direct contact */
    for (unsigned flip = 0; flip < 4; ++flip) {
        s = state(); s.flip = flip; emit_count = 0;
        event(e, 1, 0, 0, 1, 0, 0); diting_st61y_event(&s, e, 8, emit, NULL);
        assert(emitted[0].x == (flip & 1 ? 65535u : 0u));
        assert(emitted[0].y == (flip & 2 ? 65535u : 0u));
    }
}
static void test_invalid_and_reset(void) {
    DitingSt61yState s = state(); uint8_t e[8]; emit_count = 0;
    event(e, 1, 0, 15, 1, 10, 20);
    assert(diting_st61y_event(&s, e, 8, emit, NULL) == DitingSt61yInvalidEvent);
    event(e, 1, 0, 0, 0, 10, 20);
    assert(diting_st61y_event(&s, e, 8, emit, NULL) == DitingSt61yInvalidEvent);
    memset(e, 0xff, 8);
    assert(diting_st61y_event(&s, e, 8, emit, NULL) == DitingSt61yInvalidEvent);
    assert(diting_st61y_event(&s, e, 7, emit, NULL) == DitingSt61yInvalidEvent);
    assert(emit_count == 0);
    for (unsigned kind = 0; kind < 2; ++kind) {
        s = state(); event(e, 1, 0, 0, 1, 10, 20); diting_st61y_event(&s, e, 8, emit, NULL);
        event(e, 1, 0, 9, 1, 30, 40); diting_st61y_event(&s, e, 8, emit, NULL);
        memset(e, 0, 8); e[0] = kind ? 0xf3 : 3;
        unsigned before = emit_count;
        DitingSt61yEventResult r = diting_st61y_event(&s, e, 8, emit, NULL);
        assert(diting_st61y_needs_restart(r));
        assert(emit_count == before + 2 && !s.contact[0].down && !s.contact[9].down);
        assert(emitted[before].type == DitingSt61yUp && emitted[before + 1].id == 9);
    }
    assert(!diting_st61y_needs_restart(DitingSt61yIgnored));
}
static void test_fifo(void) {
    Mock m = {.fifo_count = 3}; DitingSt61yIo io = mock_io(&m);
    DitingSt61yState s = state(); DitingSt61yPollReport report; emit_count = 0;
    event(m.fifo[0], 1, 8, 0, 1, 100, 100); m.fifo[0][7] = 0xc2;
    event(m.fifo[1], 1, 4, 1, 1, 200, 200);
    event(m.fifo[2], 3, 0, 0, 1, 0, 0);
    assert(diting_st61y_poll(&io, &s, emit, NULL, &report) == DitingSt61yPollEvents);
    assert(report.events == 3 && report.invalid == 0 && m.reads == 2 && emit_count == 3);
    assert(!s.contact[0].down && s.contact[1].down);
    assert(diting_st61y_poll(&io, &s, emit, NULL, &report) == DitingSt61yPollIdle);
    assert(emit_count == 3 && s.contact[1].down);
    /* A reset/error followed by stale enters must return a restart now. */
    for (unsigned kind = 0; kind < 2; ++kind) {
        m = (Mock){.fifo_count = 2}; io = mock_io(&m);
        m.fifo[0][0] = kind ? 0xf3 : 3; m.fifo[0][7] = 1;
        event(m.fifo[1], 1, 0, 2, 1, 500, 500);
        DitingSt61yPollResult r = diting_st61y_poll(&io, &s, emit, NULL, &report);
        assert(r == (kind ? DitingSt61yPollError : DitingSt61yPollReset));
        assert(report.events == 1 && !s.contact[1].down && !s.contact[2].down);
    }
    /* Losing a batch must release an existing finger even if the next read is idle. */
    for (unsigned fail = 1; fail <= 2; ++fail) {
        s = state(); s.contact[1] = (DitingSt61yContact){true, 123, 456};
        m = (Mock){.fifo_count = 2, .fail_read = fail}; io = mock_io(&m);
        event(m.fifo[0], 3, 0, 1, 1, 0, 0); m.fifo[0][7] = 1;
        assert(diting_st61y_poll(&io, &s, emit, NULL, &report) == DitingSt61yPollBus);
        assert(!s.contact[1].down && emitted[emit_count - 1].type == DitingSt61yUp);
    }
    m = (Mock){.fifo_count = 32}; io = mock_io(&m); s = state();
    event(m.fifo[0], 1, 0, 0, 1, 1, 1); m.fifo[0][7] = 31;
    assert(diting_st61y_poll(&io, &s, emit, NULL, &report) == DitingSt61yPollEvents);
    assert(report.events == 32 && m.fifo_count == 0);
}
int main(void) {
    test_wire(); test_sysinfo(); test_start(); test_contacts(); test_invalid_and_reset(); test_fifo();
    puts("ST61Y: wire, System Info, 12 initialization outcomes, contacts, reset and FIFO PASS");
    return 0;
}
