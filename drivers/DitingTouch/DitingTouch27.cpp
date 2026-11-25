// SPDX-License-Identifier: BSD-2-Clause-Patent
// Redmi K50 Ultra (diting / SM8475) ST61Y SPI touch driver for iOS 27 arm64e.
// Publishes real multi-touch contacts directly into NativeHidInput's seqlock/sink
// (no userspace touch daemon required).
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include "../DitingPower/DitingPlatformResources.h"
#include "../DitingPower/DitingGccVotes.h"
#include "QcomRpmh.h"
#include "DitingGeniSpi.h"
#include "DitingSt61yProtocol.h"

extern "C" {
void diting_ramoops_diagnostic_write(const char *, int);
int diting_battery_usb_online(void);
extern volatile uint32_t native_touch_hid_multi[3];
volatile uint32_t native_touch_seq;
volatile uint32_t native_touch_count;
volatile uint32_t native_touch_hid_reports;
volatile int native_touch_xs[12];
volatile int native_touch_ys[12];
volatile int native_touch_ids[12];
volatile uint32_t native_legacy_transfer_calls;
}

namespace {
static void (*gTouchSink)(void *);
static void *gTouchSinkCtx;
static IOLock *gSinkLock;

static IOMemoryMap *gTlmmMap, *gGccMap, *gSeMap, *gCmdDbMap;
static uintptr_t gTlmm, gGcc, gSe, gCmdDb;

static constexpr uint64_t kTlmmPhys = 0x0f000000ULL, kTlmmSize = 0x300000ULL;
static constexpr uint64_t kGccPhys = 0x00100000ULL, kGccSize = 0x1f4200ULL;
static constexpr uint64_t kSePhys = 0x00990000ULL, kSeSize = 0x4000ULL;
static constexpr uint64_t kCmdDbPhys = 0x80860000ULL, kCmdDbSize = 0x20000ULL;
static constexpr uint32_t kGpioOffset = 0x100000u;
static constexpr uint32_t kMaxX = 12200u, kMaxY = 27120u;
static constexpr uint32_t kXoHz = 19200000u;

static constexpr unsigned kPinMiso = 16, kPinMosi = 17, kPinClk = 18, kPinCs = 19;
static constexpr unsigned kPinReset = 20, kPinInt = 21, kPinAvdd = 71, kPinId = 119;
static constexpr unsigned kPinFuncSpi = 1;
static constexpr unsigned kPullNone = 0, kPullUp = 3;

static inline uint32_t rd(uintptr_t addr) {
    uint32_t v = *reinterpret_cast<volatile uint32_t *>(addr);
    __asm__ volatile("dmb oshld" ::: "memory");
    return v;
}
static inline void wr(uintptr_t addr, uint32_t val) {
    __asm__ volatile("dsb oshst" ::: "memory");
    *reinterpret_cast<volatile uint32_t *>(addr) = val;
    __asm__ volatile("dsb oshst" ::: "memory");
}
static inline uint64_t nowUs() {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000;
}
static inline void sleepUs(unsigned us) { IODelay(us); }
static inline void sleepMs(unsigned ms) { IOSleep(ms); }

static inline uintptr_t gpioCfg(unsigned pin) { return gTlmm + kGpioOffset + 0x1000UL * pin; }
static inline uintptr_t gpioIo(unsigned pin) { return gTlmm + kGpioOffset + 0x1000UL * pin + 4; }

static bool gpioConfigure(unsigned pin, unsigned func, unsigned pull, unsigned ma, bool output) {
    uint32_t value = pull | (func << 2) | ((ma / 2 - 1) << 6) | (output ? (1u << 9) : 0);
    const uint32_t old = rd(gpioCfg(pin));
    if (old & (1u << 11)) value |= 1u << 12;
    wr(gpioCfg(pin), (old & ~0x3ffu) | value);
    const uint32_t mask = 0x3ffu | ((old & (1u << 11)) ? (1u << 12) : 0);
    return (rd(gpioCfg(pin)) & mask) == (value & mask);
}
static void gpioSet(unsigned pin, bool high) { wr(gpioIo(pin), high ? 2u : 0u); }
static bool gpioGet(unsigned pin) { return (rd(gpioIo(pin)) & 1u) != 0; }
static bool gpioOutput(unsigned pin, bool high, unsigned pull, unsigned ma) {
    gpioSet(pin, high);
    return gpioConfigure(pin, 0, pull, ma, true) && (((rd(gpioIo(pin)) & 2u) != 0) == high);
}

struct CmdDbSlave {
    uint16_t id, headerOffset, dataOffset, count, version, reserved[3];
};
struct CmdDbEntry {
    char name[8];
    uint32_t priority[2];
    uint32_t addr;
    uint16_t len, offset;
};

static uint32_t rpmhAddress(const char *name) {
    uint8_t *copy = static_cast<uint8_t *>(IOMallocData(0x20000));
    if (!copy) return 0;
    for (unsigned off = 0; off < 0x20000; off += 4) {
        uint32_t w = rd(gCmdDb + off);
        memcpy(copy + off, &w, 4);
    }
    QcomRpmhResource res = {};
    const int rc = qcom_cmd_db_find(copy, 0x20000, name, &res);
    IOFreeData(copy, 0x20000);
    return rc == 0 ? res.address : 0;
}

static bool ioSupplyOn() {
    diting_rpmh_init();
    const uint32_t vdd = rpmhAddress("ldoc2");
    if (!vdd) {
        IOLog("DITING_TOUCH: cmd-db ldoc2 not found\n");
        return false;
    }
    const uint32_t address[3] = {vdd + 8, vdd, vdd + 4};
    const uint32_t data[3] = {7u, 1800u, 1u};
    if (!diting_rpmh_vote_many(address, data, 3)) {
        IOLog("DITING_TOUCH: ldoc2 RPMh vote failed addr=0x%x\n", vdd);
        return false;
    }
    sleepMs(3);
    if (!gpioOutput(kPinAvdd, true, kPullNone, 8)) {
        IOLog("DITING_TOUCH: AVDD GPIO %u output failed\n", kPinAvdd);
        return false;
    }
    sleepMs(10);
    IOLog("DITING_TOUCH: ldoc2=0x%x + AVDD ready\n", vdd);
    return true;
}

static uint32_t gSeSourceHz, gSeClockSel;
static uint32_t rcgRate(uint32_t cfg, uint32_t m, uint32_t notNMinusM) {
    static const uint32_t parent[8] = {kXoHz, 600000000u, 0, 0, 0, 0, 300000000u, 0};
    uint64_t hz = parent[(cfg >> 8) & 7];
    uint32_t div = cfg & 0x1fu, mode = (cfg >> 12) & 3u;
    if (div) hz = hz * 2 / (div + 1);
    if (mode) {
        uint32_t n = (~notNMinusM & 0xffffu) + (m & 0xffffu);
        if (n) hz = hz * (m & 0xffffu) / n;
    }
    return uint32_t(hz);
}

static bool clocksOn() {
    constexpr uint32_t kVoteQup0 = (1u << 9) | (1u << 8) | (1u << 6) | (1u << 7);
    constexpr uint32_t kVoteSe4 = (1u << 14);
    if (!diting_gcc_vote_update(kVoteQup0, 0)) return false;
    if (rd(gGcc + 0x274f8) & 1u) {
        for (uint32_t level = 0; level < 8; ++level) {
            uint32_t hz = rcgRate(rd(gGcc + 0x27500 + 4 * level),
                                  rd(gGcc + 0x27540 + 4 * level),
                                  rd(gGcc + 0x27580 + 4 * level));
            if (hz >= 4000000u && (gSeSourceHz == 0 || hz < gSeSourceHz)) {
                gSeSourceHz = hz;
                gSeClockSel = level;
            }
        }
        if (!gSeSourceHz) return false;
    } else {
        wr(gGcc + 0x274e8, (rd(gGcc + 0x274e8) & ~0x371fu) | 1u);
        wr(gGcc + 0x274e4, rd(gGcc + 0x274e4) | 1u);
        for (int i = 0; i < 500 && (rd(gGcc + 0x274e4) & 1u); ++i) sleepUs(10);
        if (rd(gGcc + 0x274e4) & 1u) return false;
        gSeSourceHz = kXoHz;
        gSeClockSel = 0;
    }
    if (!diting_gcc_vote_update(kVoteSe4, 0)) return false;
    for (int i = 0; i < 1000; ++i) {
        if (diting_gcc_branch_ready(rd(gGcc + 0x27004), true) &&
            diting_gcc_branch_ready(rd(gGcc + 0x27008), true) &&
            diting_gcc_branch_ready(rd(gGcc + 0x33000), false) &&
            diting_gcc_branch_ready(rd(gGcc + 0x3300c), false) &&
            diting_gcc_branch_ready(rd(gGcc + 0x274dc), false)) {
            return true;
        }
        sleepUs(10);
    }
    return false;
}

static DitingGeniSpiState gGeniState;
static bool (*gSpiTransfer)(const uint8_t *, uint8_t *, uint32_t);
static uint8_t gSpiTx[1088], gSpiRx[1088];

static uint32_t geniReadIo(void *, unsigned off) { return rd(gSe + off); }
static void geniWriteIo(void *, unsigned off, uint32_t val) { wr(gSe + off, val); }
static uint64_t geniNowIo(void *) { return nowUs(); }
static void geniDelayIo(void *, unsigned us) { sleepUs(us); }
static void geniSelectIo(void *, bool sel) { gpioSet(kPinCs, !sel); }

static const char *geniSpiInit(uint32_t hz) {
    if (rd(gSe + 0x040) & DITING_GENI_ACTIVE) return "active firmware transaction";
    if (((rd(gSe + 0x068) >> 8) & 0xffu) != 1) return "no SPI firmware";
    if (rd(gSe + 0x064) & 1u) return "FIFO disabled";
    uint32_t txDepth = (rd(gSe + 0xe24) >> 16) & 0x3fu;
    uint32_t rxDepth = (rd(gSe + 0xe28) >> 16) & 0x3fu;
    if (txDepth < 4 || rxDepth < 4 ||
        ((rd(gSe + 0xe24) >> 24) & 0x3fu) != 32 || ((rd(gSe + 0xe28) >> 24) & 0x3fu) != 32) {
        return "bad FIFO params";
    }
    gGeniState.tx_depth = txDepth;
    gGeniState.rx_depth = rxDepth;
    wr(gSe + 0xe18, 0);
    wr(gSe + 0x618, 0xffffffffu);
    wr(gSe + 0x648, 0xffffffffu);
    wr(gSe + 0xc44, 0xffffffffu);
    wr(gSe + 0xd44, 0xffffffffu);
    wr(gSe + 0x028, rd(gSe + 0x028) | 0x3u);
    wr(gSe + 0xe30, rd(gSe + 0xe30) | 0x3u);
    wr(gSe + 0x024, 0x7fu);
    wr(gSe + 0x020, 1u);
    wr(gSe + 0x258, rd(gSe + 0x258) & ~1u);
    wr(gSe + 0x810, rxDepth - 3);
    wr(gSe + 0x814, rxDepth - 2);
    wr(gSe + 0x80c, 0);
    wr(gSe + 0xe1c, rd(gSe + 0xe1c) | 0xfu);
    wr(gSe + 0x614, 0x3300003fu);
    wr(gSe + 0x22c, 0);
    wr(gSe + 0x250, 0);
    wr(gSe + 0x224, 0);
    wr(gSe + 0x230, 0);
    wr(gSe + 0x24c, 0);
    wr(gSe + 0x25c, rd(gSe + 0x25c) & ~1u);
    wr(gSe + 0x278, 0);
    wr(gSe + 0x260, 0x0007f8feu);
    wr(gSe + 0x264, 0x000ffefeu);
    wr(gSe + 0x284, 0x0007f8feu);
    wr(gSe + 0x288, 0x000ffefeu);
    wr(gSe + 0x268, 4u);
    wr(gSe + 0x254, 0);
    uint32_t div = (gSeSourceHz + hz - 1) / hz;
    wr(gSe + 0x07c, gSeClockSel & 7u);
    wr(gSe + 0x048, ((div ? div : 1) << 4) | 1u);
    return nullptr;
}

static bool geniTransfer(const uint8_t *tx, uint8_t *rx, uint32_t len) {
    const DitingGeniSpiIo io = {nullptr, geniReadIo, geniWriteIo, geniNowIo, geniDelayIo, geniSelectIo};
    return diting_geni_spi_transfer(&io, &gGeniState, tx, rx, len) == DitingGeniSpiOk;
}

static bool bitbangTransfer(const uint8_t *tx, uint8_t *rx, uint32_t len) {
    gpioSet(kPinCs, false);
    sleepUs(2);
    for (uint32_t i = 0; i < len; ++i) {
        uint8_t out = tx ? tx[i] : 0, in = 0;
        for (int b = 7; b >= 0; --b) {
            gpioSet(kPinMosi, ((out >> b) & 1) != 0);
            sleepUs(1);
            gpioSet(kPinClk, true);
            sleepUs(1);
            in = uint8_t((in << 1) | (gpioGet(kPinMiso) ? 1 : 0));
            gpioSet(kPinClk, false);
        }
        if (rx) rx[i] = in;
    }
    sleepUs(10);
    gpioSet(kPinCs, true);
    return true;
}

static bool stReadIo(void *, uint8_t op, unsigned addrBytes, uint32_t addr, uint8_t *data, unsigned len) {
    return diting_st61y_read_wire(gSpiTransfer, op, addrBytes, addr, data, len, gSpiTx, gSpiRx, sizeof(gSpiTx));
}
static bool stWriteIo(void *, const uint8_t *data, unsigned len) {
    return len && len <= sizeof(gSpiTx) && gSpiTransfer(data, nullptr, len);
}
static bool stResetIo(void *) {
    if (gSpiTransfer == geniTransfer) {
        const DitingGeniSpiIo io = {nullptr, geniReadIo, geniWriteIo, geniNowIo, geniDelayIo, geniSelectIo};
        if (!diting_geni_spi_reap(&io, &gGeniState) || (rd(gSe + 0x040) & DITING_GENI_ACTIVE)) return false;
    }
    gpioSet(kPinReset, false);
    if (rd(gpioIo(kPinReset)) & 2u) return false;
    sleepMs(10);
    gpioSet(kPinReset, true);
    return (rd(gpioIo(kPinReset)) & 2u) != 0;
}
static uint64_t stNowMs(void *) { return nowUs() / 1000; }
static void stSleepMs(void *, unsigned ms) { sleepMs(ms); }

struct ActiveFinger {
    bool down;
    int x, y;
};
static ActiveFinger gFingers[DITING_ST61Y_CONTACTS];
static ActiveFinger gInjectFinger;
static uint64_t gFingerLastMs[DITING_ST61Y_CONTACTS];
static uint64_t gLastAnyEventMs;
static uint32_t gBatchDownMask;
static bool gBatchDirty;
static uint32_t gTouchEvents;
static uint32_t gInvalidEvents;
static uint32_t gFifoOverflows;
static uint32_t gStatusReleases;
static uint32_t gStaleReleases;
static uint32_t gExternalReleases;
static uint32_t gRecoveryRestarts;
static uint32_t gDiagLogs;
static int gChargerMode = -1;
static volatile uint32_t gReleaseRequested;
static volatile uint32_t gResetRequested;

static void publishFrameLockedAndUnlock(void) {
    uint32_t seq = native_touch_seq;
    native_touch_seq = seq + 1;
    __asm__ volatile("dmb ish" ::: "memory");
    unsigned cnt = 0;
    for (unsigned i = 0; i < DITING_ST61Y_CONTACTS && cnt < 10; ++i) {
        if (!gFingers[i].down) continue;
        native_touch_ids[cnt] = int(i);
        native_touch_xs[cnt] = gFingers[i].x;
        native_touch_ys[cnt] = gFingers[i].y;
        ++cnt;
    }
    if (gInjectFinger.down && cnt < 10) {
        native_touch_ids[cnt] = int(DITING_ST61Y_CONTACTS);
        native_touch_xs[cnt] = gInjectFinger.x;
        native_touch_ys[cnt] = gInjectFinger.y;
        ++cnt;
    }
    native_touch_count = cnt;
    __asm__ volatile("dmb ish" ::: "memory");
    native_touch_seq = seq + 2;
    if (cnt > 0) diting_power_note_touch();
    void (*sink)(void *) = gTouchSink;
    void *ctx = gTouchSinkCtx;
    if (gSinkLock) IOLockUnlock(gSinkLock);
    if (sink) sink(ctx);
}

static void publishFrame() {
    if (!gBatchDirty) return;
    gBatchDirty = false;
    gBatchDownMask = 0;
    if (gSinkLock) IOLockLock(gSinkLock);
    publishFrameLockedAndUnlock();
}

static void stEmitIo(void *, unsigned type, unsigned id, uint32_t x, uint32_t y) {
    if (id >= DITING_ST61Y_CONTACTS) return;
    const uint64_t now = stNowMs(nullptr);
    if (type == DitingSt61yUp) {
        if (gBatchDownMask & (1u << id)) {
            /* Same slot entered and left within one FIFO batch: flush the DOWN
             * frame first so fast taps are not collapsed to zero. */
            publishFrame();
        }
        if (gSinkLock) IOLockLock(gSinkLock);
        gFingers[id].down = false;
        if (gSinkLock) IOLockUnlock(gSinkLock);
    } else {
        if (gSinkLock) IOLockLock(gSinkLock);
        gFingers[id].down = true;
        gFingers[id].x = int(x);
        gFingers[id].y = int(y);
        if (gSinkLock) IOLockUnlock(gSinkLock);
        gFingerLastMs[id] = now;
        if (type == DitingSt61yDown) gBatchDownMask |= (1u << id);
    }
    gLastAnyEventMs = now;
    gBatchDirty = true;
    ++gTouchEvents;
    if (gTouchEvents <= 12) {
        IOLog("DITING_TOUCH: %s id=%u x=%u y=%u\n",
              type == DitingSt61yDown ? "DOWN" : (type == DitingSt61yMove ? "MOVE" : "UP"), id, x, y);
    }
}

struct HidInjectJob {
    int x1, y1, x2, y2;
    unsigned ms;
    bool isSwipe;
};
static HidInjectJob gHidJob;
static volatile uint32_t gHidBusy;

static void hidInjectEmit(bool down, int xPermille, int yPermille) {
    if (xPermille < 0) xPermille = 0;
    if (xPermille > 1000) xPermille = 1000;
    if (yPermille < 0) yPermille = 0;
    if (yPermille > 1000) yPermille = 1000;
    const int sx = (xPermille * 65535) / 1000;
    const int sy = (yPermille * 65535) / 1000;
    if (gSinkLock) IOLockLock(gSinkLock);
    gInjectFinger.down = down;
    gInjectFinger.x = sx;
    gInjectFinger.y = sy;
    publishFrameLockedAndUnlock();
}

static void hidInjectThread(void *, wait_result_t) {
    const HidInjectJob job = gHidJob;
    if (!job.isSwipe) {
        hidInjectEmit(true, job.x1, job.y1);
        IOSleep(60);
        hidInjectEmit(false, job.x1, job.y1);
    } else {
        const unsigned steps = 16;
        unsigned stepMs = job.ms / steps;
        if (stepMs < 8) stepMs = 8;
        for (unsigned i = 0; i <= steps; ++i) {
            const int x = job.x1 + ((job.x2 - job.x1) * int(i)) / int(steps);
            const int y = job.y1 + ((job.y2 - job.y1) * int(i)) / int(steps);
            hidInjectEmit(true, x, y);
            IOSleep(stepMs);
        }
        hidInjectEmit(false, job.x2, job.y2);
    }
    __atomic_store_n(&gHidBusy, 0u, __ATOMIC_RELEASE);
    thread_terminate(current_thread());
}

static bool parseIntToken(const char *&p, int &out) {
    while (*p == ' ') ++p;
    if (*p < '0' || *p > '9') return false;
    int v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        ++p;
    }
    out = v;
    return true;
}

