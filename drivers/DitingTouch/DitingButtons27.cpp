// SPDX-License-Identifier: BSD-2-Clause-Patent
// Diting SM8475 PMIC physical keys (Power, Volume Up, Volume Down) for iOS 27 arm64e.
// Hardware inputs (verified from Diting live DT and iPadOS 18 diting_buttons.c):
//   - SPMI arbiter v7: core 0x0c400000, cnfg 0x0c42d000, obsrvr 0x0c440000,
//     chnls 0x0c500000, intr 0x0c4c0000, EE 0.
//   - PMK8350 PON (PPID 0x013): RT_STS(0x10) bit 7 = Power, bit 6 = Volume Down (RESIN).
//   - PM8350 GPIO6 (PPID 0x18d): RT_STS(0x10) bit 0 active-low = Volume Up.
//   - PDC (0x0b220000) pin 1 -> GIC SPI 481 (irq 32 + 481), with bounded observer polling fallback.
// Delivers HID report 2 bits (bit 1 = Power, bit 2 = Vol+, bit 3 = Vol-) via NativeHidInput27.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLocks.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <pexpert/device_tree.h>
#include <pexpert/pexpert.h>
#include <stdint.h>
#include <string.h>

extern "C" {
extern int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *) __attribute__((weak_import));
extern kern_return_t thread_terminate(thread_t);
extern void diting_touch_request_release(void);
// [0] stage (1 mapped, 2 configured, 3 armed/running, negative = failed step)
// [1] irq count, [2] hid reports sent, [3] pressed mask (report 2 bits),
// [4] PON APID, [5] GPIO6 APID, [6] irq_mode (1=IRQ, 0=poll),
// [7] PDC version, [8] last error, [9] irq storm fallbacks
volatile int32_t native_buttons_stats[10];
}

