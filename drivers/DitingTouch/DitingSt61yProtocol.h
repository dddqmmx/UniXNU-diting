/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* ST61Y wire protocol, recorded by the Diting fts_spi 5.2.4.1 driver.
 * This implementation has no Linux dependencies and never writes flash,
 * calibration or lockdown data. See README.md for the pinned reference. */
#ifndef DITING_ST61Y_PROTOCOL_H
#define DITING_ST61Y_PROTOCOL_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define DITING_ST61Y_FIFO 0x87u
#define DITING_ST61Y_HW_READ 0xfbu
#define DITING_ST61Y_FRAME_READ 0xa7u
#define DITING_ST61Y_EVENT_BYTES 8u
#define DITING_ST61Y_FIFO_DEPTH 32u
#define DITING_ST61Y_CONTACTS 10u
#define DITING_ST61Y_SYSINFO_BYTES 208u
#define DITING_ST61Y_CHIP_ID_ADDRESS 0x20000000u
#define DITING_ST61Y_READY_TIMEOUT_MS 100u
#define DITING_ST61Y_ECHO_TIMEOUT_MS 10000u

typedef bool (*DitingSt61yTransfer)(const uint8_t *, uint8_t *, uint32_t);
/* One continuous SPI transaction: opcode/address, one dummy, then payload.
 * The GENI/GPIO transport owns chip select across this complete buffer. */
static inline bool diting_st61y_read_wire(DitingSt61yTransfer transfer,
        uint8_t opcode, unsigned address_bytes, uint32_t address,
        uint8_t *data, unsigned length, uint8_t *tx, uint8_t *rx, unsigned capacity) {
    if (!transfer || !data || !tx || !rx || !length || length > 1024 ||
        address_bytes > 4 || (address_bytes == 0 && address != 0) ||
        (address_bytes == 2 && address > 0xffff) ||
        !((opcode == DITING_ST61Y_FIFO && address_bytes == 0) ||
          (opcode == DITING_ST61Y_HW_READ && address_bytes == 4) ||
          (opcode == DITING_ST61Y_FRAME_READ && address_bytes == 2))) return false;
    const unsigned header = 1 + address_bytes + 1;
    if (header + length > capacity) return false;
    tx[0] = opcode;
    for (unsigned i = 0; i < address_bytes; ++i)
        tx[i + 1] = (uint8_t)(address >> (8 * (address_bytes - 1 - i)));
    for (unsigned i = 1 + address_bytes; i < header + length; ++i) tx[i] = 0;
    if (!transfer(tx, rx, header + length)) return false;
    for (unsigned i = 0; i < length; ++i) data[i] = rx[header + i];
    return true;
}

typedef struct {
    void *context;
    /* Address bytes are big endian; every read discards one SPI dummy byte. */
    bool (*read)(void *, uint8_t, unsigned, uint32_t, uint8_t *, unsigned);
    bool (*write)(void *, const uint8_t *, unsigned);
    bool (*reset)(void *);
    uint64_t (*now_ms)(void *);
    void (*sleep_ms)(void *, unsigned);
} DitingSt61yIo;

typedef struct {
    uint16_t firmware, config, sensor_x, sensor_y;
    uint8_t protocol, tx, rx;
} DitingSt61yInfo;

typedef enum {
    DitingSt61yOk,
    DitingSt61yBus,
    DitingSt61yReadyTimeout,
    DitingSt61yUnexpectedReset,
    DitingSt61yControllerError,
    DitingSt61yIdentity,
    DitingSt61ySysInfo,
    DitingSt61yEchoTimeout,
    DitingSt61yScanWrite
} DitingSt61yResult;

static inline uint16_t diting_st61y_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

static inline bool diting_st61y_blank(const uint8_t *p, unsigned length) {
    bool zero = true, ones = true;
    for (unsigned i = 0; i < length; ++i) {
        zero = zero && p[i] == 0;
        ones = ones && p[i] == 0xff;
    }
    return zero || ones;
}

static inline bool diting_st61y_sysinfo(const uint8_t *data, unsigned length,
                                      DitingSt61yInfo *info) {
    if (!data || !info || length != DITING_ST61Y_SYSINFO_BYTES ||
        data[0] != 0xa5 || data[1] != 0x01 || data[31] != 0x06) return false;
    const uint16_t fw = diting_st61y_le16(data + 16);
    const uint16_t x = diting_st61y_le16(data + 80), y = diting_st61y_le16(data + 82);
    if (!fw || fw == 0xffff || !x || x == 0xffff || !y || y == 0xffff ||
        !data[84] || data[84] == 0xff || !data[85] || data[85] == 0xff) return false;
    *info = (DitingSt61yInfo){fw, diting_st61y_le16(data + 20), x, y,
                            data[31], data[84], data[85]};
    return true;
}