static DitingSt61yState gStState;
static DitingSt61yInfo gStInfo;

static unsigned activeStCount() {
    unsigned n = 0;
    for (unsigned i = 0; i < DITING_ST61Y_CONTACTS; ++i) {
        if (gStState.contact[i].down) ++n;
    }
    return n;
}

static void syncChargerMode(const DitingSt61yIo *io) {
    const int usb = diting_battery_usb_online();
    if ((usb != 0 && usb != 1) || usb == gChargerMode) return;
    if (diting_st61y_set_charger(io, usb == 1)) {
        gChargerMode = usb;
        IOLog("DITING_TOUCH: charger mode=%s\n", usb ? "wired" : "battery");
    }
}

/* Guard against missed LEAVE_POINT or hardware baseline lockup:
 * 1) If >=2 slots are down and one slot stays sub-pixel frozen for >=2.5s while
 *    another slot receives newer events (>=1.5s later), release that orphan.
 * 2) If INT is idle and ZERO FIFO events arrive for >=3s (multi-touch) or
 *    >=8s (single touch), release all retained contacts.
 * Returns true if >=4 contacts froze simultaneously (triggers clean FIFO flush/reinit). */
static bool checkStaleContacts(uint64_t nowMs, bool intAsserted) {
    unsigned active = activeStCount();
    if (!active) return false;

    if (active >= 2 && nowMs >= gLastAnyEventMs && (nowMs - gLastAnyEventMs) < 2000u) {
        bool releasedAny = false;
        for (unsigned i = 0; i < DITING_ST61Y_CONTACTS; ++i) {
            DitingSt61yContact *c = &gStState.contact[i];
            if (!c->down) continue;
            if (nowMs >= gFingerLastMs[i] + 2500u && gLastAnyEventMs >= gFingerLastMs[i] + 1500u) {
                const uint64_t age = nowMs - gFingerLastMs[i];
                c->down = false;
                stEmitIo(nullptr, DitingSt61yUp, i, c->x, c->y);
                ++gStaleReleases;
                releasedAny = true;
                if (gDiagLogs < 32) {
                    ++gDiagLogs;
                    IOLog("DITING_TOUCH: released orphan slot=%u age=%llums\n",
                          i, (unsigned long long)age);
                }
            }
        }
        if (releasedAny) publishFrame();
    }

    active = activeStCount();
    if (!active || intAsserted || nowMs < gLastAnyEventMs) return false;
    const uint64_t idleMs = nowMs - gLastAnyEventMs;
    const uint64_t thresholdMs = active >= 2 ? 3000u : 8000u;
    if (idleMs >= thresholdMs) {
        const unsigned stuckCount = active;
        diting_st61y_release(&gStState, stEmitIo, nullptr);
        publishFrame();
        ++gStaleReleases;
        if (gDiagLogs < 32) {
            ++gDiagLogs;
            IOLog("DITING_TOUCH: released %u stale contact(s) after %llums idle\n",
                  stuckCount, (unsigned long long)idleMs);
        }
        return stuckCount >= 4;
    }
    return false;
}