namespace {
constexpr uint64_t kCorePhys = 0x0c400000ULL;
constexpr uint64_t kCnfgPhys = 0x0c42d000ULL;
constexpr uint64_t kObsPhys  = 0x0c440000ULL;
constexpr uint64_t kChnPhys  = 0x0c500000ULL;
constexpr uint64_t kPdcPhys  = 0x0b220000ULL;

constexpr uint32_t kPonPpid  = 0x013u;
constexpr uint32_t kGpioPpid = 0x18du;
constexpr uint32_t kPdcPin   = 1u;
constexpr uint32_t kSpi      = 481u;

constexpr uint32_t kReportPower      = 1u << 1;
constexpr uint32_t kReportVolumeUp   = 1u << 2;
constexpr uint32_t kReportVolumeDown = 1u << 3;

struct Window {
    IOMemoryMap *map = nullptr;
    volatile uint32_t *va = nullptr;
};

Window gCore, gCnfg, gObs, gChn, gPdc;
bool gV7 = false;
uint32_t gChannels = 0;

struct Peripheral {
    uint16_t ppid = 0;
    uint16_t channel = 0;
    bool found = false;
};
Peripheral gPon = {kPonPpid, 0, false};
Peripheral gPin = {kGpioPpid, 0, false};

struct KeyState {
    bool usable = false;
    bool live = false;
    bool last = false;
    bool down = false;
    uint32_t reportBit = 0;
    const char *name = "";
};
KeyState gKeys[3] = {
    {false, false, false, false, kReportPower, "power"},
    {false, false, false, false, kReportVolumeUp, "vol+"},
    {false, false, false, false, kReportVolumeDown, "vol-"},
};

volatile uint32_t gIrqEvents = 0;
bool gIrqMode = false;
uint32_t gPressedMask = 0;

void (*volatile gSink)(void *, uint32_t) = nullptr;
void *volatile gSinkCtx = nullptr;
IOLock *gSinkLock = nullptr;

bool mapWindow(Window &w, uint64_t base, uint64_t size) {
    IODeviceMemory *mem = IODeviceMemory::withRange(base, size);
    w.map = mem ? mem->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (mem) mem->release();
    w.va = w.map ? reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}

inline uint32_t rd(volatile uint32_t *base, uint32_t off) {
    uint32_t v = base[off / 4];
    __asm__ volatile("dmb oshld" ::: "memory");
    return v;
}

inline void wr(volatile uint32_t *base, uint32_t off, uint32_t val) {
    base[off / 4] = val;
    __asm__ volatile("dsb sy" ::: "memory");
}

bool dtProp(DTEntry e, const char *name, void *out, unsigned size) {
    void const *p = nullptr;
    unsigned n = 0;
    if (SecureDTGetProperty(e, name, &p, &n) != kSuccess || !p || n != size) return false;
    memcpy(out, p, size);
    return true;
}

bool loadConfig() {
    DTEntry e = nullptr;
    uint64_t c = 0, n = 0, o = 0;
    uint32_t ee = ~0u, a = 0, b = 0;
    if (SecureDTLookupEntry(nullptr, "/arm-io/diting-buttons", &e) != kSuccess ||
        !dtProp(e, "core-base", &c, 8) || !dtProp(e, "cnfg-base", &n, 8) ||
        !dtProp(e, "observer-base", &o, 8) || !dtProp(e, "ee", &ee, 4) ||
        !dtProp(e, "pon-ppid", &a, 4) || !dtProp(e, "volume-up-ppid", &b, 4) ||
        c != kCorePhys || n != kCnfgPhys || o != kObsPhys ||
        ee != 0 || a != kPonPpid || b != kGpioPpid) {
        native_buttons_stats[8] = -2;
        native_buttons_stats[0] = -1;
        return false;
    }
    if (!mapWindow(gCore, c, 0x3000) || !mapWindow(gCnfg, n, 0x4000) || !mapWindow(gObs, o, 0x80000)) {
        native_buttons_stats[8] = -12;
        native_buttons_stats[0] = -1;
        return false;
    }
    native_buttons_stats[0] = 1;
    return true;
}

void findChannel(Peripheral &p) {
    for (uint32_t i = 0; i < gChannels; ++i) {
        const uint32_t map = rd(gCore.va, (gV7 ? 0x2000u : 0x900u) + 4u * i);
        if (!map || ((map >> 8) & 0xfffu) != p.ppid) continue;
        const unsigned owner = rd(gCnfg.va, (gV7 ? 0u : 0x700u) + 4u * i) & 7u;
        if (!p.found || owner == 0) {
            p.found = true;
            p.channel = static_cast<uint16_t>(i);
        }
    }
}

bool readRegisters(const Peripheral &p, uint8_t reg, uint8_t *out, unsigned count) {
    if (!p.found || !count || count > 8) return false;
    const uint32_t off = (gV7 ? 0x20u : 0x80u) * p.channel;
    uint64_t limit = 0;
    clock_interval_to_deadline(2000, NSEC_PER_USEC, &limit);
    wr(gObs.va, off, (1u << 27) | (static_cast<uint32_t>(reg) << 4) | (count - 1u));
    uint32_t status = 0;
    do {
        status = rd(gObs.va, off + 8u);
    } while (!(status & 1u) && mach_absolute_time() < limit);
    if ((status & 0xfu) != 1u) return false;
    const uint32_t data[2] = {rd(gObs.va, off + 0x18u), count > 4 ? rd(gObs.va, off + 0x1cu) : 0u};
    for (unsigned i = 0; i < count; ++i) out[i] = static_cast<uint8_t>(data[i / 4] >> (8u * (i % 4)));
    return true;
}

bool peripheralWrite(const Peripheral &p, uint8_t reg, uint8_t value) {
    if (!p.found || !gChn.va) return false;
    const uint32_t off = 0x1000u * p.channel;
    wr(gChn.va, off + 0x10u, value);
    wr(gChn.va, off, static_cast<uint32_t>(reg) << 4);
    uint64_t limit = 0;
    clock_interval_to_deadline(2000, NSEC_PER_USEC, &limit);
    uint32_t status = 0;
    do {
        status = rd(gChn.va, off + 8u);
    } while (!(status & 1u) && mach_absolute_time() < limit);
    return (status & 0xfu) == 1u;
}

// Primary IRQ context: mask PDC pin 1 only and wake worker thread.
// Avoids touching unowned SPMI channels from IRQ context.
void keyIrq(void *) {
    const uint32_t cfgOff = 0x110u + 4u * kPdcPin;
    wr(gPdc.va, cfgOff, rd(gPdc.va, cfgOff) & ~8u);
    native_buttons_stats[1] = native_buttons_stats[1] + 1;
    __atomic_fetch_add(&gIrqEvents, 1u, __ATOMIC_RELEASE);
    thread_wakeup(reinterpret_cast<event_t>(const_cast<uint32_t *>(&gIrqEvents)));
}

bool armKeyIrq(const Peripheral &p, uint8_t bits) {
    const uint32_t map = rd(gCore.va, 0x2000u + 4u * p.channel);
    if (!(map & (1u << 24)) || (rd(gCnfg.va, 4u * p.channel) & 7u) != 0) return false;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        uint8_t type = 0, high = 0, low = 0;
        if (readRegisters(p, 0x11, &type, 1) && readRegisters(p, 0x12, &high, 1) &&
            readRegisters(p, 0x13, &low, 1) &&
            peripheralWrite(p, 0x11, static_cast<uint8_t>(type | bits)) &&
            peripheralWrite(p, 0x12, static_cast<uint8_t>(high | bits)) &&
            peripheralWrite(p, 0x13, static_cast<uint8_t>(low | bits)) &&
            peripheralWrite(p, 0x14, bits) &&
            peripheralWrite(p, 0x15, bits)) {
            wr(gChn.va, 0x1000u * p.channel + 0x100u, 1u);
            return true;
        }
        IOSleep(25);
    }
    return false;
}