/* A reset can initially return ff bytes; they are not a controller error.
 * Actual error events are f3. Never accept blank data as READY or ECHO. */
static inline DitingSt61yResult diting_st61y_wait(const DitingSt61yIo *io,
        const uint8_t *match, unsigned length, unsigned timeout_ms, uint8_t last[8]) {
    if (!io || !io->read || !io->now_ms || !io->sleep_ms || !match ||
        !last || !length || length > 8 || !timeout_ms) return DitingSt61yBus;
    const uint64_t start = io->now_ms(io->context);
    /* The iteration bound also protects against a stopped time source. */
    for (unsigned attempt = 0; attempt < timeout_ms / 2 + 1; ++attempt) {
        if (!io->read(io->context, DITING_ST61Y_FIFO, 0, 0, last, 8))
            return DitingSt61yBus;
        if (!diting_st61y_blank(last, 8)) {
            if (last[0] == 0xf3) return DitingSt61yControllerError;
            if (last[0] == 0x03 && match[0] != 0x03) return DitingSt61yUnexpectedReset;
            bool equal = true;
            for (unsigned i = 0; i < length; ++i) equal = equal && last[i] == match[i];
            if (equal) return DitingSt61yOk;
        }
        if (io->now_ms(io->context) - start >= timeout_ms) break;
        io->sleep_ms(io->context, 2);
    }
    return match[0] == 0x03 ? DitingSt61yReadyTimeout : DitingSt61yEchoTimeout;
}

static inline DitingSt61yResult diting_st61y_start(const DitingSt61yIo *io,
        DitingSt61yInfo *info, uint8_t last[8]) {
    if (!io || !io->reset || !io->read || !io->write || !info || !last)
        return DitingSt61yBus;
    if (!io->reset(io->context)) return DitingSt61yBus;
    const uint8_t ready[] = {0x03};
    DitingSt61yResult result = diting_st61y_wait(io, ready, 1, DITING_ST61Y_READY_TIMEOUT_MS, last);
    if (result != DitingSt61yOk) return result;
    uint8_t id[2];
    if (!io->read(io->context, DITING_ST61Y_HW_READ, 4,
                  DITING_ST61Y_CHIP_ID_ADDRESS, id, sizeof(id))) return DitingSt61yBus;
    if (id[0] != 0x36 || id[1] != 0x40) {
        last[0] = id[0]; last[1] = id[1];
        return DitingSt61yIdentity;
    }
    uint8_t data[DITING_ST61Y_SYSINFO_BYTES];
    /* The controller loads System Info automatically after reset. */
    if (!io->read(io->context, DITING_ST61Y_FRAME_READ, 2, 0, data, sizeof(data)))
        return DitingSt61yBus;
    if (!diting_st61y_sysinfo(data, sizeof(data), info)) {
        for (unsigned i = 0; i < 8; ++i) last[i] = data[i];
        return DitingSt61ySysInfo;
    }
    /* Flush stale FIFO events and require this system command's real echo. */
    const uint8_t flush[] = {0xa4, 0x00, 0x01};
    const uint8_t echo[] = {0x43, 0x01, 0xa4, 0x00, 0x01};
    if (!io->write(io->context, flush, sizeof(flush))) return DitingSt61yBus;
    result = diting_st61y_wait(io, echo, sizeof(echo), DITING_ST61Y_ECHO_TIMEOUT_MS, last);
    if (result != DitingSt61yOk) return result;
    /* fts_spi setScanMode checks the bus write, not an undocumented echo.
     * A successful submission is not proof of working touch input. */
    const uint8_t scan[] = {0xa0, 0x00, 0x01};
    return io->write(io->context, scan, sizeof(scan)) ? DitingSt61yOk : DitingSt61yScanWrite;
}

/* fts_spi fts_write_charge_status(): toggle ST61Y wired charger noise immunity. */
static inline bool diting_st61y_set_charger(const DitingSt61yIo *io, bool wired) {
    if (!io || !io->write) return false;
    const uint8_t cmd[3] = {0xa2u, 0x02u, (uint8_t)(wired ? 1 : 0)};
    return io->write(io->context, cmd, sizeof(cmd));
}