static bool stStart() {
    const DitingSt61yIo io = {nullptr, stReadIo, stWriteIo, stResetIo, stNowMs, stSleepMs};
    gStState.max_x = kMaxX;
    gStState.max_y = kMaxY;
    gStState.flip = 0;
    gChargerMode = -1;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        uint8_t last[8] = {};
        DitingSt61yResult r = diting_st61y_start(&io, &gStInfo, last);
        if (r == DitingSt61yOk) {
            IOLog("DITING_TOUCH: ST61Y ready fw=%04x cfg=%04x proto=%02x sensor=%ux%u\n",
                  gStInfo.firmware, gStInfo.config, gStInfo.protocol, gStInfo.sensor_x, gStInfo.sensor_y);
            sleepMs(12);
            syncChargerMode(&io);
            return true;
        }
        IOLog("DITING_TOUCH: ST61Y attempt=%u fail=%s\n", attempt + 1, diting_st61y_reason(r));
        if (r == DitingSt61yIdentity || r == DitingSt61ySysInfo) break;
        sleepMs(20);
    }
    return false;
}

static IOMemoryMap *mapMmio(uint64_t phys, size_t bytes, bool wc) {
    IODeviceMemory *mem = IODeviceMemory::withRange(phys, bytes);
    if (!mem) return nullptr;
    IOMemoryMap *m = mem->map(kIOMapAnywhere | (wc ? kIOMapWriteCombineCache : kIOMapInhibitCache));
    mem->release();
    return m;
}

