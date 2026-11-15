// SPDX-License-Identifier: BSD-2-Clause-Patent
// Diting L12 DCS brightness. Register/packet facts are documented in
// README.backlight.md; this work-loop transport does not reset the display,
// reprogram the PLL, or lend a physical DMA buffer to the DSI controller.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include "DitingBacklightProtocol.h"

extern kmod_info_t g_kernel_kmod_info;
extern "C" void DitingBacklightStaticInit(void);
class DitingBacklight;

namespace {
using namespace DitingBrightness;
DitingBacklight *gPublishedBacklight;
const OSMetaClass *gAppleBacklightClass;
uint64_t nowNs() {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns;
}
}

class DitingBacklight : public IOService {
    OSDeclareDefaultStructors(DitingBacklight)
    Board board = {};
    IOMemoryMap *mapping = nullptr;
    volatile uint32_t *regs = nullptr;
    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *timer = nullptr;
    IOTimerEventSource *requestTimer = nullptr;
    const OSMetaClass *appleClass = nullptr;
    Command command;
    bool published = false, stopped = false;
    bool needVerification = false, queryPending = false, retryPending = false;
    bool lowPowerWrite = false, retriedWrite = false;
    uint64_t verifyAfter = 0;
    enum Operation { SetReadSize, ReadInitial, WriteLevel, ReadVerify } operation = ReadInitial;
    Operation pendingRead = ReadInitial;
    uint32_t level = 0, sending = 0, from = 0, goal = 0;
    uint64_t idleDeadline = 0, classDeadline = 0, fadeStart = 0, fadeDuration = 0;
    uint64_t request = 0;
    uint32_t requestedFade = 0, requests = 0, writes = 0, reads = 0;