static inline const char *diting_st61y_reason(DitingSt61yResult result) {
    switch (result) {
    case DitingSt61yOk: return "scan submitted";
    case DitingSt61yBus: return "SPI/reset failed";
    case DitingSt61yReadyTimeout: return "controller READY timeout";
    case DitingSt61yUnexpectedReset: return "controller reset during command";
    case DitingSt61yControllerError: return "controller error event";
    case DitingSt61yIdentity: return "unexpected chip ID";
    case DitingSt61ySysInfo: return "invalid firmware/System Info";
    case DitingSt61yEchoTimeout: return "FIFO flush echo timeout";
    case DitingSt61yScanWrite: return "scan command write failed";
    }
    return "invalid result";
}

typedef struct { bool down; uint32_t x, y; } DitingSt61yContact;
typedef struct {
    DitingSt61yContact contact[DITING_ST61Y_CONTACTS];
    uint32_t max_x, max_y, flip;
} DitingSt61yState;
typedef void (*DitingSt61yEmit)(void *, unsigned, unsigned, uint32_t, uint32_t);
enum { DitingSt61yDown = 1, DitingSt61yMove = 2, DitingSt61yUp = 3 };
typedef enum {
    DitingSt61yIgnored, DitingSt61yContactEvent, DitingSt61yResetEvent,
    DitingSt61yErrorEvent, DitingSt61yInvalidEvent, DitingSt61yStatusReleaseEvent
} DitingSt61yEventResult;

/* READY/error requires reinitialization immediately. It must not be folded
 * into a transient bus-error count that the next empty FIFO can clear. */
static inline bool diting_st61y_needs_restart(DitingSt61yEventResult result) {
    return result == DitingSt61yResetEvent || result == DitingSt61yErrorEvent;
}

static inline void diting_st61y_release(DitingSt61yState *state,
                                       DitingSt61yEmit emit, void *context) {
    for (unsigned id = 0; id < DITING_ST61Y_CONTACTS; ++id) {
        DitingSt61yContact *c = &state->contact[id];
        if (c->down) {
            c->down = false;
            emit(context, DitingSt61yUp, id, c->x, c->y);
        }
    }
}

static inline uint32_t diting_st61y_axis(uint32_t value, uint32_t maximum) {
    return value >= maximum ? 65535 : (uint32_t)((uint64_t)value * 65535 / maximum);
}

/* This FIFO contains changes, not complete frames. Missing/empty events
 * never release another finger. The high event nibble selects the handler;
 * bits 2/3 carry contact shape information in enter/motion reports. */
static inline DitingSt61yEventResult diting_st61y_event(DitingSt61yState *state,
        const uint8_t *event, unsigned length, DitingSt61yEmit emit, void *context) {
    if (!state || !event || length != 8 || !emit || !state->max_x || !state->max_y)
        return DitingSt61yInvalidEvent;
    if (event[0] == 0) return DitingSt61yIgnored;
    if (diting_st61y_blank(event, 8) || (event[0] & 3) != 3)
        return DitingSt61yInvalidEvent;
    if (event[0] == 0x03 || event[0] == 0xf3) {
        diting_st61y_release(state, emit, context);
        return event[0] == 0x03 ? DitingSt61yResetEvent : DitingSt61yErrorEvent;
    }
    const unsigned kind = event[0] >> 4;
    if (kind == 4) {
        /* EVT_ID_STATUS_UPDATE (0x43): baseline recalibration, raw saturation,
         * water or pocket transitions can drop hardware tracking without a
         * per-finger LEAVE_POINT. Release retained contacts; leave ECHO alone. */
        const unsigned st = event[1];
        if (st == 0x05u || ((st == 0x06u || st == 0x07u || st == 0x0au) && event[2] == 1u)) {
            diting_st61y_release(state, emit, context);
            return DitingSt61yStatusReleaseEvent;
        }
        return DitingSt61yIgnored;
    }
    if (kind != 1 && kind != 2 && kind != 3) return DitingSt61yIgnored;
    /* fts_spi FOD release uses sentinel byte 0xb5 (slot 11); ignore cleanly. */
    if (kind == 3 && event[1] == 0xb5u) return DitingSt61yIgnored;
    const unsigned id = event[1] >> 4, type = event[1] & 15;
    if (id >= DITING_ST61Y_CONTACTS) return DitingSt61yInvalidEvent;
    DitingSt61yContact *c = &state->contact[id];
    if (kind == 3 || type == 3 || type == 5) {
        if (c->down) {
            c->down = false;
            emit(context, DitingSt61yUp, id, c->x, c->y);
        }
        return DitingSt61yContactEvent;
    }
    /* Finger, glove and palm are the vendor's direct-contact classes. */
    if (type != 1 && type != 2 && type != 4) return DitingSt61yInvalidEvent;
    uint32_t x = diting_st61y_axis(diting_st61y_le16(event + 2), state->max_x);
    uint32_t y = diting_st61y_axis(diting_st61y_le16(event + 4), state->max_y);
    if (state->flip & 1) x = 65535 - x;
    if (state->flip & 2) y = 65535 - y;
    if (!c->down) emit(context, DitingSt61yDown, id, x, y);
    else if (c->x != x || c->y != y) emit(context, DitingSt61yMove, id, x, y);
    c->down = true; c->x = x; c->y = y;
    return DitingSt61yContactEvent;
}