static void touchWorker(void *, wait_result_t) {
    gTlmmMap = mapMmio(kTlmmPhys, kTlmmSize, false);
    gGccMap = mapMmio(kGccPhys, kGccSize, false);
    gSeMap = mapMmio(kSePhys, kSeSize, false);
    gCmdDbMap = mapMmio(kCmdDbPhys, kCmdDbSize, true);
    if (!gTlmmMap || !gGccMap || !gSeMap || !gCmdDbMap) {
        IOLog("DITING_TOUCH: MMIO map failed\n");
        thread_terminate(current_thread());
        return;
    }
    gTlmm = gTlmmMap->getVirtualAddress();
    gGcc = gGccMap->getVirtualAddress();
    gSe = gSeMap->getVirtualAddress();
    gCmdDb = gCmdDbMap->getVirtualAddress();

    if (!gpioConfigure(kPinId, 0, kPullNone, 2, false) || !gpioGet(kPinId)) {
        IOLog("DITING_TOUCH: selector pin %u not ST61Y (val=%u)\n", kPinId, gpioGet(kPinId));
        thread_terminate(current_thread());
        return;
    }
    if (!gpioOutput(kPinReset, false, kPullUp, 8) || !gpioConfigure(kPinInt, 0, kPullUp, 8, false)) {
        IOLog("DITING_TOUCH: reset/INT GPIO failed\n");
        thread_terminate(current_thread());
        return;
    }
    if (!ioSupplyOn()) {
        IOLog("DITING_TOUCH: supply failed\n");
        thread_terminate(current_thread());
        return;
    }
    if (!clocksOn()) {
        IOLog("DITING_TOUCH: clocks failed qup0=%x/%x/%x/%x se4=%x vote=%x\n",
              rd(gGcc + 0x27004), rd(gGcc + 0x27008), rd(gGcc + 0x33000),
              rd(gGcc + 0x3300c), rd(gGcc + 0x274dc), rd(gGcc + 0x62008));
        thread_terminate(current_thread());
        return;
    }
    const char *err = geniSpiInit(4000000u);
    if (!err &&
        gpioConfigure(kPinMiso, kPinFuncSpi, kPullNone, 6, false) &&
        gpioConfigure(kPinMosi, kPinFuncSpi, kPullNone, 6, false) &&
        gpioConfigure(kPinClk, kPinFuncSpi, kPullNone, 6, false) &&
        gpioOutput(kPinCs, true, kPullNone, 6)) {
        gSpiTransfer = geniTransfer;
    } else {
        IOLog("DITING_TOUCH: GENI fallback (%s), using GPIO SPI\n", err ? err : "pins");
        gpioOutput(kPinCs, true, kPullNone, 6);
        gpioOutput(kPinClk, false, kPullNone, 6);
        gpioOutput(kPinMosi, false, kPullNone, 6);
        gpioConfigure(kPinMiso, 0, kPullNone, 2, false);
        gSpiTransfer = bitbangTransfer;
    }
    if (!stStart()) {
        thread_terminate(current_thread());
        return;
    }
    const DitingSt61yIo io = {nullptr, stReadIo, stWriteIo, stResetIo, stNowMs, stSleepMs};
    unsigned idle = 0, failures = 0, restarts = 0;
    uint64_t nextChargerCheckMs = stNowMs(nullptr) + 500u;
    uint64_t lastMultiGhostMs = 0;
    for (;;) {
        if (__atomic_exchange_n(&gResetRequested, 0u, __ATOMIC_ACQ_REL)) {
            diting_st61y_release(&gStState, stEmitIo, nullptr);
            if (gSinkLock) IOLockLock(gSinkLock);
            gInjectFinger.down = false;
            publishFrameLockedAndUnlock();
            gBatchDirty = false;
            ++gExternalReleases;
            ++gRecoveryRestarts;
            failures = 0;
            restarts = 0;
            IOLog("DITING_TOUCH: external ST61Y reset requested\n");
            if (!stStart()) {
                IOLog("DITING_TOUCH: external reset failed\n");
            }
            sleepMs(5);
            continue;
        }
        if (__atomic_exchange_n(&gReleaseRequested, 0u, __ATOMIC_ACQ_REL)) {
            const unsigned had = activeStCount();
            diting_st61y_release(&gStState, stEmitIo, nullptr);
            if (gSinkLock) IOLockLock(gSinkLock);
            gInjectFinger.down = false;
            publishFrameLockedAndUnlock();
            gBatchDirty = false;
            ++gExternalReleases;
            if (had && gDiagLogs < 32) {
                ++gDiagLogs;
                IOLog("DITING_TOUCH: external release cleared %u contact(s)\n", had);
            }
        }

        const bool intAsserted = !gpioGet(kPinInt);
        if (intAsserted || ++idle >= 10) {
            idle = 0;
            bool needRestart = false;
            for (unsigned batch = 0; batch < 4; ++batch) {
                uint32_t before = gTouchEvents;
                DitingSt61yPollReport rep{};
                DitingSt61yPollResult pr = diting_st61y_poll(&io, &gStState, stEmitIo, nullptr, &rep);
                publishFrame();
                if (gTouchEvents != before) restarts = 0;
                if (rep.status_releases) {
                    gStatusReleases += rep.status_releases;
                    if (gDiagLogs < 32) {
                        ++gDiagLogs;
                        IOLog("DITING_TOUCH: status update released contacts (total=%u)\n", gStatusReleases);
                    }
                }
                if (rep.overflow) {
                    ++gFifoOverflows;
                    if (gDiagLogs < 32) {
                        ++gDiagLogs;
                        IOLog("DITING_TOUCH: FIFO full (32 events); released contacts (ovf=%u)\n", gFifoOverflows);
                    }
                }
                if (rep.invalid) {
                    gInvalidEvents += rep.invalid;
                    if (gDiagLogs < 32) {
                        ++gDiagLogs;
                        IOLog("DITING_TOUCH: invalid event (%u) last=%02x%02x%02x%02x%02x%02x%02x%02x; released contacts\n",
                              rep.invalid, rep.last[0], rep.last[1], rep.last[2], rep.last[3],
                              rep.last[4], rep.last[5], rep.last[6], rep.last[7]);
                    }
                }
                if (pr == DitingSt61yPollIdle) {
                    failures = 0;
                    break;
                }
                if (pr == DitingSt61yPollEvents) {
                    failures = 0;
                    if (gpioGet(kPinInt)) break;
                    continue;
                }
                if (pr == DitingSt61yPollError || pr == DitingSt61yPollReset || ++failures >= 3) {
                    needRestart = true;
                    break;
                }
                break;
            }
            if (needRestart) {
                diting_st61y_release(&gStState, stEmitIo, nullptr);
                publishFrame();
                ++gRecoveryRestarts;
                if (++restarts > 3 || !stStart()) {
                    IOLog("DITING_TOUCH: stopped after recovery exhaustion\n");
                    thread_terminate(current_thread());
                    return;
                }
                failures = 0;
            }
        }

        const uint64_t nowMs = stNowMs(nullptr);
        if (checkStaleContacts(nowMs, !gpioGet(kPinInt))) {
            /* Repeated >=4-finger ghost freeze: re-initialize ST61Y cleanly. */
            if (lastMultiGhostMs && (nowMs - lastMultiGhostMs) <= 30000u) {
                ++gRecoveryRestarts;
                IOLog("DITING_TOUCH: repeated multi-ghost freeze; resetting ST61Y\n");
                (void)stStart();
                lastMultiGhostMs = 0;
            } else {
                lastMultiGhostMs = nowMs;
            }
        }

        const unsigned activeNow = activeStCount();
        if (!activeNow && nowMs >= nextChargerCheckMs) {
            nextChargerCheckMs = nowMs + 500u;
            syncChargerMode(&io);
        }
        sleepMs(activeNow ? 2 : 5);
    }
}
} // namespace

