// SPDX-License-Identifier: BSD-2-Clause-Patent
// Diting L12 DCS brightness. Register/packet facts are documented in
// README.backlight.md; this work-loop transport does not reset the display,
// reprogram the PLL, or lend a physical DMA buffer to the DSI controller.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include "DitingBacklightProtocol.h"
#include "DitingPanelMode.h"
#include "DitingDisplayText.h"
#include "DitingPlatformResources.h"
#include "../DitingProbe.h"

extern "C" bool diting_display_panel_rate(unsigned) __attribute__((weak_import));
extern "C" { DitingPanel::Diagnostic diting_panel_mode_diagnostic; }

extern kmod_info_t g_kernel_kmod_info;
extern "C" void DitingBacklightStaticInit(void);
class DitingBacklight;

namespace {
using namespace DitingBrightness;
DitingBacklight *gPublishedBacklight;
const OSMetaClass *gAppleBacklightClass;
uint32_t gPanelCopyReady;
#ifdef DITING_GUEST_24B156
// Standard SDR nits range for iOS 27 CoreBrightness (matches Sheng AppleARMBacklight.cpp).
constexpr uint32_t kMinNits = 2, kMidNits = 140, kMaxNits = 500;
volatile int32_t gBacklightLastNits;
volatile uint32_t gBacklightUnknownKeyLogs;
#endif
volatile uint32_t gBacklightDiagPublished, gBacklightDiagLevel, gBacklightDiagGoal;
volatile uint32_t gBacklightDiagRaw, gBacklightDiagRequests, gBacklightDiagWrites, gBacklightDiagReads;
volatile uint32_t gBacklightDiagStopped;
volatile uint32_t gBacklightDiagLastNz, gBacklightDiagFbPowered = 1, gBacklightDiagEnabled = 1;
volatile int32_t gBacklightDiagLastNzNits;
volatile uint32_t gBacklightDiagLowPower, gBacklightDiagRetries, gBacklightDiagWakeRestores;
volatile uint32_t gBacklightDiagEnableCalls, gBacklightDiagFbMsgs, gBacklightDiagMetaCalls;
volatile uint32_t gBacklightDiagErrAck, gBacklightDiagErrIrq;
char gBacklightDiagError[64];
char gBacklightDiagLastMeta[64];
volatile uint32_t *gEsdGpioRegs, *gRstGpioRegs;
constexpr uint64_t kSm8475TlmmBase = 0x0f100000ULL;
constexpr uint32_t kL12EsdErrGpio = 0x74u; // mi,esd-err-irq-gpio = <&tlmm 0x74 0x2008>
constexpr uint32_t kL12ResetGpio = 0x00u;  // qcom,platform-reset-gpio = <&tlmm 0x00 0x00>
uint64_t nowNs() {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns;
}
}

class DitingBacklight : public IOService {
    OSDeclareDefaultStructors(DitingBacklight)
    static constexpr uint32_t kMessageFramebufferState = 0xe0014001u;
    Board board = {};
    IOMemoryMap *mapping = nullptr;
    IOMemoryMap *esdGpioMap = nullptr;
    IOMemoryMap *rstGpioMap = nullptr;
    volatile uint32_t *regs = nullptr;
    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *timer = nullptr;
    IOTimerEventSource *requestTimer = nullptr;
    IOSimpleLock *policyLock = nullptr;
    const OSMetaClass *appleClass = nullptr;
    Command command;
    WakePolicy policy;
    DitingPanel::Mode panelMode;
    unsigned panelHz=0;
    bool panelReported=false;
    bool published = false, stopped = false;
    bool needVerification = false, queryPending = false, retryPending = false;
    bool lowPowerWrite = false, retriedWrite = false;
    uint64_t verifyAfter = 0;
    enum Operation { SetReadSize, ReadInitial, WriteLevel, ReadVerify } operation = ReadInitial;
    Operation pendingRead = ReadInitial;
    uint32_t level = 0, sending = 0, from = 0, goal = 0;
    uint64_t idleDeadline = 0, classDeadline = 0, fadeStart = 0, fadeDuration = 0;
    uint64_t request = 0;
    uint32_t requestedFade = 0, requests = 0, writes = 0, reads = 0, retries = 0;

