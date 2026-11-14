/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "DitingGeniSpi.h"

typedef struct {
    uint32_t active, irq, length, tx_count, rx_count, rx_head;
    uint32_t rx_words[512]; uint8_t tx[1032];
    uint64_t now, last_clock, released;
    unsigned writes, selects, commands, controls[2], control_count, extra_rx, bad_tx;
    unsigned short_by, error, fail_mode;
    bool selected, stuck_active, stopped_clock;
} Engine;
/* An engine model streams each transmitted FIFO word onto the wire and
 * enqueues independently generated RX bytes. Tests inject hardware outcomes,
 * without reproducing the driver's polling/control flow. */
static uint32_t rd(void *context, unsigned off) {
    Engine *e = context;
    switch (off) {
    case DitingGeniStatus: return e->active;
    case DitingGeniIrq: return e->irq;
    case DitingGeniTxStatus: return e->bad_tx;
    case DitingGeniRxStatus: {
        unsigned words = e->rx_count - e->rx_head;
        if (e->extra_rx && e->commands) return e->extra_rx;
        if (words && e->tx_count == e->length) {
            unsigned valid = (e->length - e->short_by) % 4;
            return words | (1u << 31) | valid << 28;
        }
        return words;
    }
    case DitingGeniRxFifo:
        assert(e->rx_head < e->rx_count);
        return e->rx_words[e->rx_head++];
    default: return 0;
    }
}
static void wr(void *context, unsigned off, uint32_t value) {
    Engine *e = context; ++e->writes;
    if (off == DitingGeniClear) e->irq &= ~value;
    else if (off == DitingGeniTxLength) e->length = value;
    else if (off == DitingGeniRxLength) assert(e->length == value);
    else if (off == DitingGeniCommand) {
        assert(e->selected && !e->active && value == 3u << 27);
        ++e->commands; e->active = 1; e->tx_count = 0; e->rx_count = e->rx_head = 0;
    } else if (off == DitingGeniTxFifo) {
        assert(e->selected && e->active && e->tx_count < e->length);
        unsigned start = e->tx_count; uint32_t received = 0; unsigned valid = 0;
        for (unsigned j = 0; j < 4 && e->tx_count < e->length; ++j) {
            const uint8_t byte = (uint8_t)(value >> (j * 8));
            e->tx[e->tx_count] = byte;
            if (e->tx_count < e->length - e->short_by) {
                received |= (uint32_t)(byte ^ 0xa5u) << (valid++ * 8);
            }
            ++e->tx_count;
        }
        if (valid && !e->fail_mode) {
            assert(e->rx_count < 512); e->rx_words[e->rx_count++] = received;
        }
        e->last_clock = e->now;
        if (e->tx_count == e->length && !e->fail_mode) {
            e->irq |= DITING_GENI_DONE | e->error;
            if (!e->stuck_active) e->active = 0;
        }
        assert(e->tx_count > start);
    } else if (off == DitingGeniControl) {
        assert(e->control_count < 2); e->controls[e->control_count++] = value;
        if (e->fail_mode == 1 || (e->fail_mode == 2 && value == 2) || (!e->fail_mode && !e->extra_rx)) {
            e->active = 0; e->irq |= value == 4 ? DITING_GENI_CANCEL : DITING_GENI_ABORT;
        }
    }
}
static uint64_t now(void *context) {return ((Engine *)context)->now;}
static void delay(void *context, unsigned us) {
    Engine *e = context; if (!e->stopped_clock) e->now += us;
}
static void select_io(void *context, bool selected) {
    Engine *e = context; ++e->selects;
    if (!selected) {assert(e->selected); assert(e->now - e->last_clock >= 10 || e->stopped_clock); e->released = e->now;}
    e->selected = selected;
}
static DitingGeniSpiIo io(Engine *e) {return (DitingGeniSpiIo){e, rd, wr, now, delay, select_io};}
static DitingGeniSpiState state(void) {return (DitingGeniSpiState){.tx_depth = 16, .rx_depth = 16};}
static void test_lengths(void) {
    const unsigned lengths[] = {1, 2, 3, 4, 5, 8, 60, 64, 208, 258, 1032};
    uint8_t tx[1032], rx[1034];
    for (unsigned i = 0; i < sizeof(tx); ++i) tx[i] = (uint8_t)i;
    for (unsigned n = 0; n < sizeof(lengths) / sizeof(lengths[0]); ++n) {
        const unsigned length = lengths[n]; Engine e = {0};
        DitingGeniSpiIo bus = io(&e); DitingGeniSpiState s = state(); memset(rx, 0x5a, sizeof(rx));
        assert(diting_geni_spi_transfer(&bus, &s, tx, rx + 1, length) == DitingGeniSpiOk);
        assert(s.sent == length && s.received == length && !s.retained);
        assert(e.commands == 1 && e.selects == 2 && !e.selected && e.released >= e.last_clock + 10);
        assert(!memcmp(e.tx, tx, length) && rx[0] == 0x5a && rx[length + 1] == 0x5a);
        for (unsigned i = 0; i < length; ++i) assert(rx[i + 1] == (uint8_t)(tx[i] ^ 0xa5u));
    }
    Engine e = {0}; DitingGeniSpiIo bus = io(&e); DitingGeniSpiState s = state();
    assert(diting_geni_spi_transfer(&bus, &s, NULL, NULL, 8) == DitingGeniSpiOk);
    for (unsigned i = 0; i < 8; ++i) assert(e.tx[i] == 0);
}
static void test_refusals(void) {
    uint8_t tx[8] = {0}, rx[8];
    const uint32_t busy[] = {1, 1u << 12, DITING_GENI_ACTIVE};
    for (unsigned i = 0; i < 3; ++i) {
        Engine e = {.active = busy[i], .irq = DITING_GENI_DONE};
        DitingGeniSpiIo bus = io(&e); DitingGeniSpiState s = state();
        assert(diting_geni_spi_transfer(&bus, &s, tx, rx, 8) == DitingGeniSpiBusy);
        assert(e.writes == 0 && e.selects == 0 && e.irq == DITING_GENI_DONE);
    }
    Engine e = {.rx_count = 1}; DitingGeniSpiIo bus = io(&e); DitingGeniSpiState s = state();
    assert(diting_geni_spi_transfer(&bus, &s, tx, rx, 8) == DitingGeniSpiFifo && e.writes == 0);
    e = (Engine){0}; s.tx_depth = 3;
    assert(diting_geni_spi_transfer(&bus, &s, tx, rx, 8) == DitingGeniSpiInvalid && e.writes == 0);
    s = state();
    assert(diting_geni_spi_transfer(&bus, &s, tx, rx, 0) == DitingGeniSpiInvalid);
    assert(diting_geni_spi_transfer(&bus, &s, tx, rx, 1033) == DitingGeniSpiInvalid);
}
static void test_short_and_error(void) {
    uint8_t tx[8] = {0}, rx[8];
    Engine e = {.short_by = 1}; DitingGeniSpiIo bus = io(&e); DitingGeniSpiState s = state();
    assert(diting_geni_spi_transfer(&bus, &s, tx, rx, 8) == DitingGeniSpiShort && !s.retained);
    assert(s.received == 7 && e.control_count == 1);
    const uint32_t errors[] = {1u << 1, 1u << 2, 1u << 3, 1u << 24, 1u << 25, 1u << 28, 1u << 29};
    for (unsigned i = 0; i < 7; ++i) {
        e = (Engine){.error = errors[i]}; s = state();
        assert(diting_geni_spi_transfer(&bus, &s, tx, rx, 8) == DitingGeniSpiError && !s.retained);
        assert(e.control_count == 1 && e.rx_head == e.rx_count);
    }
    e = (Engine){.bad_tx = 17}; s = state();
    assert(diting_geni_spi_transfer(&bus, &s, tx, rx, 8) == DitingGeniSpiFifo);
    e = (Engine){.extra_rx = 17}; s = state();
    assert(diting_geni_spi_transfer(&bus, &s, tx, rx, 8) == DitingGeniSpiFifo && s.retained);
    e = (Engine){.extra_rx = 1u << 31 | 5u << 28 | 1u}; s = state();
    assert(diting_geni_spi_transfer(&bus, &s, tx, rx, 8) == DitingGeniSpiFifo);
}
static void test_timeouts_and_late_completion(void) {
    uint8_t tx[8] = {1, 2, 3}, rx[8];
    for (unsigned mode = 1; mode <= 3; ++mode) {
        Engine e = {.fail_mode = mode}; DitingGeniSpiIo bus = io(&e); DitingGeniSpiState s = state();
        assert(diting_geni_spi_transfer(&bus, &s, tx, rx, 8) == DitingGeniSpiTimeout);
        assert(e.now >= 250000 && !e.selected);
        assert(e.control_count == (mode == 1 ? 1u : 2u) && e.controls[0] == 4);
        if (mode > 1) assert(e.controls[1] == 2);
        assert(s.retained == (mode == 3));
        if (mode == 3) {
            unsigned writes = e.writes, commands = e.commands;
            assert(diting_geni_spi_transfer(&bus, &s, tx, rx, 8) == DitingGeniSpiRetained);
            assert(e.writes == writes && e.commands == commands);
            /* IRQ alone while active cannot release ownership. */
            e.irq = DITING_GENI_DONE; assert(!diting_geni_spi_reap(&bus, &s));
            e.active = 0; e.irq = 0; assert(!diting_geni_spi_reap(&bus, &s));
            /* A real late DONE + idle discards only the old command's RX. */
            e.irq = DITING_GENI_DONE; e.rx_words[0] = 0xdeadbeef; e.rx_count = 1; e.rx_head = 0;
            assert(diting_geni_spi_reap(&bus, &s) && !s.retained && e.rx_head == 1);
            e.fail_mode = 0; e.control_count = 0;
            assert(diting_geni_spi_transfer(&bus, &s, tx, rx, 8) == DitingGeniSpiOk);
            assert(rx[0] == (uint8_t)(1 ^ 0xa5));
        }
    }
    Engine e = {.stuck_active = true}; DitingGeniSpiIo bus = io(&e); DitingGeniSpiState s = state();
    assert(diting_geni_spi_transfer(&bus, &s, tx, rx, 8) == DitingGeniSpiTimeout);
    e = (Engine){.fail_mode = 3, .stopped_clock = true}; s = state();
    assert(diting_geni_spi_transfer(&bus, &s, tx, rx, 8) == DitingGeniSpiTimeout && s.retained);
}
int main(void) {
    test_lengths(); test_refusals(); test_short_and_error(); test_timeouts_and_late_completion();
    puts("GENI: 11 lengths, inherited owners, FIFO bounds, errors, cancel/abort and late completion PASS");
    return 0;
}