extern "C" {
void diting_touch_request_release(void) {
    __atomic_store_n(&gReleaseRequested, 1u, __ATOMIC_RELEASE);
}

void native_touch_set_sink(void (*sink)(void *), void *ctx) {
    if (!gSinkLock) gSinkLock = IOLockAlloc();
    if (gSinkLock) IOLockLock(gSinkLock);
    gTouchSink = sink;
    gTouchSinkCtx = ctx;
    if (gSinkLock) IOLockUnlock(gSinkLock);
}

unsigned diting_touch_usb_command(const char *commandLine, char *out, unsigned cap) {
    if (!out || cap < 64) return 0;
    const char *p = commandLine ? commandLine : "";
    if (!strncmp(p, "hid ", 4)) p += 4;
    if (!strcmp(p, "release")) {
        __atomic_store_n(&gReleaseRequested, 1u, __ATOMIC_RELEASE);
        int n = snprintf(out, cap, "HID status=OK op=release active=%u\n", native_touch_count);
        return n > 0 ? unsigned(n) : 0;
    }
    if (!strcmp(p, "reset")) {
        __atomic_store_n(&gResetRequested, 1u, __ATOMIC_RELEASE);
        int n = snprintf(out, cap, "HID status=OK op=reset active=%u\n", native_touch_count);
        return n > 0 ? unsigned(n) : 0;
    }
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0, ms = 300;
    HidInjectJob job{};
    if (!strncmp(p, "tap ", 4)) {
        p += 4;
        if (!parseIntToken(p, x1) || !parseIntToken(p, y1)) {
            int n = snprintf(out, cap, "HID status=EINVAL usage=hid tap X(0..1000) Y(0..1000)\n");
            return n > 0 ? unsigned(n) : 0;
        }
        job = {x1, y1, x1, y1, 60u, false};
    } else if (!strncmp(p, "swipe ", 6)) {
        p += 6;
        if (!parseIntToken(p, x1) || !parseIntToken(p, y1) ||
            !parseIntToken(p, x2) || !parseIntToken(p, y2)) {
            int n = snprintf(out, cap, "HID status=EINVAL usage=hid swipe X1 Y1 X2 Y2 [MS]\n");
            return n > 0 ? unsigned(n) : 0;
        }
        if (parseIntToken(p, ms)) {
            if (ms < 50) ms = 50;
            if (ms > 2000) ms = 2000;
        } else {
            ms = 300;
        }
        job = {x1, y1, x2, y2, unsigned(ms), true};
    } else {
        int n = snprintf(
            out, cap,
            "HID reports=%u events=%u active=%u multi=%u max=%u ups=%u invalid=%u ovf=%u status_rel=%u stale_rel=%u ext_rel=%u restarts=%u chg=%d spi=%s sink=%u usage=hid tap|swipe|release|reset\n",
            native_touch_hid_reports, gTouchEvents, native_touch_count,
            native_touch_hid_multi[0], native_touch_hid_multi[1], native_touch_hid_multi[2],
            gInvalidEvents, gFifoOverflows, gStatusReleases, gStaleReleases,
            gExternalReleases, gRecoveryRestarts, gChargerMode,
            gSpiTransfer == geniTransfer ? "geni" : (gSpiTransfer == bitbangTransfer ? "gpio" : "none"),
            gTouchSink != nullptr);
        return n > 0 ? unsigned(n) : 0;
    }
    if (!gTouchSink) {
        int n = snprintf(out, cap, "HID status=NOSINK\n");
        return n > 0 ? unsigned(n) : 0;
    }
    if (__atomic_exchange_n(&gHidBusy, 1u, __ATOMIC_ACQ_REL)) {
        int n = snprintf(out, cap, "HID status=BUSY\n");
        return n > 0 ? unsigned(n) : 0;
    }
    gHidJob = job;
    thread_t th = THREAD_NULL;
    if (kernel_thread_start(hidInjectThread, nullptr, &th) != KERN_SUCCESS) {
        __atomic_store_n(&gHidBusy, 0u, __ATOMIC_RELEASE);
        int n = snprintf(out, cap, "HID status=ETHREAD\n");
        return n > 0 ? unsigned(n) : 0;
    }
    thread_deallocate(th);
    int n = snprintf(out, cap, "HID status=OK op=%s from=%d,%d to=%d,%d ms=%u\n",
                     job.isSwipe ? "swipe" : "tap", job.x1, job.y1, job.x2, job.y2, job.ms);
    return n > 0 ? unsigned(n) : 0;
}

void diting_touch27_start(void) {
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("diting_touch", &enabled, sizeof(enabled)) || enabled != 1) return;
    if (!gSinkLock) gSinkLock = IOLockAlloc();
    thread_t th = THREAD_NULL;
    if (kernel_thread_start(touchWorker, nullptr, &th) == KERN_SUCCESS) {
        thread_deallocate(th);
        IOLog("DITING_TOUCH: ST61Y SPI touch worker started\n");
    }
}
}
