/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* CPU MC3/SH5 requests and GPU-owned Cape LLCC slices. No CPU/DPU cache
 * partition, regulator, sleep/wake/fast TCS or firmware vote is replaced. */
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include "DitingPlatformResources.h"
#include "DitingMemoryBandwidth.h"
#include "DitingMemoryPolicy.h"
#include "DitingBandwidthProbe.h"
#include "DitingDisplayText.h"
extern "C" { volatile DitingMemoryDiagnostic diting_memory_diagnostic = {}; }
namespace {
using namespace DitingMemory;
IOLock *lock;
IOMemoryMap *dbMap, *llccMap, *broadcastMap, *bwmonMap[2];
volatile uint8_t *llcc, *broadcast, *bwmon[2];
uint32_t started;
bool bandwidthRetained;
uint64_t nowUs() { uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000; }
uint32_t rd(volatile uint8_t *base, unsigned off) {
    const uint32_t value = *reinterpret_cast<volatile uint32_t *>(base + off);
    __asm__ volatile("dmb oshld" ::: "memory"); return value;
}
void wr(volatile uint8_t *base, unsigned off, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(base + off) = value;
    __asm__ volatile("dsb oshst" ::: "memory");
}
volatile uint8_t *map(IOMemoryMap *&slot, uint64_t address, size_t bytes) {
    if (!slot) {
        auto memory = IODeviceMemory::withRange(address, bytes);
        if (!memory) return nullptr;
        slot = memory->map(kIOMapAnywhere | kIOMapInhibitCache); memory->release();
    }
    return slot ? reinterpret_cast<volatile uint8_t *>(slot->getVirtualAddress()) : nullptr;
}
void begin() { __atomic_add_fetch(&diting_memory_diagnostic.sequence, 1, __ATOMIC_ACQ_REL); }
void end() {
    diting_memory_diagnostic.us = nowUs();
    __atomic_add_fetch(&diting_memory_diagnostic.sequence, 1, __ATOMIC_RELEASE);
}
uint32_t readBank(void *, unsigned bank, unsigned off) { return rd(llcc, BankOffsets[bank] + off); }
uint32_t readBroadcast(void *, unsigned off) { return rd(broadcast, off); }
void writeBroadcast(void *, unsigned off, uint32_t value) { wr(broadcast, off, value); }
uint64_t timeCallback(void *) { return nowUs(); }
void delayCallback(void *, unsigned us) { IODelay(us); }
void sample() {
    if (llcc && broadcast) {
        for (unsigned bank = 0; bank < Banks; ++bank) {
            diting_memory_diagnostic.common_status[bank] = readBank(nullptr, bank, CommonStatus);
            for (unsigned i = 0; i < 2; ++i) {
                const unsigned id = i ? GpuScid : GpuHtwScid;
                diting_memory_diagnostic.slice[i][bank][0] = readBank(nullptr, bank, statusReg(id));
                diting_memory_diagnostic.slice[i][bank][1] = readBank(nullptr, bank, attr0Reg(id));
                diting_memory_diagnostic.slice[i][bank][2] = readBank(nullptr, bank, attr1Reg(id));
            }
        }
        const unsigned regs[] = {WriteScid, WriteCacheable, DisableCapacity, Retain};
        for (unsigned i = 0; i < 4; ++i) diting_memory_diagnostic.shared[i] = readBroadcast(nullptr, regs[i]);
    }
    // Passive reads only. An inherited/disabled BWMON is reported verbatim;
    // a zero counter is not a zero-bandwidth or actual-frequency measurement.
    const unsigned ddr[] = {0x10, 0x20, 0x38, 0x3c, 0x40, 0x44, 0x48, 0x4c, 0x50, 0};
    const unsigned cache[] = {0x2a0, 0x2a8, 0x2d0, 0x2d4, 0x2d8, 0x2e0, 0x2e4, 0x2e8, 0x2ec, 0x100};
    for (unsigned i = 0; i < 2; ++i) if (bwmon[i])
        for (unsigned j = 0; j < 10; ++j) diting_memory_diagnostic.bwmon[i][j] = rd(bwmon[i], (i ? cache : ddr)[j]);
}
struct BandwidthContext { volatile uint8_t *base; unsigned kind; };
bool bandwidthRead(void *cookie,DitingBandwidth::Region region,unsigned off,uint32_t *value) {
    const auto &c=*static_cast<BandwidthContext *>(cookie);
    if(!c.base || (off&3) || (region==DitingBandwidth::Global && c.kind!=4) || off>=0x300)return false;
    *value=rd(c.base-(region==DitingBandwidth::Global?0x100:0),off);return true;
}
bool bandwidthWrite(void *cookie,DitingBandwidth::Region region,unsigned off,uint32_t value) {
    const auto &c=*static_cast<BandwidthContext *>(cookie);
    if(!c.base || (off&3))return false;
    if(region==DitingBandwidth::Global) {
        if(c.kind!=4 || off!=0x108 || value!=1)return false;
    } else {
        const unsigned v5[]={0x8,0x10,0x14,0x18,0x1c,0x20,0x24,0x28,0x2c,0x30,0x34,0xc};
        const unsigned v4[]={0x108,0x2a0,0x2a4,0x2a8,0x2ac,0x2b0,0x2b4,0x2b8,0x2bc,0x10c};
        bool allowed=false;
        if(c.kind==5)for(unsigned candidate:v5)allowed=allowed || candidate==off;
        else for(unsigned candidate:v4)allowed=allowed || candidate==off;
        if(!allowed)return false;
    }
    wr(c.base-(region==DitingBandwidth::Global?0x100:0),off,value);return true;
}
void bandwidthSleep(void *,unsigned us) { IOSleep((us+999)/1000); }
}
extern "C" unsigned diting_memory_bandwidth_probe_text(char *out,unsigned capacity) {
    DitingDisplay::Text text{out,capacity};
    if(!lock || !IOLockTryLock(lock)) {
        text.add("BANDWIDTH schema=1 status=SKIP reason=memory-owner-unavailable register_writes=0\n");return text.used;
    }
    if(bandwidthRetained || diting_memory_diagnostic.stage!=4 || !bwmon[0] || !bwmon[1]) {
        text.add("BANDWIDTH schema=1 status=SKIP reason=%s register_writes=0\n",bandwidthRetained?"previous-restore-failed":"monitor-unavailable");
        IOLockUnlock(lock);return text.used;
    }
    DitingBandwidth::Diagnostic data[2];
    for(unsigned i=0;i<2;++i) {
        BandwidthContext context{bwmon[i],i?4u:5u};
        const DitingBandwidth::Io io{&context,bandwidthRead,bandwidthWrite,timeCallback,bandwidthSleep};
        DitingBandwidth::Probe(io,data[i]).run(context.kind);
        if(data[i].error==7) {bandwidthRetained=true;break;}
    }
    text.add("BANDWIDTH schema=2 status=%s sample_window_us=1000 count_unit_bytes=65536 requested_ddr_khz=%u actual_ddr_frequency=UNQUALIFIED metric=quantized-window-peak\n",
        data[0].stage==3 && data[1].stage==3?"PASS":"PARTIAL",diting_memory_diagnostic.bcm[0][4]);
    for(unsigned i=0;i<2;++i) {
        const auto &d=data[i];
        text.add("BWMON name=%s kind=%u stage=%u error=%u writes=%u restored=%u start_us=%llu end_us=%llu enable_before=%08x window_before=%08x window=%u count=%u timer=%u zones=%08x irq=%08x max=%u,%u,%u,%u peak_bytes_per_second=%llu\n",
            i?"llcc":"ddr",d.kind,d.stage,d.error,d.writes,d.restored,
            (unsigned long long)d.startUs,(unsigned long long)d.endUs,d.enabledBefore,d.windowBefore,
            d.window,d.count,d.timer,d.zones,d.irq,d.maximum[0],d.maximum[1],d.maximum[2],d.maximum[3],
            (unsigned long long)d.peakBytesPerSecond);
        const auto &g=d.guardsBefore;
        text.add("BWMON_GUARD name=%s reject_mask=%08x enable=%08x irq_enable=%08x irq=%08x count=%08x zones=%08x max=%u,%u,%u,%u global_irq=%08x global_irq_enable=%08x filter_mask=%08x filter_match=%08x timer=%08x\n",
            i?"llcc":"ddr",d.rejectMask,g[0],g[1],g[2],g[3],g[4],g[5],g[6],g[7],g[8],g[9],g[10],g[11],g[12],g[13]);
        text.add("BWMON_CONFIG name=%s words=",i?"llcc":"ddr");
        for(unsigned n=0;n<(d.kind==5?10u:8u);++n)text.add("%s%08x",n?",":"",d.configBefore[n]);
        text.add("\n");
    }
    begin();sample();end();IOLockUnlock(lock);return text.used;
}
extern "C" void diting_memory_bandwidth_start(void) {
    if (__atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL)) return;
    lock = IOLockAlloc();
    if (!lock) return;
    IOLockLock(lock); begin();
    diting_memory_diagnostic.version = 1; diting_memory_diagnostic.bytes = sizeof(DitingMemoryDiagnostic);
    diting_memory_diagnostic.stage = 1;
    unsigned enabled = 1; PE_parse_boot_argn("diting_memory_bw", &enabled, sizeof(enabled));
    diting_memory_diagnostic.enabled = enabled == 1;
    if (enabled != 1) { end(); IOLockUnlock(lock); return; }
    auto db = map(dbMap, 0x80860000, 0x20000);
    Bcm bcms[2] = {};
    uint32_t addresses[2] = {}, values[2] = {};
    const uint32_t khz[] = {DdrMaxKhz, LlccMaxKhz};
    bool ok = db && readBcm(const_cast<const uint8_t *>(db), 0x20000, "MC3", bcms[0]) &&
        readBcm(const_cast<const uint8_t *>(db), 0x20000, "SH5", bcms[1]) && bcms[0].address != bcms[1].address;
    for (unsigned i = 0; ok && i < 2; ++i) {
        ok = frequencyVote(bcms[i], khz[i], khz[i], values[i]); addresses[i] = bcms[i].address;
        const uint32_t fields[] = {bcms[i].address, bcms[i].unit, bcms[i].width, bcms[i].vcd, khz[i], values[i]};
        for (unsigned j = 0; j < 6; ++j) diting_memory_diagnostic.bcm[i][j] = fields[j];
    }
    if (!ok) diting_memory_diagnostic.error = -1;
    else {
        diting_memory_diagnostic.stage = 2;
        // Same APSS owner as GPU/WLAN/ADSP, active TCS0..2 only. MC3/SH5
        // are CPU resources; the GMU's SH0/MC0/ACV requests remain intact.
        if (diting_rpmh_vote_many(addresses, values, 2)) diting_memory_diagnostic.stage = 3;
        else diting_memory_diagnostic.error = -2;
    }
    end(); IOLockUnlock(lock);
    IOLog("DITING MEMORY: stage=%u error=%d MC3=%x/%x requested_ddr_khz=%u SH5=%x/%x requested_llcc_khz=%u\n",
        diting_memory_diagnostic.stage, diting_memory_diagnostic.error,
        addresses[0], values[0], khz[0], addresses[1], values[1], khz[1]);
}
extern "C" int diting_memory_gpu_cache(void) {
    diting_memory_bandwidth_start();
    if (!lock) return 0;
    IOLockLock(lock); begin();
    if (diting_memory_diagnostic.cache_ready) { end(); IOLockUnlock(lock); return 1; }
    if (diting_memory_diagnostic.stage != 3) { end(); IOLockUnlock(lock); return 0; }
    llcc = map(llccMap, 0x19200000, 0x580000);
    broadcast = map(broadcastMap, 0x19a00000, 0x80000);
    CacheResult result = CacheResult::Unavailable;
    if (llcc && broadcast) {
        const CacheIo io = {nullptr, readBank, readBroadcast, writeBroadcast, timeCallback, delayCallback};
        result = configureSlice(io, GpuHtwScid, 512, false);
        if (result == CacheResult::Ok) result = configureSlice(io, GpuScid, 2048, true);
        if (result == CacheResult::Ok) {
            diting_memory_diagnostic.cache_ready = 1; diting_memory_diagnostic.stage = 4;
        }
    }
    diting_memory_diagnostic.cache_result = int32_t(result);
    bwmon[0] = map(bwmonMap[0], 0x19091000, 0x1000);
    bwmon[1] = map(bwmonMap[1], 0x190b6000, 0x1000);
    if (bwmon[1]) bwmon[1] += 0x400;
    diting_memory_diagnostic.bwmon_mapped = (bwmon[0] ? 1u : 0u) | (bwmon[1] ? 2u : 0u);
    sample(); end(); IOLockUnlock(lock);
    IOLog("DITING MEMORY: Cape LLCC GPUHTW/GPU result=%d ready=%u; CPU/display slices retained\n",
        int(result), diting_memory_diagnostic.cache_ready);
    return diting_memory_diagnostic.cache_ready != 0;
}
extern "C" unsigned diting_memory_snapshot_text(char *out, unsigned capacity) {
    if (!out || !capacity) return 0;
    out[0] = 0;
    if (!lock || !IOLockTryLock(lock)) return 0;
    begin(); sample(); end();
    const int n = snprintf(out, capacity,
        "[memory] stage=%u error=%d MC3=%x/%x DDR_request_khz=%u SH5=%x/%x LLCC_request_khz=%u cache=%u/%d\n"
        "LLCC GPUHTW=%x/%x/%x GPU=%x/%x/%x WRSC=%x RETAIN=%x BWMON_EN=%x/%x SW=%x/%x raw_only=1\n",
        diting_memory_diagnostic.stage, diting_memory_diagnostic.error,
        diting_memory_diagnostic.bcm[0][0], diting_memory_diagnostic.bcm[0][5], diting_memory_diagnostic.bcm[0][4],
        diting_memory_diagnostic.bcm[1][0], diting_memory_diagnostic.bcm[1][5], diting_memory_diagnostic.bcm[1][4],
        diting_memory_diagnostic.cache_ready, diting_memory_diagnostic.cache_result,
        diting_memory_diagnostic.slice[0][0][0], diting_memory_diagnostic.slice[0][0][1], diting_memory_diagnostic.slice[0][0][2],
        diting_memory_diagnostic.slice[1][0][0], diting_memory_diagnostic.slice[1][0][1], diting_memory_diagnostic.slice[1][0][2],
        diting_memory_diagnostic.shared[0], diting_memory_diagnostic.shared[3],
        diting_memory_diagnostic.bwmon[0][0], diting_memory_diagnostic.bwmon[1][0],
        diting_memory_diagnostic.bwmon[0][1], diting_memory_diagnostic.bwmon[1][1]);
    IOLockUnlock(lock);
    return n <= 0 ? 0 : n < int(capacity) ? unsigned(n) : capacity - 1;
}