    void syncPolicyDiagLocked() const {
        __atomic_store_n(&gBacklightDiagLastNz, policy.lastNonZeroLevel, __ATOMIC_RELAXED);
        __atomic_store_n(&gBacklightDiagLastNzNits, policy.lastNonZeroNits, __ATOMIC_RELAXED);
        __atomic_store_n(&gBacklightDiagFbPowered, policy.fbPowered ? 1u : 0u, __ATOMIC_RELAXED);
        __atomic_store_n(&gBacklightDiagEnabled, policy.enabled ? 1u : 0u, __ATOMIC_RELAXED);
        const uint32_t reqs = __atomic_load_n(&gBacklightDiagRequests, __ATOMIC_RELAXED);
        const uint32_t diagGoal = __atomic_load_n(&gBacklightDiagGoal, __ATOMIC_RELAXED);
        const bool displayOn = policy.fbPowered && policy.enabled && (reqs == 0 || diagGoal > 0);
        diting_power_set_display_on(displayOn ? 1 : 0);
    }
    uint32_t queueTargetLocked(uint32_t target, uint32_t fadeMs) {
        const uint32_t req = __atomic_add_fetch(&requests, 1u, __ATOMIC_RELAXED);
        __atomic_store_n(&gBacklightDiagRequests, req, __ATOMIC_RELAXED);
        __atomic_store_n(&gBacklightDiagGoal, target, __ATOMIC_RELAXED);
        __atomic_store_n(&request, (1ULL << 63) | ((uint64_t)target << 16) | (fadeMs & 0xffffu), __ATOMIC_RELEASE);
        syncPolicyDiagLocked();
        return req;
    }
    static bool parseBoolOrNumber(OSObject *obj, bool &out) {
        if (OSBoolean *b = OSDynamicCast(OSBoolean, obj)) { out = b->getValue(); return true; }
        if (OSNumber *n = OSDynamicCast(OSNumber, obj)) { out = n->unsigned64BitValue() != 0; return true; }
        return false;
    }

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
    static bool applyPanelTear(void *,unsigned hz) {
        return &diting_display_panel_rate && diting_display_panel_rate(hz);
    }
    void reportPanel() {
        const auto &d=panelMode.diagnostic();auto &snapshot=diting_panel_mode_diagnostic;
        __atomic_add_fetch(&snapshot.sequence,1,__ATOMIC_ACQ_REL);
        memcpy(reinterpret_cast<char *>(&snapshot)+12,reinterpret_cast<const char *>(&d)+12,sizeof(d)-12);
        __atomic_add_fetch(&snapshot.sequence,1,__ATOMIC_RELEASE);
        if(panelReported && panelMode.active() && !panelMode.retained())panelReported=false;
        if((!panelMode.active() || panelMode.retained()) && !panelReported) {
            panelReported=true;
            setProperty("DitingPanelModeStage",d.stage,32);
            setProperty("DitingPanelModeError",d.error,32);
            setProperty("DitingPanelModeReadback",d.after,32);
            setProperty("DitingPanelRequestedHz",d.targetHz,32);
            IOLog("DitingPanel: stage=%u error=%u panel=%u id=%02x/%02x/%02x target=%u before=%u after=%u batches=%u restored=%u query=%02x rx=%08x/%08x/%08x sizes=%u address=%02x pixel=%02x colour_error=%u; physical fps unqualified\n",
                d.stage,d.error,d.panel,d.id[0],d.id[1],d.id[2],d.targetHz,d.before,d.after,d.batches,d.restored,
                d.queryRegister,d.readControl,d.data0,d.data1,d.returnSizeCommands,d.addressMode,d.pixelFormat,d.colourError);
            DitingProbeRecord(8,"panel-mode",d.stage==6 && !d.error?DitingProbePass:DitingProbeFail,
                d.stage==6 && !d.error?"identified L12 timing and tear count accepted; physical fps unqualified":
                panelMode.retained()?"DSI command timeout; FIFO retained":"panel mode failed; bounded restoration recorded",d.endUs);
        }
    }
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
        if (valid) {
            setProperty("DitingBrightnessRawReadback", raw, 32);
            __atomic_store_n(&gBacklightDiagRaw, raw, __ATOMIC_RELAXED);
        }
        return valid && DitingBrightness::controlLevel(board, raw, answer);
    }
    bool publishLevel() {
        __atomic_store_n(&gBacklightDiagLevel, level, __ATOMIC_RELAXED);
        __atomic_store_n(&gBacklightDiagGoal, goal, __ATOMIC_RELAXED);
        OSDictionary *item = OSDictionary::withCapacity(3);
#ifdef DITING_GUEST_24B156
        OSDictionary *milliNits = OSDictionary::withCapacity(3);
        OSDictionary *parameters = OSDictionary::withCapacity(2);
        OSNumber *minMilli = OSNumber::withNumber(kMinNits * 1000ULL, 32);
        OSNumber *maxMilli = OSNumber::withNumber(kMaxNits * 1000ULL, 32);
        OSNumber *curMilli = OSNumber::withNumber(board.maximum ? (uint64_t)level * kMaxNits * 1000ULL / board.maximum : 0ULL, 32);
#else
        OSDictionary *parameters = OSDictionary::withCapacity(1);
#endif
        OSNumber *minimum = OSNumber::withNumber(0ULL, 32);
        OSNumber *maximum = OSNumber::withNumber(board.maximum, 32);
        OSNumber *value = OSNumber::withNumber(level, 32);
        bool ready = item && parameters && minimum && maximum && value &&
            item->setObject("min", minimum) && item->setObject("max", maximum) && item->setObject("value", value) &&
            parameters->setObject("brightness", item);
#ifdef DITING_GUEST_24B156
        ready = ready && milliNits && minMilli && maxMilli && curMilli &&
            milliNits->setObject("min", minMilli) && milliNits->setObject("max", maxMilli) &&
            milliNits->setObject("value", curMilli) &&
            parameters->setObject("BrightnessMilliNits", milliNits);
        OSSafeReleaseNULL(milliNits); OSSafeReleaseNULL(minMilli);
        OSSafeReleaseNULL(maxMilli); OSSafeReleaseNULL(curMilli);
#endif
        ready = ready && setProperty("IODisplayParameters", parameters);
        OSSafeReleaseNULL(item); OSSafeReleaseNULL(parameters);
        OSSafeReleaseNULL(minimum); OSSafeReleaseNULL(maximum); OSSafeReleaseNULL(value);
        return ready;
    }