typedef enum {
    DitingSt61yPollIdle, DitingSt61yPollEvents, DitingSt61yPollBus,
    DitingSt61yPollInvalid, DitingSt61yPollReset, DitingSt61yPollError
} DitingSt61yPollResult;
typedef struct {
    unsigned events, invalid, status_releases;
    bool overflow;
    uint8_t last[8];
} DitingSt61yPollReport;

/* Consume the FIFO's bounded batch of changes. A failed SPI read, corrupted
 * packet, or saturated 32-event FIFO can lose a leave event, so release
 * retained contacts whenever a batch is compromised. A controller reset/error
 * stops the batch before stale contacts can reenter. */
static inline DitingSt61yPollResult diting_st61y_poll(const DitingSt61yIo *io,
        DitingSt61yState *state, DitingSt61yEmit emit, void *context,
        DitingSt61yPollReport *report) {
    if (!io || !io->read || !state || !emit || !report || !state->max_x || !state->max_y)
        return DitingSt61yPollInvalid;
    report->events = 0;
    report->invalid = 0;
    report->status_releases = 0;
    report->overflow = false;
    for (unsigned i = 0; i < 8; ++i) report->last[i] = 0;
    uint8_t events[DITING_ST61Y_FIFO_DEPTH * DITING_ST61Y_EVENT_BYTES];
    if (!io->read(io->context, DITING_ST61Y_FIFO, 0, 0, events, 8)) {
        diting_st61y_release(state, emit, context);
        return DitingSt61yPollBus;
    }
    if (events[0] == 0) return DitingSt61yPollIdle;
    if (diting_st61y_blank(events, 8)) {
        for (unsigned i = 0; i < 8; ++i) report->last[i] = events[i];
        report->invalid = 1;
        diting_st61y_release(state, emit, context);
        return DitingSt61yPollInvalid;
    }
    const unsigned remaining = events[7] & 31;
    if (remaining && !io->read(io->context, DITING_ST61Y_FIFO, 0, 0,
                               events + 8, remaining * 8)) {
        diting_st61y_release(state, emit, context);
        return DitingSt61yPollBus;
    }
    unsigned non_empty = 0;
    bool skip_continuation = false;
    for (unsigned i = 0; i <= remaining; ++i) {
        const uint8_t *event = events + i * 8;
        ++report->events;
        if (event[0] != 0) ++non_empty;
        if (skip_continuation) {
            skip_continuation = false;
            continue;
        }
        /* fts_spi: EVT_ID_USER_REPORT 0x53 0x02 0x18 is followed by an 8-byte
         * continuation payload that must not be decoded as an event header. */
        if (event[0] == 0x53u && event[1] == 0x02u && event[2] == 0x18u) {
            skip_continuation = true;
            continue;
        }
        const DitingSt61yEventResult result = diting_st61y_event(state, event, 8, emit, context);
        if (result == DitingSt61yStatusReleaseEvent) {
            ++report->status_releases;
        } else if (diting_st61y_needs_restart(result) || result == DitingSt61yInvalidEvent) {
            for (unsigned j = 0; j < 8; ++j) report->last[j] = event[j];
            if (result == DitingSt61yResetEvent) return DitingSt61yPollReset;
            if (result == DitingSt61yErrorEvent) return DitingSt61yPollError;
            ++report->invalid;
        }
    }
    if (non_empty == DITING_ST61Y_FIFO_DEPTH) {
        report->overflow = true;
    }
    if (report->invalid || report->overflow) {
        diting_st61y_release(state, emit, context);
    }
    return DitingSt61yPollEvents;
}
#endif
