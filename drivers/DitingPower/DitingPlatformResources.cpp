/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Shared Diting APSS resources. GPU, WLAN and audio serialize complete
 * active-only requests; the DT's sleep/wake TCSes remain firmware-owned. */
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/clock.h>
#include "DitingPlatformResources.h"
#include "DitingRpmhTransaction.h"
#include "DitingQmpTransaction.h"

extern "C" void diting_ramoops_diagnostic_write(const char *, int);

namespace {
IOLock *powerLock, *smmuLeaseLock, *scmCallLock, *aopLock;
IOMemoryMap *rscMap;
IOMemoryMap *aopMap, *aopDoorbellMap;
volatile uint8_t *rsc;
volatile uint8_t *aop, *aopDoorbell;
DitingRpmh::Controller controller;
DitingQmp::Controller qmpController;
unsigned traceCount;
unsigned qmpTraceCount;

void initLock(IOLock **slot) {
    if (__atomic_load_n(slot, __ATOMIC_ACQUIRE)) return;
    IOLock *lock = IOLockAlloc(), *empty = nullptr;
    if (lock && !__atomic_compare_exchange_n(slot, &empty, lock, false,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) IOLockFree(lock);
}
uint64_t now() {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000;
}
uint32_t rd(unsigned off) {
    uint32_t value = *reinterpret_cast<volatile uint32_t *>(rsc + off);
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}
void wr(unsigned off, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(rsc + off) = value;
    __asm__ volatile("dsb oshst" ::: "memory");
}
bool mapped() {
    if (rsc) return true;
    auto memory = IODeviceMemory::withRange(0x17a20000, 0x10000);
    if (!memory) return false;
    rscMap = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
    memory->release();
    if (rscMap) rsc = reinterpret_cast<volatile uint8_t *>(rscMap->getVirtualAddress());
    if (rsc) {
        char line[160];
        int n = snprintf(line, sizeof(line),
            "DITING RPMH: APSS DRV2 active=0..2 sleep=3..4 wake=5..6 id=%x caps=%x\n", rd(0), rd(0xc));
        if (n > 0) diting_ramoops_diagnostic_write(line, n < int(sizeof(line)) ? n : int(sizeof(line)) - 1);
    }
    return rsc != nullptr;
}
void recordRaw(void *, const char *phase, unsigned tcs, uint32_t address, uint32_t value,
               uint32_t first, uint32_t second, uint32_t control, uint32_t idle, uint32_t irq) {
    if (traceCount++ >= 192) return;
    char line[256];
    int n = snprintf(line, sizeof(line),
        "DITING RPMH: %s tcs=%u address=%x value=%x status=%x/%x control=%x idle=%x irq=%x\n",
        phase, tcs, address, value, first, second, control, idle, irq);
    // Save before any potentially blocking console operation. This path
    // allocates nothing and never takes another owner/registry/console lock.
    if (n > 0) diting_ramoops_diagnostic_write(line, n < int(sizeof(line)) ? n : int(sizeof(line)) - 1);
}
uint32_t readRaw(void *, unsigned off) { return rd(off); }
void writeRaw(void *, unsigned off, uint32_t value) { wr(off, value); }
uint64_t nowRaw(void *) { return now(); }
void delayRaw(void *, unsigned us) { IODelay(us); }
volatile uint8_t *mapAopWindow(IOMemoryMap *&map, uint64_t base) {
    if (!map) {
        auto memory = IODeviceMemory::withRange(base, 0x1000);
        if (!memory) return nullptr;
        map = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
        memory->release();
    }
    return map ? reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress()) : nullptr;
}
uint32_t qmpRead(void *, unsigned off) {
    const uint32_t value = *reinterpret_cast<volatile uint32_t *>(aop + off);
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}
void qmpWrite(void *, unsigned off, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(aop + off) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
void qmpKick(void *) {
    // Diting DT: IPCC client AOP=0, GLINK/QMP signal=0, SEND_ID=0x0c.
    *reinterpret_cast<volatile uint32_t *>(aopDoorbell + 0x0c) = 0;
    __asm__ volatile("dsb sy" ::: "memory");
}
void qmpDelay(void *, unsigned us) { IOSleep((us + 999) / 1000); }
void qmpRecord(void *, const char *phase, uint32_t box, uint32_t value) {
    if (__atomic_fetch_add(&qmpTraceCount, 1, __ATOMIC_RELAXED) >= 96) return;
    char line[192];
    const int n = snprintf(line, sizeof(line), "DITING AOP: %s box=%x value=%x\n", phase, box, value);
    if (n > 0) diting_ramoops_diagnostic_write(line, n < int(sizeof(line)) ? n : int(sizeof(line)) - 1);
}
}

extern "C" void diting_rpmh_init(void) {
    initLock(&powerLock); initLock(&smmuLeaseLock);
}
extern "C" bool diting_rpmh_vote(uint32_t address, uint32_t value) {
    return diting_rpmh_vote_many(&address, &value, 1);
}
extern "C" bool diting_rpmh_vote_many(const uint32_t *addresses, const uint32_t *values, unsigned count) {
    diting_rpmh_init();
    if (!powerLock) return false;
    IOLockLock(powerLock);
    DitingRpmh::Result result = DitingRpmh::Result::Layout;
    if (mapped()) {
        const DitingRpmh::Io io = {nullptr, readRaw, writeRaw, nowRaw, delayRaw, recordRaw};
        result = controller.vote(io, addresses, values, count);
    }
    IOLockUnlock(powerLock);
    if (result != DitingRpmh::Result::Ok) {
        char line[192]; int n = snprintf(line, sizeof(line), "DITING RPMH: FAIL %s address=%x count=%u\n",
            DitingRpmh::reason(result), addresses && count ? addresses[0] : 0, count);
        if (n > 0) { diting_ramoops_diagnostic_write(line, n < int(sizeof(line)) ? n : int(sizeof(line)) - 1); IOLog("%s", line); }
    }
    return result == DitingRpmh::Result::Ok;
}
extern "C" bool diting_aop_send(const char *message, unsigned wireBytes) {
    initLock(&aopLock);
    if (!aopLock) return false;
    IOLockLock(aopLock);
    if (!aop) aop = mapAopWindow(aopMap, 0xc300000);
    if (!aopDoorbell) aopDoorbell = mapAopWindow(aopDoorbellMap, 0xed18000);
    DitingQmp::Result result = DitingQmp::Result::Descriptor;
    if (aop && aopDoorbell) {
        const DitingQmp::Io io = {nullptr, qmpRead, qmpWrite, qmpKick, nowRaw, qmpDelay, qmpRecord};
        result = qmpController.send(io, message, wireBytes);
    }
    IOLockUnlock(aopLock);
    if (result != DitingQmp::Result::Ok) qmpRecord(nullptr, DitingQmp::reason(result), 0, unsigned(result));
    return result == DitingQmp::Result::Ok;
}
extern "C" bool diting_smmu_lease_lock(void) {
    diting_rpmh_init();
    if (!smmuLeaseLock) return false;
    IOLockLock(smmuLeaseLock);
    return true;
}
extern "C" void diting_smmu_lease_unlock(void) {
    if (smmuLeaseLock) IOLockUnlock(smmuLeaseLock);
}

/* A suspended/interrupted secure transaction owns its session across
 * resumes. Serialize the complete GPU/ADSP call, never individual SMCs. */
extern "C" bool diting_scm_call_lock(void) {
    initLock(&scmCallLock);if(!scmCallLock)return false;
    IOLockLock(scmCallLock);return true;
}
extern "C" void diting_scm_call_unlock(void) {
    if(scmCallLock)IOLockUnlock(scmCallLock);
}