#ifdef DITING_GUEST_24B156
    void publishCalibration() {
        const int32_t table[] = {(int32_t)(kMinNits << 16), (int32_t)(kMidNits << 16), (int32_t)(kMaxNits << 16)};
        if (OSData *marketing = OSData::withBytes(table, sizeof(table))) {
            setProperty("backlight-marketing-table", marketing);
            marketing->release();
        }
        if (OSData *lmax = OSData::withBytes(&table[2], sizeof(table[2]))) {
            setProperty("LmaxProduct", lmax);
            lmax->release();
        }
        const struct { const char *key; uint32_t nits; } products[] = {
            {"LminProduct", kMinNits}, {"LmidProduct", kMidNits}
        };
        for (const auto &p : products) {
            if (OSNumber *v = OSNumber::withNumber(p.nits, 32)) {
                setProperty(p.key, v);
                v->release();
            }
        }
        OSDictionary *calibration = OSDictionary::withCapacity(6);
        if (!calibration) return;
        const struct { const char *key; uint32_t nits; } fields[] = {
            {"lMinProduct", kMinNits}, {"lMidProduct", kMidNits}, {"lMaxProduct", kMaxNits},
            {"lMinPanel", kMinNits}, {"lMidPanel", kMidNits}, {"lMaxPanel", kMaxNits}
        };
        for (const auto &f : fields) {
            if (OSNumber *num = OSNumber::withNumber((uint64_t)f.nits << 16, 32)) {
                calibration->setObject(f.key, num);
                num->release();
            }
        }
        setProperty("backlight-calibration-parameters", calibration);
        calibration->release();
    }
    uint32_t levelForNits(int32_t fixed) const {
        if (fixed <= 0) return 0;
        const uint64_t scale = (uint64_t)kMaxNits << 16;
        const uint64_t scaled = ((uint64_t)fixed * board.maximum + scale / 2) / scale;
        if (scaled < board.minimum) return board.minimum;
        if (scaled > board.maximum) return board.maximum;
        return (uint32_t)scaled;
    }
