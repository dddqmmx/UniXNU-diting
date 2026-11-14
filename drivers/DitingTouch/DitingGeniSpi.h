/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* SM8475 GENI FIFO transfer. The pins/clock/mode are prepared by the board
 * driver. GPIO CS keeps the complete command/dummy/data transaction selected
 * for the ST-required 10 us hold after its last clock. No DMA is used. */
#ifndef DITING_GENI_SPI_H
#define DITING_GENI_SPI_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    void *context;
    uint32_t (*read)(void *, unsigned);
    void (*write)(void *, unsigned, uint32_t);
    uint64_t (*now_us)(void *);
    void (*delay_us)(void *, unsigned);
    void (*select)(void *, bool);
} DitingGeniSpiIo;

typedef struct {
    unsigned tx_depth, rx_depth;
    bool retained;
    uint32_t status, sent, received;
} DitingGeniSpiState;

typedef enum {
    DitingGeniSpiOk, DitingGeniSpiInvalid, DitingGeniSpiBusy,
    DitingGeniSpiRetained, DitingGeniSpiFifo, DitingGeniSpiError,
    DitingGeniSpiShort, DitingGeniSpiTimeout
} DitingGeniSpiResult;

enum {
    DitingGeniStatus = 0x040, DitingGeniCommand = 0x600, DitingGeniControl = 0x604,
    DitingGeniIrq = 0x610, DitingGeniClear = 0x618,
    DitingGeniTxLength = 0x26c, DitingGeniRxLength = 0x270,
    DitingGeniTxFifo = 0x700, DitingGeniRxFifo = 0x780,
    DitingGeniTxStatus = 0x800, DitingGeniRxStatus = 0x804
};
#define DITING_GENI_DONE (1u << 0)
#define DITING_GENI_CANCEL (1u << 4)
#define DITING_GENI_ABORT (1u << 5)
/* msm-geni-se.h: the secondary engine is bit12, not bit1. */
#define DITING_GENI_ACTIVE ((1u << 0) | (1u << 12))
#define DITING_GENI_ERRORS ((1u << 1) | (1u << 2) | (1u << 3) | \
    (1u << 24) | (1u << 25) | (1u << 28) | (1u << 29))

static inline bool diting_geni_spi_drain(const DitingGeniSpiIo *io,
        DitingGeniSpiState *state, uint8_t *data, unsigned length) {
    const uint32_t status = io->read(io->context, DitingGeniRxStatus);
    const unsigned words = status & 0x01ffffffu;
    if (words > state->rx_depth) return false;
    unsigned bytes = words * 4;
    if ((status & 0x80000000u) && words) {
        const unsigned valid = status >> 28 & 7;
        if (valid > 4) return false;
        if (valid && valid < 4) bytes -= 4 - valid;
    }
    if (state->received > length || bytes > length - state->received) return false;
    for (unsigned i = 0; i < words; ++i) {
        const uint32_t word = io->read(io->context, DitingGeniRxFifo);
        for (unsigned j = 0; j < 4 && bytes; ++j, --bytes) {
            if (data) data[state->received] = (uint8_t)(word >> (j * 8));
            ++state->received;
        }
    }
    return true;
}

/* A timed-out command is still ours. Do not clear its status, drain its FIFO,
 * or start another command before a real DONE/CANCEL/ABORT and idle state. */
static inline bool diting_geni_spi_reap(const DitingGeniSpiIo *io,
                                       DitingGeniSpiState *state) {
    if (!state->retained) return true;
    const uint32_t irq = io->read(io->context, DitingGeniIrq);
    if ((io->read(io->context, DitingGeniStatus) & DITING_GENI_ACTIVE) ||
        !(irq & (DITING_GENI_DONE | DITING_GENI_CANCEL | DITING_GENI_ABORT))) return false;
    const unsigned words = io->read(io->context, DitingGeniRxStatus) & 0x01ffffffu;
    if (words > state->rx_depth) return false;
    for (unsigned i = 0; i < words; ++i) (void)io->read(io->context, DitingGeniRxFifo);
    io->write(io->context, DitingGeniClear, 0xffffffffu);
    state->retained = false;
    return true;
}