void acknowledgeKeyIrq(const Peripheral &p, uint8_t bits) {
    if (!p.found || !gChn.va) return;
    const uint32_t off = 0x1000u * p.channel;
    const uint32_t status = rd(gChn.va, off + 0x104u) & bits;
    if (status) {
        wr(gChn.va, off + 0x108u, status);
        (void)peripheralWrite(p, 0x14, static_cast<uint8_t>(status));
    }
}

void disableIrqs() {
    if (!gIrqMode) return;
    const uint32_t cfgOff = 0x110u + 4u * kPdcPin;
    if (gPdc.va) wr(gPdc.va, cfgOff, rd(gPdc.va, cfgOff) & ~8u);
    if (gPon.found) (void)peripheralWrite(gPon, 0x16, 0xc0);
    if (gPin.found) (void)peripheralWrite(gPin, 0x16, 0x01);
    gIrqMode = false;
    native_buttons_stats[6] = 0;
}

bool enableIrqs() {
    if (!gV7 || !gPon.found || !gPin.found || !armcpu_spi_attach) return false;
    for (unsigned i = 0; i < 2; ++i) {
        const Peripheral &p = i ? gPin : gPon;
        if (!(rd(gCore.va, 0x2000u + 4u * p.channel) & (1u << 24)) ||
            (rd(gCnfg.va, 4u * p.channel) & 7u) != 0) {
            return false;
        }
    }
    if (!mapWindow(gChn, kChnPhys, 0x400000) || !mapWindow(gPdc, kPdcPhys, 0x2000)) return false;
    const uint32_t pdcVersion = rd(gPdc.va, 0x1000u);
    native_buttons_stats[7] = static_cast<int32_t>(pdcVersion);
    if (pdcVersion < 0x30200u) return false;
    int rc = -1;
    for (unsigned attempt = 0; attempt < 120 && rc; ++attempt) {
        rc = armcpu_spi_attach(32u + kSpi, false, keyIrq, nullptr, nullptr, nullptr);
        if (rc) IOSleep(100);
    }
    if (rc) {
        native_buttons_stats[8] = rc;
        return false;
    }
    if (!armKeyIrq(gPon, 0xc0) || !armKeyIrq(gPin, 0x01)) {
        (void)peripheralWrite(gPon, 0x16, 0xc0);
        (void)peripheralWrite(gPin, 0x16, 0x01);
        native_buttons_stats[8] = -5;
        return false;
    }
    wr(gPdc.va, 0x110u + 4u * kPdcPin, 12u); // level high + enabled (PDC v3.2+)
    native_buttons_stats[6] = 1;
    IOLog("DitingButtons: armed PMIC SPI%u IRQs (PON=%u GPIO6=%u PDC=0x%x)\n",
          kSpi, gPon.channel, gPin.channel, pdcVersion);
    return true;
}

void emitMask(uint32_t mask) {
    native_buttons_stats[3] = static_cast<int32_t>(mask);
    void (*sink)(void *, uint32_t) = nullptr;
    void *ctx = nullptr;
    if (gSinkLock) IOLockLock(gSinkLock);
    sink = gSink;
    ctx = gSinkCtx;
    if (gSinkLock) IOLockUnlock(gSinkLock);
    if (sink) {
        sink(ctx, mask);
        native_buttons_stats[2] = native_buttons_stats[2] + 1;
    }
}