#endif
    void reportError(const char *reason) {
        strlcpy(gBacklightDiagError, reason ? reason : "", sizeof(gBacklightDiagError));
        OSString *previous = OSDynamicCast(OSString, getProperty("DitingBacklightError"));
        if (previous && previous->isEqualTo(reason)) return;
        setProperty("DitingBacklightError", reason);
        IOLog("DitingBacklight: %s; requests=%u writes=%u reads=%u status=%x irq=%x ack=%x rx=%x data=%08x/%08x\n",
              reason, __atomic_load_n(&requests, __ATOMIC_RELAXED), writes, reads, rd(Status), rd(InterruptControl), rd(AckError),rd(ReadControl),rd(ReadData0),rd(ReadData1));
    }
    void fail(const char *reason) {
        __atomic_store_n(&stopped, true, __ATOMIC_RELEASE);
        __atomic_store_n(&gBacklightDiagStopped, 1u, __ATOMIC_RELEASE);
        timer->cancelTimeout(); requestTimer->cancelTimeout();
        reportError(reason);
        if (command.inFlight()) command.finish();
    }
    void abandonRetired(const char *reason) {
        // This failed packet has retired. Keep the last measured DBV and
        // permit a newer request; the mailbox has not been consumed yet.
        needVerification = queryPending = retryPending = false;
        goal = from = level; fadeDuration = 0; idleDeadline = 0;
        if (policyLock) {
            IOSimpleLockLock(policyLock);
            if (!(__atomic_load_n(&request, __ATOMIC_ACQUIRE) >> 63)) {
                policy.currentTarget = level;
                syncPolicyDiagLocked();
            }
            IOSimpleLockUnlock(policyLock);
        }
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
#ifdef DITING_GUEST_24B156
        publishCalibration();
#endif
        gAppleBacklightClass = appleClass;
        __atomic_store_n(&gPublishedBacklight, this, __ATOMIC_RELEASE);
        __atomic_store_n(&gBacklightDiagPublished, 1u, __ATOMIC_RELEASE);
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
        if(panelMode.active()) {
            const unsigned next=panelMode.step(nowNs());reportPanel();
            if(next)timer->setTimeoutMS(next);
            // A timeout retains the same command FIFO. A subsequent OS
            // request may reap a late completion but cannot submit over it.
            if(panelMode.active())return;
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
            if (completion == Completion::Error) {
                __atomic_store_n(&gBacklightDiagErrAck, rd(AckError), __ATOMIC_RELAXED);
                __atomic_store_n(&gBacklightDiagErrIrq, rd(InterruptControl), __ATOMIC_RELAXED);
                reportError("retired DSI command error");
            }
            if (!command.finish()) { timer->setTimeoutMS(1); return; }
            if (!readOk) {
                // The command has retired, so a malformed response does
                // not leave DMA ownership uncertain. Preserve the last
                // measured level and allow a later OS request to retry.
                // An initial read is still required before publication.
                if (!published) { fail("invalid initial DCS brightness response"); return; }
                const bool isWriteOrVerify = (operation == WriteLevel) || (operation == ReadVerify) ||
                    (operation == SetReadSize && pendingRead == ReadVerify);
                if (completion == Completion::Error &&
                    DitingBrightness::shouldRetryError(published, retriedWrite, sending, level, isWriteOrVerify)) {
                    // Retry a retired HS/LP command error once in the opposite
                    // signalling mode before abandoning the write.
                    retriedWrite = retryPending = true;
                    lowPowerWrite = !lowPowerWrite;
                    retries++;
                    __atomic_store_n(&gBacklightDiagLowPower, lowPowerWrite ? 1u : 0u, __ATOMIC_RELAXED);
                    __atomic_store_n(&gBacklightDiagRetries, retries, __ATOMIC_RELAXED);
                    needVerification = queryPending = false;
                    idleDeadline = 0;
                    IOLog("DitingBacklight: DSI command error op=%u sending=%u; retrying once with LP=%u\n",
                          (unsigned)operation, sending, lowPowerWrite ? 1u : 0u);
                    timer->setTimeoutMS(2);
                    return;
                }
                abandonRetired(completion == Completion::Error ?
                    "retired DSI command error" : "invalid DCS brightness response");
                return;
            }
            if (operation == SetReadSize) {
                queryPending = true;
            } else if (operation == WriteLevel) {
                writes++; needVerification = true;
                __atomic_store_n(&gBacklightDiagWrites, writes, __ATOMIC_RELAXED);
                // DMA completion retires the packet, not the panel's DBV
                // latch. Let at least two 120 Hz scans pass before GET.
                verifyAfter = nowNs() + 20000000ULL;
            } else {
                reads++;
                __atomic_store_n(&gBacklightDiagReads, reads, __ATOMIC_RELAXED);
                if (operation == ReadVerify && returned != sending) {
                    level = clamped(returned); publishLevel(); needVerification = false;
                    // Retry the same retired write once with the other
                    // command signalling mode after the first retired write.
                    // Never disable every later slider request for a value
                    // the panel rejected. Uncertain DMA retains ownership.
                    if (!retriedWrite) {
                        retriedWrite = retryPending = true; lowPowerWrite = !lowPowerWrite;
                        retries++;
                        __atomic_store_n(&gBacklightDiagLowPower, lowPowerWrite ? 1u : 0u, __ATOMIC_RELAXED);
                        __atomic_store_n(&gBacklightDiagRetries, retries, __ATOMIC_RELAXED);
                    } else {
                        setProperty("DitingBacklightError", "DBV not accepted");
                        IOLog("DitingBacklight: DBV rejected wanted=%u measured=%u LP=%u; later requests remain available\n",
                              sending, returned, lowPowerWrite);
                        from = goal = level; fadeDuration = 0;
                    }
                } else {
                    level = clamped(returned);
                    removeProperty("DitingBacklightError");
                    gBacklightDiagError[0] = 0;
                    retriedWrite = retryPending = false;
                }
                if (!published) {
                    from = goal = level;
                    if (policyLock) {
                        IOSimpleLockLock(policyLock);
                        if (!policy.initialized) {
                            int32_t initNits = 0;
#ifdef DITING_GUEST_24B156
                            initNits = board.maximum ?
                                (int32_t)(((uint64_t)level * ((uint64_t)kMaxNits << 16)) / board.maximum) : 0;
#endif
                            policy.onInitialLevel(board, level, initNits);
                            syncPolicyDiagLocked();
                        }
                        IOSimpleLockUnlock(policyLock);
                    }
                    if (!publishFirst()) return;
                } else publishLevel();
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
        if(panelHz && !panelMode.started() && __atomic_load_n(&gPanelCopyReady,__ATOMIC_ACQUIRE)) {
            if(panelMode.start(panelHz,nowNs())) {
                DitingProbeRecord(8,"panel-mode",DitingProbeRunning,"identify DDIC after completed scanout copy");
                reportPanel();timer->setTimeoutMS(1);return;
            }
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
    void requestPanel() {if(requestTimer)requestTimer->setTimeoutUS(1);}
    bool prepare() {
        IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
        OSData *data = chosen ? OSDynamicCast(OSData, chosen->getProperty("diting-dsi-backlight")) : nullptr;
        if (data && data->getLength() == sizeof(board)) memcpy(&board, data->getBytesNoCopy(), sizeof(board));
        DitingPanel::Selection selection{};
        OSData *panel = chosen ? OSDynamicCast(OSData, chosen->getProperty("diting-dsi-panel")) : nullptr;
        if(panel && panel->getLength()==sizeof(selection))memcpy(&selection,panel->getBytesNoCopy(),sizeof(selection));
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
        const Io io={this,readRegister,writeRegister,delay};
        command.initialize(io);
        unsigned hz=0;PE_parse_boot_argn("diting_panel_hz",&hz,sizeof(hz));
        if((hz==60 || hz==90 || hz==120) && &diting_display_panel_rate) {
            panelHz=hz;panelMode.initialize(command,io,{this,applyPanelTear},selection);
        }
        if (!rd(Version) || rd(Version) == ~0u || !(rd(Control) & 1u) || (rd(PatternControl) & 1u)) {
            IOLog("DitingBacklight: DSI0 unavailable version=%x control=%x pattern=%x\n",rd(Version),rd(Control),rd(PatternControl));
            return false;
        }
        loop = IOWorkLoop::workLoop();
        timer = IOTimerEventSource::timerEventSource(this, tick);
        requestTimer = IOTimerEventSource::timerEventSource(this, tick);
        policyLock = IOSimpleLockAlloc();
        if (!loop || !timer || !requestTimer || !policyLock ||
            loop->addEventSource(timer) || loop->addEventSource(requestTimer)) return false;
        if (IODeviceMemory *esdMem = IODeviceMemory::withRange(kSm8475TlmmBase + kL12EsdErrGpio * 0x1000ULL, 0x1000)) {
            esdGpioMap = esdMem->map(kIOMapAnywhere | kIOMapInhibitCache);
            esdMem->release();
            if (esdGpioMap) {
                __atomic_store_n(&gEsdGpioRegs,
                    reinterpret_cast<volatile uint32_t *>(esdGpioMap->getVirtualAddress()), __ATOMIC_RELEASE);
            }
        }
        if (IODeviceMemory *rstMem = IODeviceMemory::withRange(kSm8475TlmmBase + kL12ResetGpio * 0x1000ULL, 0x1000)) {
            rstGpioMap = rstMem->map(kIOMapAnywhere | kIOMapInhibitCache);
            rstMem->release();
            if (rstGpioMap) {
                __atomic_store_n(&gRstGpioRegs,
                    reinterpret_cast<volatile uint32_t *>(rstGpioMap->getVirtualAddress()), __ATOMIC_RELEASE);
            }
        }
        setProperty("DitingPanel", "Diting L12 AMOLED / SM8475 DSI0");
        setProperty("DitingDSIVersion", rd(Version), 32);
        classDeadline = nowNs() + 30000000000ULL;
        timer->setTimeoutMS(1);
        return true;
    }
    IOReturn message(UInt32 type, IOService *provider, void *argument) override {
        if (type == kMessageFramebufferState) {
            if (__atomic_load_n(&stopped, __ATOMIC_ACQUIRE) || !policyLock) return kIOReturnNotReady;
            const bool powered = (reinterpret_cast<uintptr_t>(argument) != 0);
            const uint32_t msgCount = __atomic_add_fetch(&gBacklightDiagFbMsgs, 1u, __ATOMIC_RELAXED);
            IOSimpleLockLock(policyLock);
            const WakeAction act = policy.onFramebufferState(board, powered);
            uint32_t req = 0;
            if (act.submit) {
#ifdef DITING_GUEST_24B156
                if (act.restored) {
                    const int32_t restoredNits = policy.lastNonZeroNits > 0 ? policy.lastNonZeroNits :
                        (board.maximum ? (int32_t)(((uint64_t)act.target * ((uint64_t)kMaxNits << 16)) / board.maximum) : 0);
                    __atomic_store_n(&gBacklightLastNits, restoredNits, __ATOMIC_RELAXED);
                } else if (act.target == 0) {
                    __atomic_store_n(&gBacklightLastNits, 0, __ATOMIC_RELAXED);
                }
#endif
                req = queueTargetLocked(act.target, 0);
            } else {
                syncPolicyDiagLocked();
            }
            IOSimpleLockUnlock(policyLock);
            if (act.restored) __atomic_add_fetch(&gBacklightDiagWakeRestores, 1u, __ATOMIC_RELAXED);
            if (act.submit) requestTimer->setTimeoutUS(1);
            if (msgCount <= 16 || act.restored) {
                IOLog("DitingBacklight: fbState -> %u submit=%u restored=%u target=%u req#%u\n",
                      powered ? 1u : 0u, act.submit ? 1u : 0u, act.restored ? 1u : 0u, act.target, req);
            }
            return kIOReturnSuccess;
        }
        return IOService::message(type, provider, argument);
    }
    IOReturn setProperties(OSObject *object) override {
        OSDictionary *properties = OSDynamicCast(OSDictionary, object);
        if (!properties) return kIOReturnBadArgument;
        if (__atomic_load_n(&stopped, __ATOMIC_ACQUIRE) || !policyLock) return kIOReturnNotReady;
#ifdef DITING_GUEST_24B156
        // iOS 27 CoreBrightness sends signed 16.16 nits in "brightness-nits" /
        // "dcp-brightness-nits", enable controls in "EnableBacklight" /
        // "dcp-backlight-enable", plus brightness/FadePeriod compatibility.
        bool handled = false;
        if (OSNumber *fade = OSDynamicCast(OSNumber, properties->getObject("FadePeriod"))) {
            __atomic_store_n(&requestedFade,
                DitingBrightness::fadeMilliseconds(fade->unsigned64BitValue(), fade->numberOfBits()),
                __ATOMIC_RELAXED);
            handled = true;
        }
        OSNumber *nits = OSDynamicCast(OSNumber, properties->getObject("brightness-nits"));
        if (!nits) nits = OSDynamicCast(OSNumber, properties->getObject("dcp-brightness-nits"));
        OSNumber *brightness = OSDynamicCast(OSNumber, properties->getObject("brightness"));
        const bool hasExplicitBrightness = (nits != nullptr) || (brightness != nullptr);

        bool dcpEnable = false;
        if (parseBoolOrNumber(properties->getObject("dcp-backlight-enable"), dcpEnable)) {
            const uint32_t enCalls = __atomic_add_fetch(&gBacklightDiagEnableCalls, 1u, __ATOMIC_RELAXED);
            IOSimpleLockLock(policyLock);
            const WakeAction act = policy.onFramebufferState(board, dcpEnable);
            uint32_t req = 0;
            const bool submit = !hasExplicitBrightness && act.submit;
            if (submit) {
                if (act.restored) {
                    const int32_t restoredNits = policy.lastNonZeroNits > 0 ? policy.lastNonZeroNits :
                        (board.maximum ? (int32_t)(((uint64_t)act.target * ((uint64_t)kMaxNits << 16)) / board.maximum) : 0);
                    __atomic_store_n(&gBacklightLastNits, restoredNits, __ATOMIC_RELAXED);
                } else if (act.target == 0) {
                    __atomic_store_n(&gBacklightLastNits, 0, __ATOMIC_RELAXED);
                }
                req = queueTargetLocked(act.target, 0);
            } else {
                syncPolicyDiagLocked();
            }
            IOSimpleLockUnlock(policyLock);
            if (submit && act.restored) __atomic_add_fetch(&gBacklightDiagWakeRestores, 1u, __ATOMIC_RELAXED);
            if (submit) requestTimer->setTimeoutUS(1);
            if (enCalls <= 16 || (submit && act.restored)) {
                IOLog("DitingBacklight: dcp-backlight-enable=%u submit=%u restored=%u target=%u req#%u\n",
                      dcpEnable ? 1u : 0u, submit ? 1u : 0u, act.restored ? 1u : 0u, act.target, req);
            }
            handled = true;
        }
        bool enableFlag = false;
        if (parseBoolOrNumber(properties->getObject("EnableBacklight"), enableFlag)) {
            const uint32_t enCalls = __atomic_add_fetch(&gBacklightDiagEnableCalls, 1u, __ATOMIC_RELAXED);
            IOSimpleLockLock(policyLock);
            const WakeAction act = policy.onEnableRequest(board, enableFlag);
            uint32_t req = 0;
            const bool submit = !hasExplicitBrightness && act.submit;
            if (submit) {
                if (act.restored) {
                    const int32_t restoredNits = policy.lastNonZeroNits > 0 ? policy.lastNonZeroNits :
                        (board.maximum ? (int32_t)(((uint64_t)act.target * ((uint64_t)kMaxNits << 16)) / board.maximum) : 0);
                    __atomic_store_n(&gBacklightLastNits, restoredNits, __ATOMIC_RELAXED);
                } else if (act.target == 0) {
                    __atomic_store_n(&gBacklightLastNits, 0, __ATOMIC_RELAXED);
                }
                req = queueTargetLocked(act.target, 0);
            } else {
                syncPolicyDiagLocked();
            }
            IOSimpleLockUnlock(policyLock);
            if (submit && act.restored) __atomic_add_fetch(&gBacklightDiagWakeRestores, 1u, __ATOMIC_RELAXED);
            if (submit) requestTimer->setTimeoutUS(1);
            if (enCalls <= 16 || (submit && act.restored)) {
                IOLog("DitingBacklight: EnableBacklight=%u submit=%u restored=%u target=%u req#%u\n",
                      enableFlag ? 1u : 0u, submit ? 1u : 0u, act.restored ? 1u : 0u, act.target, req);
            }
            handled = true;
        }
        if (nits) {
            const int32_t fixed = static_cast<int32_t>(nits->unsigned32BitValue());
            const uint32_t target = levelForNits(fixed);
            IOSimpleLockLock(policyLock);
            const WakeAction act = policy.onBrightnessRequest(board, target, fixed, true);
            __atomic_store_n(&gBacklightLastNits, fixed, __ATOMIC_RELAXED);
            const uint32_t req = queueTargetLocked(act.target, 0);
            IOSimpleLockUnlock(policyLock);
            if (req <= 12) IOLog("DitingBacklight: req#%u brightness-nits=0x%x (%d nits) -> level=%u\n", req, (unsigned)fixed, fixed >> 16, act.target);
            requestTimer->setTimeoutUS(1);
            return kIOReturnSuccess;
        }
        if (brightness) {
            const uint32_t ms = __atomic_load_n(&requestedFade, __ATOMIC_RELAXED);
            const uint32_t target = DitingBrightness::requestedLevel(board, brightness->unsigned64BitValue(), brightness->numberOfBits());
            IOSimpleLockLock(policyLock);
            const WakeAction act = policy.onBrightnessRequest(board, target);
            queueTargetLocked(act.target, ms);
            IOSimpleLockUnlock(policyLock);
            requestTimer->setTimeoutUS(1);
            return kIOReturnSuccess;
        }
        if (handled) return kIOReturnSuccess;
        const uint32_t metaIdx = __atomic_fetch_add(&gBacklightUnknownKeyLogs, 1u, __ATOMIC_RELAXED);
        __atomic_store_n(&gBacklightDiagMetaCalls, metaIdx + 1u, __ATOMIC_RELAXED);
        OSCollectionIterator *it = OSCollectionIterator::withCollection(properties);
        if (it) {
            bool recordedFirst = false;
            while (const OSSymbol *key = OSDynamicCast(OSSymbol, it->getNextObject())) {
                const char *kname = key->getCStringNoCopy();
                OSObject *val = properties->getObject(key);
                char summary[48];
                if (OSNumber *num = OSDynamicCast(OSNumber, val)) {
                    snprintf(summary, sizeof(summary), "num(0x%llx)", (unsigned long long)num->unsigned64BitValue());
                } else if (OSBoolean *b = OSDynamicCast(OSBoolean, val)) {
                    snprintf(summary, sizeof(summary), "bool(%u)", b->getValue() ? 1u : 0u);
                } else if (OSDictionary *dict = OSDynamicCast(OSDictionary, val)) {
                    snprintf(summary, sizeof(summary), "dict(%u)", dict->getCount());
                } else if (OSData *d = OSDynamicCast(OSData, val)) {
                    snprintf(summary, sizeof(summary), "data(%u)", d->getLength());
                } else {
                    snprintf(summary, sizeof(summary), "other");
                }
                if (!recordedFirst && kname) {
                    recordedFirst = true;
                    snprintf(gBacklightDiagLastMeta, sizeof(gBacklightDiagLastMeta), "%s:%s", kname, summary);
                }
                if (metaIdx < 16 && kname) {
                    if (OSDictionary *dict = OSDynamicCast(OSDictionary, val)) {
                        char subkeys[96] = {};
                        size_t pos = 0;
                        unsigned subCount = 0;
                        if (OSCollectionIterator *sub = OSCollectionIterator::withCollection(dict)) {
                            while (const OSSymbol *sk = OSDynamicCast(OSSymbol, sub->getNextObject())) {
                                if (subCount >= 6 || pos + 4 >= sizeof(subkeys)) break;
                                const char *sn = sk->getCStringNoCopy();
                                if (!sn) continue;
                                int w = snprintf(subkeys + pos, sizeof(subkeys) - pos, "%s%s", subCount ? "," : "", sn);
                                if (w > 0) pos += (size_t)w < sizeof(subkeys) - pos ? (size_t)w : sizeof(subkeys) - pos - 1;
                                subCount++;
                            }
                            sub->release();
                        }
                        IOLog("DitingBacklight: meta#%u key=%s %s [%s]\n", metaIdx + 1u, kname, summary, subkeys);
                    } else {
                        IOLog("DitingBacklight: meta#%u key=%s %s\n", metaIdx + 1u, kname, summary);
                    }
                }
            }
            it->release();
        }
        return kIOReturnSuccess;
#else
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
        IOSimpleLockLock(policyLock);
        const WakeAction act = policy.onBrightnessRequest(board, target);
        queueTargetLocked(act.target, ms);
        IOSimpleLockUnlock(policyLock);
        // A separate event source prevents the transaction/fade timer from
        // overwriting a concurrently armed slider wakeup.
        requestTimer->setTimeoutUS(1); // caller never waits on DSI or the work-loop gate
        return kIOReturnSuccess;
#endif
    }
    void free() override {
        if (__atomic_load_n(&gPublishedBacklight, __ATOMIC_ACQUIRE) == this)
            __atomic_store_n(&gPublishedBacklight, (DitingBacklight *)nullptr, __ATOMIC_RELEASE);
        __atomic_store_n(&gEsdGpioRegs, (volatile uint32_t *)nullptr, __ATOMIC_RELEASE);
        __atomic_store_n(&gRstGpioRegs, (volatile uint32_t *)nullptr, __ATOMIC_RELEASE);
        if (timer) { timer->cancelTimeout(); if (loop) loop->removeEventSource(timer); }
        if (requestTimer) { requestTimer->cancelTimeout(); if (loop) loop->removeEventSource(requestTimer); }
        if (policyLock) { IOSimpleLockFree(policyLock); policyLock = nullptr; }
        OSSafeReleaseNULL(timer); OSSafeReleaseNULL(requestTimer); OSSafeReleaseNULL(loop);
        OSSafeReleaseNULL(esdGpioMap); OSSafeReleaseNULL(rstGpioMap); OSSafeReleaseNULL(mapping);
        IOService::free();
    }
};

extern "C" void diting_backlight_display_power(bool on) {
    DitingBacklight *backlight = __atomic_load_n(&gPublishedBacklight, __ATOMIC_ACQUIRE);
    if (!backlight) return;
    backlight->message(0xe0014001u, backlight->getProvider(),
                       reinterpret_cast<void *>(static_cast<uintptr_t>(on ? 1u : 0u)));
}

extern "C" void diting_panel_copy_ready(void) {
    __atomic_store_n(&gPanelCopyReady,1,__ATOMIC_RELEASE);
    auto *backlight=__atomic_load_n(&gPublishedBacklight,__ATOMIC_ACQUIRE);
    if(backlight)backlight->requestPanel();
}
extern "C" unsigned diting_panel_snapshot_text(char *out,unsigned bytes) {
    if(!out || !bytes)return 0;
    DitingPanel::Diagnostic d{};bool stable=false;
    for(unsigned attempt=0;attempt<3;++attempt) {
        const auto before=__atomic_load_n(&diting_panel_mode_diagnostic.sequence,__ATOMIC_ACQUIRE);
        if(before&1)continue;
        memcpy(&d,&diting_panel_mode_diagnostic,sizeof(d));
        const auto after=__atomic_load_n(&diting_panel_mode_diagnostic.sequence,__ATOMIC_ACQUIRE);
        if(before==after) {stable=true;break;}
    }
    const char *status=!stable?"CHANGED":d.stage==6 && !d.error?"PASS":!d.stage?"WAIT":d.stage==8?"FAIL":d.stage==9?"RETAINED":"RUNNING";
    DitingDisplay::Text t{out,bytes};
    t.add("PANEL schema=3 status=%s stage=%u error=%u panel=%u id=%02x/%02x/%02x target_hz=%u rollback_hz=%u before=%u after=%u batches=%u restoring=%u restored=%u start_us=%llu end_us=%llu return_sizes=%u address_mode=%02x pixel_format=%02x colour_error=%u physical_fps=UNQUALIFIED\n",
        status,d.stage,d.error,d.panel,d.id[0],d.id[1],d.id[2],d.targetHz,d.rollbackHz,d.before,d.after,d.batches,d.restoring,d.restored,
        (unsigned long long)d.startUs,(unsigned long long)d.endUs,d.returnSizeCommands,d.addressMode,d.pixelFormat,d.colourError);
    t.add("PANEL_SELECTION model=%08x/%08x reason=%u basis=retained-live-DT-and-DCS-guard\n",d.modelHigh,d.modelLow,d.selectionReason);
    t.add("PANEL_COLOUR status=%u address_before=%02x address_after=%02x writes=%u restored=%u error=%u physical_colour=UNQUALIFIED\n",
        d.colourStatus,d.addressMode,d.addressAfter,d.colourWrites,d.colourRestored,d.colourError);
    for(const auto &r:d.responses)if(r[0])t.add("PANEL_REPLY reg=%02x control=%08x data0=%08x data1=%08x\n",r[0],r[1],r[2],r[3]);
    return t.used;
}
extern "C" unsigned diting_backlight_snapshot_text(char *out,unsigned bytes) {
    if(!out || !bytes)return 0;
    DitingDisplay::Text t{out,bytes};
#ifdef DITING_GUEST_24B156
    const int32_t lastNits = __atomic_load_n(&gBacklightLastNits, __ATOMIC_RELAXED);
#else
    const int32_t lastNits = 0;
#endif
    t.add("BACKLIGHT published=%u level=%u goal=%u raw=%u requests=%u writes=%u reads=%u stopped=%u last_nits_q16=%d err=%s\n",
        __atomic_load_n(&gBacklightDiagPublished, __ATOMIC_ACQUIRE),
        __atomic_load_n(&gBacklightDiagLevel, __ATOMIC_RELAXED),
        __atomic_load_n(&gBacklightDiagGoal, __ATOMIC_RELAXED),
        __atomic_load_n(&gBacklightDiagRaw, __ATOMIC_RELAXED),
        __atomic_load_n(&gBacklightDiagRequests, __ATOMIC_RELAXED),
        __atomic_load_n(&gBacklightDiagWrites, __ATOMIC_RELAXED),
        __atomic_load_n(&gBacklightDiagReads, __ATOMIC_RELAXED),
        __atomic_load_n(&gBacklightDiagStopped, __ATOMIC_ACQUIRE),
        lastNits, gBacklightDiagError[0] ? gBacklightDiagError : "none");
    uint32_t esdCfg = 0, esdIo = 0, rstCfg = 0, rstIo = 0;
    if (volatile uint32_t *esd = __atomic_load_n(&gEsdGpioRegs, __ATOMIC_ACQUIRE)) {
        esdCfg = esd[0]; esdIo = esd[1];
        __asm__ volatile("dmb oshld" ::: "memory");
    }
    if (volatile uint32_t *rst = __atomic_load_n(&gRstGpioRegs, __ATOMIC_ACQUIRE)) {
        rstCfg = rst[0]; rstIo = rst[1];
        __asm__ volatile("dmb oshld" ::: "memory");
    }
    t.add("BACKLIGHT_DIAG fb=%u enabled=%u last_nz=%u last_nz_nits_q16=%d lp=%u retries=%u wake_restores=%u en_calls=%u fb_msgs=%u meta_calls=%u last_meta=%s err_ack=%08x err_irq=%08x esd_gpio=%08x/%08x rst_gpio=%08x/%08x\n",
        __atomic_load_n(&gBacklightDiagFbPowered, __ATOMIC_RELAXED),
        __atomic_load_n(&gBacklightDiagEnabled, __ATOMIC_RELAXED),
        __atomic_load_n(&gBacklightDiagLastNz, __ATOMIC_RELAXED),
        __atomic_load_n(&gBacklightDiagLastNzNits, __ATOMIC_RELAXED),
        __atomic_load_n(&gBacklightDiagLowPower, __ATOMIC_RELAXED),
        __atomic_load_n(&gBacklightDiagRetries, __ATOMIC_RELAXED),
        __atomic_load_n(&gBacklightDiagWakeRestores, __ATOMIC_RELAXED),
        __atomic_load_n(&gBacklightDiagEnableCalls, __ATOMIC_RELAXED),
        __atomic_load_n(&gBacklightDiagFbMsgs, __ATOMIC_RELAXED),
        __atomic_load_n(&gBacklightDiagMetaCalls, __ATOMIC_RELAXED),
        gBacklightDiagLastMeta[0] ? gBacklightDiagLastMeta : "none",
        __atomic_load_n(&gBacklightDiagErrAck, __ATOMIC_RELAXED),
        __atomic_load_n(&gBacklightDiagErrIrq, __ATOMIC_RELAXED),
        esdCfg, esdIo, rstCfg, rstIo);
    return t.used;
}

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

extern "C" void diting_display_timing_start(IOService *display);
extern "C" void diting_backlight_start(IOService *display) {
    diting_display_timing_start(display);
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