static inline void diting_geni_spi_recover(const DitingGeniSpiIo *io,
                                          DitingGeniSpiState *state) {
    state->retained = true;
    for (unsigned phase = 0; phase < 2; ++phase) {
        io->write(io->context, DitingGeniControl, phase ? 1u << 1 : 1u << 2);
        const uint64_t start = io->now_us(io->context);
        for (unsigned attempt = 0; attempt < 25001; ++attempt) {
            if (diting_geni_spi_reap(io, state)) return;
            if (io->now_us(io->context) - start >= 250000) break;
            io->delay_us(io->context, 10);
        }
    }
}

static inline DitingGeniSpiResult diting_geni_spi_transfer(const DitingGeniSpiIo *io,
        DitingGeniSpiState *state, const uint8_t *tx, uint8_t *rx, unsigned length) {
    if (!io || !state || !io->read || !io->write || !io->now_us ||
        !io->delay_us || !io->select || !length || length > 1032 ||
        state->tx_depth < 4 || state->tx_depth > 63 ||
        state->rx_depth < 4 || state->rx_depth > 63) return DitingGeniSpiInvalid;
    if (!diting_geni_spi_reap(io, state)) return DitingGeniSpiRetained;
    if (io->read(io->context, DitingGeniStatus) & DITING_GENI_ACTIVE) return DitingGeniSpiBusy;
    if (io->read(io->context, DitingGeniRxStatus) & 0x01ffffffu) return DitingGeniSpiFifo;
    state->sent = state->received = state->status = 0;
    io->write(io->context, DitingGeniClear, 0xffffffffu);
    io->write(io->context, DitingGeniTxLength, length);
    io->write(io->context, DitingGeniRxLength, length);
    io->select(io->context, true);
    /* Vendor spi-msm-geni FIFO uses FULL_DUPLEX=3; TX_RX=7 is a different
     * operation. GPIO CS is independent of the engine's automatic CS. */
    io->write(io->context, DitingGeniCommand, 3u << 27);
    const uint64_t start = io->now_us(io->context);
    DitingGeniSpiResult result = DitingGeniSpiTimeout;
    for (unsigned attempt = 0; attempt < 250001; ++attempt) {
        const unsigned queued = io->read(io->context, DitingGeniTxStatus) & 0x0fffffffu;
        if (queued > state->tx_depth) { result = DitingGeniSpiFifo; break; }
        unsigned available = queued < state->tx_depth ? state->tx_depth - queued - 1 : 0;
        while (state->sent < length && available--) {
            uint32_t word = 0;
            for (unsigned j = 0; j < 4 && state->sent < length; ++j, ++state->sent)
                word |= (uint32_t)(tx ? tx[state->sent] : 0) << (j * 8);
            io->write(io->context, DitingGeniTxFifo, word);
        }
        state->status = io->read(io->context, DitingGeniIrq);
        if (state->status & DITING_GENI_ERRORS) { result = DitingGeniSpiError; break; }
        if (!diting_geni_spi_drain(io, state, rx, length)) { result = DitingGeniSpiFifo; break; }
        if (state->status & DITING_GENI_DONE) {
            if (!diting_geni_spi_drain(io, state, rx, length)) { result = DitingGeniSpiFifo; break; }
            if (state->sent != length || state->received != length) { result = DitingGeniSpiShort; break; }
            if (!(io->read(io->context, DitingGeniStatus) & DITING_GENI_ACTIVE)) {
                io->write(io->context, DitingGeniClear, state->status);
                result = DitingGeniSpiOk;
                break;
            }
        }
        if (io->now_us(io->context) - start >= 250000) break;
        io->delay_us(io->context, 1);
    }
    if (result != DitingGeniSpiOk) diting_geni_spi_recover(io, state);
    io->delay_us(io->context, 10);
    io->select(io->context, false);
    return result;
}
#endif
