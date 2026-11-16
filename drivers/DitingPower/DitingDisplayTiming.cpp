/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* SM8475 display diagnostics and the completed-copy autorefresh handover.
 * Hardware counters never certify a new composed frame or physical fps. */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>
#include <kern/thread_call.h>
#include "DitingDisplayText.h"
#include "DitingDisplayPresentation.h"
#include "DitingDisplayRefresh.h"
#include "DitingDisplayColour.h"
#include "DitingDisplaySolid.h"
#include "DitingPanelTear.h"
#include "../DitingProbe.h"
#define NATIVE_GPU_TRACE_OPTIONAL
#include "../DitingAdreno/submit/NativeGpuTrace.h"
extern "C" void diting_panel_copy_ready(void) __attribute__((weak_import));

namespace {
using namespace DitingDisplay;
static Board board;
static IOMemoryMap *maps[3];
static volatile uint32_t *registers[3];
static IOLock *displayGate;
static thread_call_t solidCall;
static SolidState solidState;
static uint32_t initialized;
static uint32_t copySnapshotAttempted;
static uint64_t uptimeUs() {
    uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000;
}
static bool read(void *, Region region, uint32_t offset, uint32_t *value) {
    if (unsigned(region) > Dsi || !registers[region]) return false;
    *value = registers[region][offset / 4];
    __asm__ volatile("dmb oshld" ::: "memory"); return true;
}
static bool write(void *,Region region,uint32_t offset,uint32_t value) {
    if(region!=Mdp || !registers[Mdp] ||
       (offset!=0x36288 && offset!=0x362b4 && offset!=0x36290 &&
        offset!=0x1418 && offset!=0x1348))return false;
    registers[Mdp][offset/4]=value;
    __asm__ volatile("dsb oshst" ::: "memory");return true;
}
static bool solidWrite(void *,Region region,uint32_t offset,uint32_t value) {
    if(region!=Mdp || !registers[Mdp] ||
       (offset!=0x5030 && offset!=0x5034 && offset!=0x503c &&
        offset!=0x7030 && offset!=0x7034 && offset!=0x703c && offset!=0x16018))return false;
    registers[Mdp][offset/4]=value;
    __asm__ volatile("dsb oshst" ::: "memory");return true;
}
static void pause(void *,unsigned us) {IODelay(us);}

}

extern "C" {
DitingDisplay::CopyDiagnostic diting_display_copy_diagnostic;
DitingDisplay::RefreshDiagnostic diting_display_refresh_diagnostic;
DitingDisplay::PanelTearDiagnostic diting_panel_tear_diagnostic;
DitingDisplay::SolidDiagnostic diting_display_solid_diagnostic;
}

namespace {
static SolidFill solidController() {
    return SolidFill({{nullptr,read},solidWrite},solidState,diting_display_solid_diagnostic);
}
static void solidFired(thread_call_param_t,thread_call_param_t) {
    if(__atomic_load_n(&initialized,__ATOMIC_ACQUIRE)!=2)return;
    IOLockLock(displayGate);
    auto &d=diting_display_solid_diagnostic;
    const auto before=d.stage;
    __atomic_add_fetch(&d.sequence,1,__ATOMIC_ACQ_REL);
    auto controller=solidController();
    const uint64_t now=uptimeUs();controller.step(board,now);
    if(controller.active()) {
        unsigned delay=2000;
        if(d.stage==SolidVisible)delay=unsigned(d.expiryUs>now?d.expiryUs-now:1);
        if(delay>100000)delay=100000;
        if(d.stage==SolidPowerWait)delay=20000;
        uint64_t deadline=0;clock_interval_to_deadline(delay,NSEC_PER_USEC,&deadline);
        // The driver SDK exports the ordinary delayed-call API. Critical
        // refresh notification timing is implemented inside XNU separately.
        thread_call_enter_delayed(solidCall,deadline);
    }
    __atomic_add_fetch(&d.sequence,1,__ATOMIC_RELEASE);
    const auto after=d.stage,error=d.error,restored=d.restored;
    IOLockUnlock(displayGate);
    if(before!=after)IOLog("DitingDisplay: solid stage=%u error=%u restored=%u; physical colour unqualified\n",after,error,restored);
}
}