    uint32_t rd(uint32_t offset) const { return regs[offset / 4]; }
    void wr(uint32_t offset, uint32_t value) {
        regs[offset / 4] = value;
        __asm__ volatile("dsb sy" ::: "memory");
    }
    uint32_t clamped(uint32_t value) const {
        return DitingBrightness::clamp(board, value);
    }
    static uint32_t readRegister(void *context, uint32_t offset) {
        return static_cast<DitingBacklight *>(context)->rd(offset);
    }
    static void writeRegister(void *context, uint32_t offset, uint32_t value) {
        static_cast<DitingBacklight *>(context)->wr(offset, value);
    }
    static void delay(void *, unsigned microseconds) { IODelay(microseconds); }
    bool beginTransaction(const uint32_t *words, unsigned count, bool lowPower, bool read = false) {
        return !__atomic_load_n(&stopped, __ATOMIC_ACQUIRE) &&
            command.begin(words, count, lowPower, read, nowNs());
    }
    bool startRead(Operation next) {
        // DSI resets to a one-byte return limit. Negotiate two bytes as a
        // separate short packet (bit 6 means LONG, not a short-packet flag).
        // Complete it before resetting readback and issuing the BTA query.
        const uint32_t words[] = {ReturnSizePacket};
        if (!beginTransaction(words, 1, true)) return false;
        pendingRead = next; operation = SetReadSize;
        return true;
    }
    bool startQuery() {
        const uint32_t words[] = {ReadBrightnessPacket};
        if (!beginTransaction(words, 1, true, true)) return false;
        operation = pendingRead;
        return true;
    }
    bool readLevel(uint32_t &answer) {
        uint32_t raw = 0;
        const bool valid = DitingBrightness::readLevel(rd(ReadControl) >> 16,
            rd(ReadData0), rd(ReadData1), raw);
        if (valid) setProperty("DitingBrightnessRawReadback", raw, 32);
        return valid && DitingBrightness::controlLevel(board, raw, answer);
    }
    bool publishLevel() {
        OSDictionary *item = OSDictionary::withCapacity(3);
        OSDictionary *parameters = OSDictionary::withCapacity(1);
        OSNumber *minimum = OSNumber::withNumber(0ULL, 32);
        OSNumber *maximum = OSNumber::withNumber(board.maximum, 32);
        OSNumber *value = OSNumber::withNumber(level, 32);
        const bool ready = item && parameters && minimum && maximum && value &&
            item->setObject("min", minimum) && item->setObject("max", maximum) && item->setObject("value", value) &&
            parameters->setObject("brightness", item) && setProperty("IODisplayParameters", parameters);
        OSSafeReleaseNULL(item); OSSafeReleaseNULL(parameters);
        OSSafeReleaseNULL(minimum); OSSafeReleaseNULL(maximum); OSSafeReleaseNULL(value);
        return ready;
    }
    void reportError(const char *reason) {
        OSString *previous = OSDynamicCast(OSString, getProperty("DitingBacklightError"));
        if (previous && previous->isEqualTo(reason)) return;
        setProperty("DitingBacklightError", reason);
        IOLog("DitingBacklight: %s; requests=%u writes=%u reads=%u status=%x irq=%x ack=%x rx=%x data=%08x/%08x\n",
              reason, __atomic_load_n(&requests, __ATOMIC_RELAXED), writes, reads, rd(Status), rd(InterruptControl), rd(AckError),rd(ReadControl),rd(ReadData0),rd(ReadData1));
    }
    void fail(const char *reason) {
        __atomic_store_n(&stopped, true, __ATOMIC_RELEASE);
        timer->cancelTimeout(); requestTimer->cancelTimeout();
        reportError(reason);
        if (command.inFlight()) command.finish();
    }
    void abandonRetired(const char *reason) {
        // This failed packet has retired. Keep the last measured DBV and
        // permit a newer request; the mailbox has not been consumed yet.
        needVerification = queryPending = retryPending = false;
        goal = from = level; fadeDuration = 0; idleDeadline = 0;
        reportError(reason); timer->cancelTimeout();
        if (__atomic_load_n(&request, __ATOMIC_ACQUIRE) >> 63) timer->setTimeoutMS(1);
    }
    void waitForEngine(const char *reason) {
        const uint64_t now = nowNs();
        if (!idleDeadline) idleDeadline = now + (published ? 200000000ULL : 2000000000ULL);
        if (now < idleDeadline) { timer->setTimeoutMS(2); return; }
        idleDeadline = 0;
        if (!published) fail(reason);
        else {
            // A temporarily busy or sleeping display is still owned by its
            // existing driver. Do not reset it or disable this OS service.
            reportError(reason); timer->cancelTimeout();
        }
    }
    bool publishFirst() {
        // Use the actual AppleARMPlatform metaclass for CoreBrightness's
        // conformance query. The getter below is compiled against Darwin24;
        // no vtable-size assumption, fixed slot, or duplicate class name.
        if (!publishLevel() || !setProperty("DitingBacklightProtocol", "sm8475-diting-dcs-v1") ||
            !setProperty("NativeSandboxGrantProcess", "backboardd") ||
            !setProperty("backlight-control", true) || !setProperty("built-in", true)) {
            removeProperty("backlight-control"); removeProperty("DitingBacklightProtocol");
            fail("brightness service property allocation failed"); return false;
        }
        gAppleBacklightClass = appleClass;
        __atomic_store_n(&gPublishedBacklight, this, __ATOMIC_RELEASE);
        published = true; setName("AppleARMBacklight"); registerService();
        IOLog("DitingBacklight: L12 DCS service ready, level=%u max=%u, readback PASS\n", level, board.maximum);
        return true;
    }
    void service() {
        if (__atomic_load_n(&stopped, __ATOMIC_ACQUIRE)) return;
        if (!appleClass) {
            // AppleARMPlatform initializes asynchronously after framebuffer
            // startup. Never register its private class name in the kernel:
            // its later real initializer would panic on the duplicate.
            const OSSymbol *name = OSSymbol::withCString("AppleARMBacklight");
            appleClass = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
            OSSafeReleaseNULL(name);
            if (!appleClass) {
                if (nowNs() >= classDeadline) { fail("native AppleARMBacklight class unavailable"); return; }
                timer->setTimeoutMS(100); return;
            }
            IOLog("DitingBacklight: native AppleARMBacklight class ready\n");
        }
        if (command.inFlight()) {
            const Completion completion = command.poll(nowNs());
            if (completion == Completion::Retained) {
                // No proven retirement: keep the FIFO/register ownership.
                // A later request can reap a late completion, with no idle
                // polling or reset while the controller still owns it.
                if (!published) fail("initial DSI timeout; command FIFO retained");
                else { reportError("DSI timeout; command FIFO retained"); timer->cancelTimeout(); }
                return;
            }
            if (completion == Completion::Pending) {
                timer->setTimeoutMS(1); return;
            }
            uint32_t returned = 0;
            const bool readOk = completion == Completion::Done &&
                (operation == WriteLevel || operation == SetReadSize || readLevel(returned));
            if (completion == Completion::Error) reportError("retired DSI command error");
            if (!command.finish()) { timer->setTimeoutMS(1); return; }
            if (!readOk) {
                // The command has retired, so a malformed response does
                // not leave DMA ownership uncertain. Preserve the last
                // measured level and allow a later OS request to retry.
                // An initial read is still required before publication.
                if (!published) { fail("invalid initial DCS brightness response"); return; }
                abandonRetired(completion == Completion::Error ?
                    "retired DSI command error" : "invalid DCS brightness response");
                return;
            }
            if (operation == SetReadSize) {
                queryPending = true;
            } else if (operation == WriteLevel) {
                writes++; needVerification = true;
                // DMA completion retires the packet, not the panel's DBV
                // latch. Let at least two 120 Hz scans pass before GET.
                verifyAfter = nowNs() + 20000000ULL;
            } else {
                reads++;
                if (operation == ReadVerify && returned != sending) {
                    level = clamped(returned); publishLevel(); needVerification = false;
                    // Retry the same retired write once with the other
                    // command signalling mode after the first retired write.
                    // Never disable every later slider request for a value
                    // the panel rejected. Uncertain DMA retains ownership.
                    if (!retriedWrite) {
                        retriedWrite = retryPending = true; lowPowerWrite = !lowPowerWrite;
                    } else {
                        setProperty("DitingBacklightError", "DBV not accepted");
                        IOLog("DitingBacklight: DBV rejected wanted=%u measured=%u LP=%u; later requests remain available\n",
                              sending, returned, lowPowerWrite);
                        from = goal = level; fadeDuration = 0;
                    }
                } else {
                    level = clamped(returned);
                    removeProperty("DitingBacklightError");
                    retriedWrite = retryPending = false;
                }
                if (!published) { from = goal = level; if (!publishFirst()) return; }
                else publishLevel();
                needVerification = false;
                setProperty("DitingBrightnessReadback", returned, 32);
                setProperty("DitingBrightnessReadQuantum", 1u, 32);
                setProperty("DitingBrightnessReadScale", DitingBrightness::ReadScale, 32);
                setProperty("DitingBrightnessWrites", writes, 32);
                setProperty("DitingBrightnessRequests", __atomic_load_n(&requests, __ATOMIC_RELAXED), 32);
            }
        }
        if (queryPending) {
            if (!startQuery()) { waitForEngine("DSI BTA engine unavailable"); return; }
            queryPending = false; idleDeadline = 0;
            timer->setTimeoutMS(1); return;
        }
        if (needVerification) {
            if (nowNs() < verifyAfter) { timer->setTimeoutMS(2); return; }
            if (!startRead(ReadVerify)) { waitForEngine("DSI readback engine unavailable"); return; }
            idleDeadline = 0;
            timer->setTimeoutMS(1); return;
        }
        if (!published) {
            if (!startRead(ReadInitial)) { waitForEngine("DSI engine not ready at handoff"); return; }
            idleDeadline = 0;
            timer->setTimeoutMS(1); return;
        }
        // Take the newest slider request only after the old packet and its
        // verification retire. A rejected old write cannot erase this goal.
        const uint64_t latest = __atomic_exchange_n(&request, 0ULL, __ATOMIC_ACQUIRE);
        if (latest >> 63) {
            from = level; goal = clamped((latest >> 16) & 0xffff);
            fadeStart = nowNs(); fadeDuration = (latest & 0xffff) * 1000000ULL;
            retriedWrite = retryPending = false; idleDeadline = 0;
            setProperty("DitingBrightnessRequests", __atomic_load_n(&requests, __ATOMIC_RELAXED), 32);
            if (goal == level && getProperty("DitingBacklightError")) {
                // A restore may already equal the last measured level.
                // Re-read it to clear an old error only on real success.
                sending = goal; needVerification = true; verifyAfter = nowNs();
                timer->setTimeoutMS(1); return;
            }
        }
        const uint64_t elapsed = nowNs() - fadeStart;
        uint32_t next = retryPending ? sending : DitingBrightness::fadeLevel(from, goal, elapsed, fadeDuration);
        next = clamped(next);
        if (next != level) {
            sending = next;
            const uint32_t words[] = {WriteBrightnessHeader, DitingBrightness::writePayload(next)};
            if (!beginTransaction(words, 2, lowPowerWrite)) { waitForEngine("DSI write engine unavailable"); return; }
            retryPending = false; idleDeadline = 0;
            operation = WriteLevel;
            timer->setTimeoutMS(1);
        } else if (next != goal) timer->setTimeoutMS(16);
    }
    static void tick(OSObject *owner, IOTimerEventSource *) {
        static_cast<DitingBacklight *>(owner)->service();
    }
public:
    bool prepare() {
        IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
        OSData *data = chosen ? OSDynamicCast(OSData, chosen->getProperty("diting-dsi-backlight")) : nullptr;
        if (data && data->getLength() == sizeof(board)) memcpy(&board, data->getBytesNoCopy(), sizeof(board));
        OSSafeReleaseNULL(chosen);
        if (!DitingBrightness::validBoard(board)) {
            IOLog("DitingBacklight: invalid L12 ADT base=%llx span=%x min=%u max=%u swap=%u channel=%u version=%u\n",
                board.base, board.span, board.minimum, board.maximum, board.swappedDbv, board.channel, board.version);
            return false;
        }
        IODeviceMemory *memory = IODeviceMemory::withRange(board.base, board.span);
        mapping = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        OSSafeReleaseNULL(memory);
        regs = mapping ? reinterpret_cast<volatile uint32_t *>(mapping->getVirtualAddress()) : nullptr;
        if (!regs) return false;
        command.initialize({this, readRegister, writeRegister, delay});
        if (!rd(Version) || rd(Version) == ~0u || !(rd(Control) & 1u) || (rd(PatternControl) & 1u)) {
            IOLog("DitingBacklight: DSI0 unavailable version=%x control=%x pattern=%x\n",rd(Version),rd(Control),rd(PatternControl));
            return false;
        }
        loop = IOWorkLoop::workLoop();
        timer = IOTimerEventSource::timerEventSource(this, tick);
        requestTimer = IOTimerEventSource::timerEventSource(this, tick);
        if (!loop || !timer || !requestTimer || loop->addEventSource(timer) || loop->addEventSource(requestTimer)) return false;
        setProperty("DitingPanel", "Diting L12 AMOLED / SM8475 DSI0");
        setProperty("DitingDSIVersion", rd(Version), 32);
        classDeadline = nowNs() + 30000000000ULL;
        timer->setTimeoutMS(1);
        return true;
    }
    IOReturn setProperties(OSObject *object) override {
        OSDictionary *properties = OSDynamicCast(OSDictionary, object);
        if (!properties) return kIOReturnBadArgument;
        if (__atomic_load_n(&stopped, __ATOMIC_ACQUIRE)) return kIOReturnNotReady;
        // Darwin24 also sends the optional calibrated current-table level.
        // It is metadata, not another L12 DBV command.
        if (properties->getCount() > 3 ||
            properties->getCount() != (properties->getObject("brightness") ? 1u : 0u) +
                                      (properties->getObject("FadePeriod") ? 1u : 0u) +
                                      (properties->getObject("backlight-level") ? 1u : 0u)) return kIOReturnUnsupported;
        OSNumber *brightness = OSDynamicCast(OSNumber, properties->getObject("brightness"));
        OSNumber *fade = OSDynamicCast(OSNumber, properties->getObject("FadePeriod"));
        OSNumber *calibrated = OSDynamicCast(OSNumber, properties->getObject("backlight-level"));
        if ((properties->getObject("brightness") && !brightness) ||
            (properties->getObject("FadePeriod") && !fade) ||
            (properties->getObject("backlight-level") && !calibrated)) return kIOReturnBadArgument;
        const uint32_t ms = fade ? DitingBrightness::fadeMilliseconds(fade->unsigned64BitValue(), fade->numberOfBits()) :
            __atomic_load_n(&requestedFade, __ATOMIC_RELAXED);
        if (fade) __atomic_store_n(&requestedFade, ms, __ATOMIC_RELAXED);
        if (!brightness) return kIOReturnSuccess;
        const uint32_t target = DitingBrightness::requestedLevel(board, brightness->unsigned64BitValue(), brightness->numberOfBits());
        __atomic_fetch_add(&requests, 1u, __ATOMIC_RELAXED);
        __atomic_store_n(&request, (1ULL << 63) | ((uint64_t)target << 16) | ms, __ATOMIC_RELEASE);
        // A separate event source prevents the transaction/fade timer from
        // overwriting a concurrently armed slider wakeup.
        requestTimer->setTimeoutUS(1); // caller never waits on DSI or the work-loop gate
        return kIOReturnSuccess;
    }
    void free() override {
        if (__atomic_load_n(&gPublishedBacklight, __ATOMIC_ACQUIRE) == this)
            __atomic_store_n(&gPublishedBacklight, (DitingBacklight *)nullptr, __ATOMIC_RELEASE);
        if (timer) { timer->cancelTimeout(); if (loop) loop->removeEventSource(timer); }
        if (requestTimer) { requestTimer->cancelTimeout(); if (loop) loop->removeEventSource(requestTimer); }
        OSSafeReleaseNULL(timer); OSSafeReleaseNULL(requestTimer); OSSafeReleaseNULL(loop); OSSafeReleaseNULL(mapping);
        IOService::free();
    }
};