void look(KeyState &k, bool down, bool &changed) {
    const bool steady = (k.last == down);
    k.last = down;
    if (!k.usable || !steady) return;
    if (!k.live) {
        if (!down) k.live = true;
        return;
    }
    if (k.down == down) return;
    k.down = down;
    if (down) gPressedMask |= k.reportBit;
    else gPressedMask &= ~k.reportBit;
    changed = true;
    if (down && k.reportBit == kReportPower) diting_touch_request_release();
    IOLog("DitingButtons: %s %s (mask=0x%x)\n", k.name, down ? "down" : "up", gPressedMask);
}

void buttonsThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "diting-buttons");
    if (!loadConfig()) {
        IOLog("DitingButtons: missing/incompatible /arm-io/diting-buttons DT; SKIP\n");
        thread_terminate(current_thread());
        return;
    }
    const uint32_t version = rd(gCore.va, 0);
    if (version < 0x50000000u || version >= 0x80000000u) {
        native_buttons_stats[8] = static_cast<int32_t>(version);
        native_buttons_stats[0] = -2;
        IOLog("DitingButtons: unsupported SPMI arbiter version 0x%x; SKIP\n", version);
        thread_terminate(current_thread());
        return;
    }
    gV7 = version >= 0x70000000u;
    gChannels = rd(gCore.va, 4u) & 0x7ffu;
    if (!gChannels || gChannels > (gV7 ? 1024u : 512u)) {
        native_buttons_stats[8] = static_cast<int32_t>(gChannels);
        native_buttons_stats[0] = -2;
        IOLog("DitingButtons: invalid SPMI channel count %u; SKIP\n", gChannels);
        thread_terminate(current_thread());
        return;
    }
    findChannel(gPon);
    findChannel(gPin);
    native_buttons_stats[4] = gPon.found ? static_cast<int32_t>(gPon.channel) : -1;
    native_buttons_stats[5] = gPin.found ? static_cast<int32_t>(gPin.channel) : -1;

    uint8_t id[2] = {}, state = 0, cfg[8] = {};
    if (readRegisters(gPon, 4, id, 2) && id[0] == 1 && id[1] == 9 &&
        readRegisters(gPon, 0x10, &state, 1)) {
        gKeys[0].usable = gKeys[2].usable = true;
        gKeys[0].last = (state & 0x80u) != 0;
        gKeys[2].last = (state & 0x40u) != 0;
    }
    if (readRegisters(gPin, 4, id, 2) && id[0] == 0x10 && id[1] >= 0x10 && id[1] <= 0x13 &&
        readRegisters(gPin, 0x40, cfg, 8) && (cfg[0] & 3u) == 0 && (cfg[2] & 7u) < 4u &&
        (cfg[6] & 0x80u) && readRegisters(gPin, 0x10, &state, 1)) {
        gKeys[1].usable = true;
        gKeys[1].last = (state & 1u) == 0;
    }
    if (!(gKeys[0].usable || gKeys[1].usable)) {
        native_buttons_stats[8] = -4;
        native_buttons_stats[0] = -4;
        IOLog("DitingButtons: no verified PMIC button peripheral; SKIP\n");
        thread_terminate(current_thread());
        return;
    }
    for (unsigned i = 0; i < 3; ++i) gKeys[i].live = gKeys[i].usable && !gKeys[i].last;
    native_buttons_stats[0] = 2;
    IOLog("DitingButtons: arbiter v%u channels=%u PON=%u GPIO6=%u power=%u vol+=%u vol-=%u\n",
          gV7 ? 7u : 5u, gChannels, gPon.channel, gPin.channel,
          gKeys[0].usable, gKeys[1].usable, gKeys[2].usable);

    gIrqMode = enableIrqs();
    native_buttons_stats[0] = 3;

    unsigned failures = 0, irqBurst = 0;
    uint64_t irqWindow = mach_absolute_time();
    for (;;) {
        bool good = true, changed = false;
        if (gIrqMode) {
            uint64_t elapsed = 0;
            absolutetime_to_nanoseconds(mach_absolute_time() - irqWindow, &elapsed);
            if (elapsed >= NSEC_PER_SEC) {
                irqBurst = 0;
                irqWindow = mach_absolute_time();
            }
            if (++irqBurst > 128) {
                disableIrqs();
                native_buttons_stats[9] = native_buttons_stats[9] + 1;
                IOLog("DitingButtons: shared SPI%u remained pending; switched to bounded observer polling\n", kSpi);
            }
        }
        if (gKeys[0].usable) {
            if (readRegisters(gPon, 0x10, &state, 1)) {
                look(gKeys[0], (state & 0x80u) != 0, changed);
                look(gKeys[2], (state & 0x40u) != 0, changed);
            } else {
                good = false;
            }
        }
        if (gKeys[1].usable) {
            if (readRegisters(gPin, 0x10, &state, 1)) {
                look(gKeys[1], (state & 1u) == 0, changed);
            } else {
                good = false;
            }
        }
        if (changed) emitMask(gPressedMask);
        failures = good ? 0u : failures + 1u;
        if (failures >= 8) {
            if (gPressedMask) {
                gPressedMask = 0;
                emitMask(0);
            }
            native_buttons_stats[8] = -8;
            native_buttons_stats[0] = -8;
            IOLog("DitingButtons: repeated observer read failure; stopping\n");
            break;
        }
        bool settling = false;
        for (unsigned i = 0; i < 3; ++i) {
            if (gKeys[i].usable && gKeys[i].live && gKeys[i].last != gKeys[i].down) settling = true;
        }
        if (gIrqMode) {
            acknowledgeKeyIrq(gPon, 0xc0);
            acknowledgeKeyIrq(gPin, 0x01);
            const uint32_t seen = __atomic_load_n(&gIrqEvents, __ATOMIC_ACQUIRE);
            assert_wait_timeout(reinterpret_cast<event_t>(const_cast<uint32_t *>(&gIrqEvents)), THREAD_UNINT, settling ? 8 : 64, NSEC_PER_MSEC);
            const uint32_t cfgOff = 0x110u + 4u * kPdcPin;
            wr(gPdc.va, cfgOff, rd(gPdc.va, cfgOff) | 8u);
            if (seen != __atomic_load_n(&gIrqEvents, __ATOMIC_ACQUIRE)) {
                clear_wait(current_thread(), THREAD_AWAKENED);
            } else {
                thread_block(THREAD_CONTINUE_NULL);
            }
        } else {
            uint64_t deadline = 0;
            clock_interval_to_deadline(settling ? 5 : 10, NSEC_PER_MSEC, &deadline);
            assert_wait_deadline(reinterpret_cast<event_t>(gKeys), THREAD_UNINT, deadline);
            thread_block(THREAD_CONTINUE_NULL);
        }
    }
    disableIrqs();
    thread_terminate(current_thread());
}
} // namespace