// Called only after a real GPU scanout copy completes, with the presenter gate
// held. One bounded transition; subsequent copies and USB snapshots serialize
// through displayGate. No PLL, panel command, pixel format or source is changed.
extern "C" void diting_display_copy_ready(uint64_t sequence) {
    if(__atomic_load_n(&initialized,__ATOMIC_ACQUIRE)!=2)return;
    uint32_t expected=0;
    if(!__atomic_compare_exchange_n(&copySnapshotAttempted,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return;
    auto &d=diting_display_copy_diagnostic;
    IOLockLock(displayGate);
    __atomic_add_fetch(&d.sequence,1,__ATOMIC_ACQ_REL);
    d.stage=1;d.us=uptimeUs();d.copy=sequence;
    Snapshot s=DitingDisplay::capture(board,{nullptr,read},true);
    d.before=s.intf[0].reg[14];d.console=matchesConsole(board,s);
    if(!d.console) {d.stage=4;d.error=s.status==Ok?5:unsigned(s.status)+1;}
    else {
        d.stage=2;
        auto &refresh=diting_display_refresh_diagnostic;
        __atomic_add_fetch(&refresh.sequence,1,__ATOMIC_ACQ_REL);
        refresh.us=d.us;refresh.copy=sequence;
        // The controller needs the same diagnostic instance throughout the
        // disabled/enabled transition and any bounded restoration.
        const bool changed=Refresh({{nullptr,read},write,pause},refresh).run(board,s);
        refresh.endUs=uptimeUs();
        __atomic_add_fetch(&refresh.sequence,1,__ATOMIC_RELEASE);
        d.after=refresh.after;
        if(changed)d.stage=3;
        else {d.stage=4;d.error=refresh.error;}
    }
    __atomic_add_fetch(&d.sequence,1,__ATOMIC_RELEASE);
    IOLockUnlock(displayGate);
    const bool ok=d.stage==3;
    IOLog("DitingDisplay: copy stage=%u error=%u console=%u auto before=%08x after=%08x refresh stage=%u writes=%u restored=%u; physical fps unqualified\n",
        d.stage,d.error,d.console,d.before,d.after,diting_display_refresh_diagnostic.stage,
        diting_display_refresh_diagnostic.writes,diting_display_refresh_diagnostic.restored);
    DitingProbeRecord(7,"display-copy",ok?DitingProbePass:DitingProbeSkip,
        ok?"completed copy; bounded DPU refresh transition; physical fps unqualified":"display refresh skipped/failed; normal boot continues",d.us);
    if(ok && &diting_panel_copy_ready)diting_panel_copy_ready();
}

// The backlight work loop calls this only after the identified DDIC has
// accepted the requested timing. Its existing command owner remains held.
extern "C" bool diting_display_panel_rate(unsigned hz) {
    if(__atomic_load_n(&initialized,__ATOMIC_ACQUIRE)!=2)return false;
    IOLockLock(displayGate);
    auto &d=diting_panel_tear_diagnostic;
    __atomic_add_fetch(&d.sequence,1,__ATOMIC_ACQ_REL);
    d.startUs=uptimeUs();d.error=d.writes=d.polls=d.restored=0;
    const Snapshot s=DitingDisplay::capture(board,{nullptr,read},true);
    const bool result=PanelTear({{nullptr,read},write,pause},d).run(board,s,hz);
    d.endUs=uptimeUs();
    __atomic_add_fetch(&d.sequence,1,__ATOMIC_RELEASE);
    IOLockUnlock(displayGate);
    return result;
}

extern "C" void diting_display_timing_start(IOService *display) {
    if (!display) return;
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&initialized, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    DitingProbeRecord(7, "display-timing", DitingProbeRunning, "validate SM8475 ADT; no register writes");
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    OSData *data = chosen ? OSDynamicCast(OSData, chosen->getProperty("diting-dpu-timing")) : nullptr;
    if (data && data->getLength() == sizeof(board)) memcpy(&board, data->getBytesNoCopy(), sizeof(board));
    OSSafeReleaseNULL(chosen);
    if (!DitingDisplay::valid(board)) {
        IOLog("DitingDisplayTiming: SKIP invalid SM8475 ADT; no MMIO\n");
        DitingProbeRecord(7, "display-timing", DitingProbeSkip, "invalid SM8475 ADT; no MMIO");
        __atomic_store_n(&initialized, 3, __ATOMIC_RELEASE); return;
    }
    const uint64_t base[3] = {board.mdp, board.clocks, board.dsi};
    const uint32_t span[3] = {board.mdpBytes, board.clockBytes, board.dsiBytes};
    for (unsigned i = 0; i < 3; ++i) {
        IODeviceMemory *memory = IODeviceMemory::withRange(base[i], span[i]);
        maps[i] = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        OSSafeReleaseNULL(memory);
        if (!maps[i]) {
            for (unsigned n = 0; n <= i; ++n) OSSafeReleaseNULL(maps[n]);
            IOLog("DitingDisplayTiming: SKIP mapping failed; no MMIO\n");
            DitingProbeRecord(7, "display-timing", DitingProbeSkip, "mapping failed; no MMIO");
            __atomic_store_n(&initialized, 3, __ATOMIC_RELEASE); return;
        }
        registers[i] = reinterpret_cast<volatile uint32_t *>(maps[i]->getVirtualAddress());
    }
    displayGate=IOLockAlloc();
    if(!displayGate) {
        for(unsigned i=0;i<3;++i)OSSafeReleaseNULL(maps[i]);
        DitingProbeRecord(7,"display-timing",DitingProbeSkip,"display gate allocation failed; no writes");
        __atomic_store_n(&initialized,3,__ATOMIC_RELEASE);return;
    }
    // Optional USB-only diagnostic. It never runs at boot or holds the gate
    // during the observation interval. ONCE prevents concurrent callbacks.
    solidCall=thread_call_allocate_with_options(solidFired,nullptr,THREAD_CALL_PRIORITY_KERNEL,THREAD_CALL_OPTIONS_ONCE);
    // All mappings persist until reboot. The USB readers never write. Only
    // the one completed-copy hook may perform the verified TE/idle transition.
    __atomic_store_n(&initialized, 2, __ATOMIC_RELEASE);
    IOLog("DitingDisplayTiming: SM8475 readers and bounded refresh handover ready; physical present/fps unqualified\n");
    DitingProbeRecord(7, "display-timing", DitingProbePass, "read-only reader published; physical present unqualified");
}

extern "C" unsigned diting_display_solid_text(const char *command,char *out,unsigned bytes) {
    Text text{out,bytes};const auto request=parseSolid(command);
    if(request.action==3) {
        text.add("SOLID schema=1 available=0 accepted=0 reason=bad-request physical_colour=UNQUALIFIED\n");return text.used;
    }
    if(__atomic_load_n(&initialized,__ATOMIC_ACQUIRE)!=2 || !solidCall) {
        text.add("SOLID schema=1 available=0 accepted=0 reason=controller-unavailable physical_colour=UNQUALIFIED\n");return text.used;
    }
    IOLockLock(displayGate);
    auto &d=diting_display_solid_diagnostic;auto controller=solidController();
    __atomic_add_fetch(&d.sequence,1,__ATOMIC_ACQ_REL);
    bool accepted=true;
    if(request.action==2)accepted=controller.request(request.colour,request.durationMs,uptimeUs());
    else if(request.action==1)controller.cancel(uptimeUs());
    if(request.action && accepted && controller.active())thread_call_enter(solidCall);
    __atomic_add_fetch(&d.sequence,1,__ATOMIC_RELEASE);
    const auto copy=d;
    IOLockUnlock(displayGate);
    const char *names[]={"IDLE","QUEUED","APPLYING","VISIBLE","RESTORING","DONE","REJECTED","POWER_WAIT","FAILED"};
    text.add("SOLID schema=1 available=1 accepted=%u stage=%u status=%s error=%u colour=%u duration_ms=%u writes=%u restored=%u cancelled=%u start_us=%llu visible_us=%llu expiry_us=%llu end_us=%llu flush=%08x frame=%u physical_colour=UNQUALIFIED mode=%s\n",
        unsigned(accepted),copy.stage,names[copy.stage<=SolidFailed?copy.stage:SolidFailed],copy.error,copy.colour,copy.durationMs,
        copy.writes,copy.restored,copy.cancelled,(unsigned long long)copy.startUs,(unsigned long long)copy.visibleUs,
        (unsigned long long)copy.expiryUs,(unsigned long long)copy.endUs,copy.flush,copy.frame,
        copy.colour==4?"FETCH_SWAP":"CONSTANT_COLOUR");
    for(unsigned n=0;n<2;++n)text.add("SOLIDPIPE index=%u before=%08x,%08x,%08x after=%08x,%08x,%08x\n",n,
        copy.before[n][0],copy.before[n][1],copy.before[n][2],copy.after[n][0],copy.after[n][1],copy.after[n][2]);
    return text.used;
}

extern "C" unsigned diting_display_snapshot_text(char *out, unsigned bytes) {
    Text text{out, bytes};
    if (__atomic_load_n(&initialized, __ATOMIC_ACQUIRE) != 2) {
        text.add("DISPLAY schema=1 status=SKIP reason=reader-unavailable physical_present=UNQUALIFIED\n");
        return text.used;
    }
    const uint64_t start = uptimeUs();
    IOLockLock(displayGate);
    Snapshot s = DitingDisplay::capture(board, {nullptr, read}, true);
    IOLockUnlock(displayGate);
    const uint64_t end = uptimeUs();
    char reason[128];
    snprintf(reason, sizeof(reason), "status=%u reads=%u frame=%u TE=%08x TS=%u physical=UNQUALIFIED",
        unsigned(s.status), s.reads, s.intf[0].reg[4], s.intf[0].reg[16], s.intf[0].stable);
    DitingProbeRecord(7, "display-timing", s.status == Ok ? DitingProbePass : DitingProbeSkip, reason, start);
    return DitingDisplay::formatSnapshot(out, bytes, board, s, start, end);
}

extern "C" unsigned diting_display_colour_text(char *out,unsigned bytes) {
    Text text{out,bytes};
    if(__atomic_load_n(&initialized,__ATOMIC_ACQUIRE)!=2) {
        text.add("COLOUR schema=1 status=SKIP reason=reader-unavailable register_writes=0 physical_colour=UNQUALIFIED\n");
        return text.used;
    }
    const uint64_t start=uptimeUs();
    IOLockLock(displayGate);
    ColourSnapshot s=captureColour(board,{nullptr,read});
    IOLockUnlock(displayGate);
    return formatColour(out,bytes,s,start,uptimeUs());
}

extern "C" void diting_display_observe_copy(uint64_t sequence) {
    if (__atomic_load_n(&initialized, __ATOMIC_ACQUIRE) != 2 || !&native_gpu_trace_enabled || !native_gpu_trace_enabled()) return;
    IOLockLock(displayGate);
    Snapshot s = DitingDisplay::capture(board, {nullptr, read}, false);
    IOLockUnlock(displayGate);
    const auto &v = s.intf[0];
    gpuTraceEvent(GpuTraceDisplayHardware, 0, sequence, unsigned(s.status), v.reg[4], s.intf[1].reg[4],
        v.panelTimestamp, v.mdpTimestamp, v.reg[10], v.reg[9], v.reg[3], v.stable, v.reg[14], v.reg[16]);
}