// Expand the ordinary metaclass definition so the conformance getter is
// selected in source, rather than replacing a guessed Darwin17 vtable slot.
DitingBacklight::MetaClass DitingBacklight::gMetaClass;
const OSMetaClass *const DitingBacklight::metaClass = &DitingBacklight::gMetaClass;
const OSMetaClass *const DitingBacklight::superClass = &IOService::gMetaClass;
DitingBacklight::DitingBacklight(const OSMetaClass *meta) : IOService(meta) {}
DitingBacklight::~DitingBacklight() {}
const OSMetaClass *DitingBacklight::getMetaClass() const {
    return __atomic_load_n(&gPublishedBacklight, __ATOMIC_ACQUIRE) == this ? gAppleBacklightClass : &gMetaClass;
}
OSMetaClassConstructorInit(DitingBacklight, IOService, )
OSDefineDefaultStructors(DitingBacklight, IOService)

extern "C" void diting_backlight_start(IOService *display) {
    static DitingBacklight *backlight;
    static bool registered;
    unsigned enabled = 1;
    PE_parse_boot_argn("diting_backlight", &enabled, sizeof(enabled));
    if (!enabled || backlight || !display) return;
    if (!registered) {
        void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
        if (!handle) { IOLog("DitingBacklight: class registration unavailable\n"); return; }
        DitingBacklightStaticInit();
        if (OSMetaClass::postModLoad(handle)) { IOLog("DitingBacklight: class registration failed\n"); return; }
        registered = true;
    }
    DitingBacklight *service = OSTypeAlloc(DitingBacklight);
    if (!service) return;
    if (!service->init() || !service->attach(display) || !service->prepare()) {
        IOLog("DitingBacklight: unavailable DT, AppleARMBacklight ABI or running DSI0; keep current panel brightness\n");
        if (service->getProvider()) service->detach(display);
        service->release(); return;
    }
    backlight = service; // the service and hardware handoff persist until reboot
}