extern "C" {
void native_buttons_set_sink(void (*sink)(void *, uint32_t), void *context) {
    if (!gSinkLock) gSinkLock = IOLockAlloc();
    if (gSinkLock) IOLockLock(gSinkLock);
    gSink = sink;
    gSinkCtx = context;
    if (gSinkLock) IOLockUnlock(gSinkLock);
}

unsigned diting_buttons_snapshot_text(char *out, unsigned capacity) {
    if (!out || capacity < 32) return 0;
    const int n = snprintf(out, capacity,
        "BUTTONS stage=%d irq_mode=%d irqs=%d reports=%d mask=0x%x pon=%d gpio6=%d pdc=0x%x err=%d fallbacks=%d usable=%u/%u/%u sink=%u\n",
        native_buttons_stats[0], native_buttons_stats[6], native_buttons_stats[1],
        native_buttons_stats[2], (unsigned)native_buttons_stats[3],
        native_buttons_stats[4], native_buttons_stats[5], (unsigned)native_buttons_stats[7],
        native_buttons_stats[8], native_buttons_stats[9],
        gKeys[0].usable, gKeys[1].usable, gKeys[2].usable, gSink != nullptr);
    return n > 0 ? (unsigned(n) < capacity ? unsigned(n) : capacity - 1) : 0;
}

void diting_buttons27_start(void) {
    unsigned enabled = 0;
    if ((!PE_parse_boot_argn("diting_buttons", &enabled, sizeof(enabled)) || enabled != 1) &&
        (!PE_parse_boot_argn("buttons", &enabled, sizeof(enabled)) || enabled != 1)) {
        return;
    }
    if (native_buttons_stats[0]) return;
    if (!gSinkLock) gSinkLock = IOLockAlloc();
    thread_t th = THREAD_NULL;
    if (kernel_thread_start(buttonsThread, nullptr, &th) == KERN_SUCCESS) {
        thread_deallocate(th);
        IOLog("DitingButtons: SM8475 PMIC button worker started\n");
    } else {
        native_buttons_stats[8] = -12;
        native_buttons_stats[0] = -7;
    }
}
}
