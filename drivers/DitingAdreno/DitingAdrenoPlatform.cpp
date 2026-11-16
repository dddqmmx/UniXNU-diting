/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* SM8475 cold GMU startup. No A740/Redwood platform owner or ready device.
 * All MMIO belongs to Diting's captured DT. This driver is the sole XNU owner
 * of the GPU SMMU; APSS DRV2 transactions use the shared board RPMh owner.
 * Do not steal a live master, a busy TCS or unrelated stream/context bank.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/task.h>
#include <libkern/c++/OSSharedPtr.h>
#include <pexpert/pexpert.h>
#include "DitingAdrenoGmu.h"
#include "DitingAdrenoClocks.h"
#include "DitingAdrenoSmmu.h"
#include "DitingAdrenoRegisters.h"
#include "DitingAdrenoCp.h"
#include "DitingAdrenoCpProfile.h"
#include "DitingAdrenoClockSnapshot.h"
#include "DitingAdrenoStartup.h"
#include "../DitingProbe.h"
#include "../DitingPower/DitingMemoryBandwidth.h"
#include "../DitingPower/DitingPlatformResources.h"

extern "C" const uint8_t DitingAdrenoGmuFirmware[];
extern "C" const size_t DitingAdrenoGmuFirmwareBytes;
extern "C" void diting_ramoops_diagnostic_write(const char *,int);
namespace {
struct Mapping {OSSharedPtr<IOMemoryMap> keep;uintptr_t cpu=0;size_t bytes=0;};
struct Memory {IOBufferMemoryDescriptor *keep=nullptr;uint8_t *cpu=nullptr;uint64_t pa=0;size_t bytes=0;};
static Mapping windows[DitingGpuWindowCount],gcc,db;
static Memory cache,dummy,hfi,logmem,tables;
static DitingClockState clocks;
static DitingGpuGmuDevice gmu;
static DitingAdrenoSmmuSession context;
static ArmSmmu4KConfig table_config;
static ArmSmmu4KResult table_result;
static ArmSmmu4KRange ranges[4];
static unsigned char visited[32];
static ArmSmmu4KAuditRegion region;
static ArmSmmu4KAuditConfig audit;
static ArmSmmu4KContextConfig config;
static ArmSmmu4KIo smmu_io;
static uint64_t bank_offset;
static unsigned parents_ready,attempted;
static unsigned diagnostics_ready,diagnostics_power_level;
static DitingAdrenoStartup::Log startup_log{};
static DitingGpuPowerResult startup_power{};
static unsigned startup_power_status,startup_finished;
static uint64_t microseconds();
static void record(const char *phase,unsigned result,uint64_t a=0,uint64_t b=0)
{
    DitingAdrenoStartup::put(startup_log,phase,result,a,b,microseconds());
    char text[240];int n=snprintf(text,sizeof(text),
        "DITING ADRENO: %s result=%u a=0x%llx b=0x%llx\n",phase,result,a,b);
    if(n>0){IOLog("%s",text);diting_ramoops_diagnostic_write(text,n<(int)sizeof(text)?n:(int)sizeof(text)-1);}
}
static bool map(Mapping &m,uint64_t physical,size_t bytes)
{
    OSSharedPtr<IOMemoryDescriptor> memory(IOMemoryDescriptor::withPhysicalAddress(
        physical,bytes,kIODirectionInOut),OSNoRetain);
    if(memory)m.keep=OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere|kIOMapInhibitCache),OSNoRetain);
    if(!m.keep)return false;
    m.cpu=m.keep->getVirtualAddress();m.bytes=bytes;return m.cpu!=0;
}
static uint32_t rd(const Mapping &m,size_t at)
{return *reinterpret_cast<volatile uint32_t *>(m.cpu+at);}
static void wr(const Mapping &m,size_t at,uint32_t value)
{__asm__ volatile("dsb sy":::"memory");*reinterpret_cast<volatile uint32_t *>(m.cpu+at)=value;__asm__ volatile("dsb sy":::"memory");}
static int barrier(void *){__asm__ volatile("dsb sy":::"memory");return 0;}
static uint64_t microseconds()
{clock_sec_t s;clock_usec_t u;clock_get_system_microtime(&s,&u);return uint64_t(s)*1000000+u;}
static int now(void *,uint64_t *out){*out=microseconds();return 0;}
static int wait(void *,unsigned us){IODelay(us);return 0;}
static void clean(const void *memory,size_t bytes)
{
    uint64_t ctr;__asm__ volatile("mrs %0, ctr_el0":"=r"(ctr));
    uintptr_t line=4u<<((ctr>>16)&15),at=reinterpret_cast<uintptr_t>(memory);
    uintptr_t end=at+bytes;
    for(at&=~(line-1);at<end;at+=line)__asm__ volatile("dc civac, %0"::"r"(at):"memory");
    __asm__ volatile("dsb sy":::"memory");
}
static bool allocate(Memory &m,size_t bytes)
{
    m.keep=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        IOOptionBits(kIODirectionInOut)|IOOptionBits(kIOMemoryPhysicallyContiguous)|
        IOOptionBits(kIOMemoryMapperNone),bytes,4096);
    if(!m.keep)return false;
    IOByteCount contiguous=0;m.cpu=static_cast<uint8_t *>(m.keep->getBytesNoCopy());
    m.pa=m.keep->getPhysicalSegment(0,&contiguous,kIOMemoryMapperNone);m.bytes=bytes;
    if(!m.cpu || !m.pa || (m.pa&4095) || contiguous<bytes || m.pa+bytes>UINT64_C(1)<<36)return false;
    bzero(m.cpu,bytes);clean(m.cpu,bytes);return true;
}
static bool poll(const Mapping &m,size_t at,uint32_t mask,uint32_t wanted,unsigned us)
{
    uint64_t begin=microseconds();
    for(unsigned n=0;n<us && microseconds()-begin<us;n++) {
        if((rd(m,at)&mask)==wanted)return true;IODelay(1);
    }
    return false;
}
static unsigned retained_corner(const DitingPowerResource &rail)
{
    // APSS DRV2 previously served all boot-firmware consumers. Until XNU
    // has a shared RPMh aggregator, do not lower an unknown retained vote.
    // Hold the highest ACTUAL allowed AUX corner, without changing CPU/GPU
    // frequency. Never invent a corner or exceed the firmware's own table.
    unsigned index=16,highest=0;
    for(unsigned i=0;i<rail.bytes/2 && i<16;i++) {
        unsigned v=rail.aux[2*i]|unsigned(rail.aux[2*i+1])<<8;
        if(v>highest){highest=v;index=i;}
    }
    return index;
}
static bool vote_parents(const DitingPowerInputs &in)
{
    uint32_t cx=retained_corner(in.cx);
    uint32_t mx=retained_corner(in.mx);
    if(cx==16 || mx==16){record("SKIP ARC AUX levels",1,cx,mx);return false;}
    const uint32_t addresses[]={in.mx.address,in.cx.address},values[]={mx,cx};
    // ARC commands remain actual AUX indices. The same lock now covers GPU,
    // WLAN and ADSP through response/reclaim; no check-then-trigger race and
    // no reuse of the DT's sleep TCS3/4 as active command slots.
    if(!diting_rpmh_vote_many(addresses,values,2)){record("FAIL parent RPMh request",1,mx,cx);return false;}
    record("parent ARC votes acknowledged",0,mx,cx);
    return true;
}
static int clock_read(void *,DitingClockWindow w,size_t at,uint32_t *v)
{const Mapping &m=w==DitingClockGcc?gcc:windows[DitingGpuClock];if(at>m.bytes-4)return 1;*v=rd(m,at);return 0;}
static int clock_write(void *,DitingClockWindow w,size_t at,uint32_t v)
{const Mapping &m=w==DitingClockGcc?gcc:windows[DitingGpuClock];if(at>m.bytes-4)return 1;wr(m,at,v);return 0;}
static int clock_prerequisite(void *)
{
    // GCC fixed LUCID_EVO GPLL0, actual L/alpha/lock, TCXO19.2MHz from DT.
    // Its hardware-decoded rate must be600MHz; no A740 PLL is programmed.
    uint64_t rate=19200000ULL*((rd(gcc,0x10)&0xffff)*65536ULL+(rd(gcc,0x14)&0xffff))/65536;
    return !parents_ready || !(rd(gcc,0)&0x80000000) || rate!=600000000;
}
static int smmu_read(void *,size_t at,uint32_t *v)
{const Mapping &m=windows[DitingGpuSmmu];if((at&3)||at>m.bytes-4)return 1;*v=rd(m,at);return 0;}
static int smmu_write(void *,size_t at,uint32_t v)
{const Mapping &m=windows[DitingGpuSmmu];if((at&3)||at>m.bytes-4)return 1;wr(m,at,v);return 0;}
static int smmu_read64(void *,size_t at,uint64_t *v)
{const Mapping &m=windows[DitingGpuSmmu];if((at&7)||at>m.bytes-8)return 1;*v=*reinterpret_cast<volatile uint64_t *>(m.cpu+at);return 0;}
static int smmu_write64(void *,size_t at,uint64_t v)
{const Mapping &m=windows[DitingGpuSmmu];if((at&7)||at>m.bytes-8)return 1;*reinterpret_cast<volatile uint64_t *>(m.cpu+at)=v;return barrier(nullptr);}
static int read_table(void *,uint64_t physical,uint64_t *value)
{
    if((physical&7)||physical<tables.pa||physical>tables.pa+tables.bytes-8)return 1;
    *value=*reinterpret_cast<volatile uint64_t *>(tables.cpu+physical-tables.pa);return 0;
}
static bool start_smmu()
{
    Mapping &m=windows[DitingGpuSmmu];uint32_t id0=rd(m,0x20),id1=rd(m,0x24),id2=rd(m,0x28);
    record("GPU SMMU actual IDs",0,(uint64_t(id0)<<32)|id1,id2);
    unsigned groups=id0&255,banks=id1&255;
    if((id1>>31)||groups<2||banks<2||rd(m,0x48))return false;
    // ID1 reports22 on actual Diting; DT intentionally exposes only21.
    // A capacity override limits the usable prefix, not the hardware ID.
    if(groups>DitingAdrenoStreamSlots)groups=DitingAdrenoStreamSlots;
    if(banks>DitingAdrenoContextBanks)banks=DitingAdrenoContextBanks;
    unsigned pages=1u<<(((id1>>28)&7)+1);bool ex=(id0>>8)&1;
    // Shared ASID namespaces: reserve1 for GMU and2 for GPU, independently
    // of the chosen context bank. The two stopped owned masters can be
    // replaced, but never collide with an enabled foreign translation.
    for(unsigned i=0;i<groups;i++) {
        uint32_t smr=rd(m,0x800+4*i),sc=rd(m,0xc00+4*i);
        if(!(ex?!!(sc&1024):!!(smr&0x80000000)) || (sc&0x30000))continue;
        uint32_t mask=(smr>>16)&(ex?65535:32767),sid=smr&65535;
        if((mask==0 || mask==0x400) && ((sid&~0x400u)==0 || (sid&~0x400u)==5))continue;
        unsigned b=sc&255;
        if(b>=(id1&255))return false;
        size_t at=(pages+b)*4096;
        if(!(rd(m,at)&1))continue;
        uint64_t lower=*reinterpret_cast<volatile uint64_t *>(m.cpu+at+0x20);
        uint64_t upper=*reinterpret_cast<volatile uint64_t *>(m.cpu+at+0x28);
        if((lower>>48)==1 || (lower>>48)==2 || (upper>>48)==1 || (upper>>48)==2) {
            record("SKIP ASID lease conflicts with foreign stream",1,b,sid);return false;
        }
    }
    unsigned slots[2]={~0u,~0u},found=0;
    // Masters are ACTUALLY stopped: GX is off, CM3 was observed held in reset.
    // Quarantine only exact5/405 or the vendor mask0x400 for these two SIDs.
    for(unsigned i=0;i<groups;i++) {
        uint32_t smr=rd(m,0x800+4*i),s2cr=rd(m,0xc00+4*i);
        bool valid=ex?!!(s2cr&1024):!!(smr&0x80000000);
        if(!valid)continue;
        uint32_t mask=(smr>>16)&(ex?65535:32767),sid=smr&65535;
        bool matches=!((sid^5)&~mask&65535)||!((sid^0x405)&~mask&65535);
        if(!matches)continue;
        if((mask!=0&&mask!=0x400)||(sid&~0x400u)!=5)return false;
        wr(m,0xc00+4*i,0x20000|(ex?1024:0));
        if(rd(m,0xc00+4*i)!=(0x20000u|(ex?1024u:0u)))return false;
        if(found<2)slots[found++]=i;else return false;
    }
    for(unsigned i=0;i<groups&&found<2;i++) {
        uint32_t smr=rd(m,0x800+4*i),s2cr=rd(m,0xc00+4*i);
        bool valid=ex?!!(s2cr&1024):!!(smr&0x80000000);
        if(!valid && i!=slots[0])slots[found++]=i;
    }
    if(found!=2)return false;
    unsigned bank=0;
    for(unsigned b=1;b<banks;b++) {
        bool other=false;
        for(unsigned i=0;i<groups;i++) {
            uint32_t smr=rd(m,0x800+4*i),sc=rd(m,0xc00+4*i);
            if((ex?!!(sc&1024):!!(smr&0x80000000)) && !(sc&0x30000) && (sc&255)==b)other=true;
        }
        if(!other){bank=b;break;}
    }
    if(!bank)return false;
    bank_offset=uint64_t(pages+bank)*4096;
    // The old context has no other live stream and its sole master is reset.
    wr(m,bank_offset,0);if(rd(m,bank_offset)!=0)return false;
    wr(m,bank_offset+0x618,0);wr(m,bank_offset+0x7f0,0);
    if(!poll(m,bank_offset+0x7f4,1,0,2000)||(rd(m,bank_offset+0x58)&0xc00001feu))return false;
    wr(m,bank_offset+4,0x32b); // Diting qcom,actlr mask0/0x7ff from actual DT.
    if(rd(m,bank_offset+4)!=0x32b)return false;
    uint32_t scr=(rd(m,0)&~13u)|0x402u|(ex?8u:0u);wr(m,0,scr);
    for(unsigned i=0;i<2;i++) {
        wr(m,0xc00+4*slots[i],0x20000);
        wr(m,0x800+4*slots[i],(ex?0:0x80000000u)|(i?0x405u:5u));
        wr(m,0xc00+4*slots[i],0x20000|(ex?1024:0));
    }
    if(!allocate(cache,0xffc000)||!allocate(dummy,0x2000)||!allocate(hfi,0x4000)||
       !allocate(logmem,0x4000)||!allocate(tables,32*4096))return false;
    /* HFI is bidirectional DMA. A volatile cached CPU view cannot observe a
     * fresh GMU ACK merely by DSB; use matching normal-NC/WC CPU attributes
     * before either agent touches these pages. Firmware cache is upload-only. */
    Memory *shared_pages[]={&dummy,&hfi,&logmem};
    for(Memory *shared:shared_pages) {
        NativeAdrenoPhysicalSpan span={0,shared->pa,shared->bytes};
        if(NativeAdrenoSetPagesCacheMode(&span,1,true))return false;
    }
    Memory *mem[]={&cache,&dummy,&hfi,&logmem};uint64_t iovas[]={0x4000,0x60000000,0x60002000,0x60006000};
    for(unsigned i=0;i<4;i++)ranges[i]={iovas[i],mem[i]->pa,mem[i]->bytes,i?3u:15u,0,2};
    table_config={32,36,tables.pa,8192};
    if(ArmSmmu4KBuild(&table_config,ranges,4,reinterpret_cast<uint64_t *>(tables.cpu),32,&table_result))return false;
    clean(tables.cpu,tables.bytes);
    region={ranges,4,32,32*512,visited,sizeof(visited)};
    audit={{nullptr,read_table},&region,nullptr};
    config={};config.lower=&table_config;config.lower_tables=&table_result;
    config.id0=id0;config.id2=id2;config.asid=1;config.continue_after_fault=1;config.polled_faults=1;
    config.ignore_top_byte=1;config.shared_asid=1;
    smmu_io={nullptr,m.bytes,smmu_read,smmu_read64,smmu_write,smmu_write64,barrier,1};
    DitingAdrenoSmmuResult result={};
    auto status=DitingAdrenoSmmuStart(&smmu_io,0x03da0000,DitingAdrenoSmmuGmu,bank,slots,
        &config,&audit,4096,&context,&result);
    record("GMU native4K context",status,bank,result.phase);
    if(status==ArmSmmu4KStateMismatch) {
        const auto &v=result.context.readback;
        record("GMU context mismatch CBAR/CBA2R",status,v.cbar,v.cba2r);
        record("GMU context mismatch TCR/TCR2",status,v.tcr,v.tcr2);
        record("GMU context mismatch SCTLR/MAIR",status,v.sctlr,(uint64_t(v.mair1)<<32)|v.mair0);
        record("GMU context mismatch roots",status,v.ttbr0,v.ttbr1);
    }
    return status==ArmSmmu4KSuccess;
}
static int healthy(void *)
{
    if(!clocks.ready||!context.ready||context.poisoned)return 1;
    Mapping &m=windows[DitingGpuSmmu];
    uint32_t global=rd(m,0x48),fault=rd(m,bank_offset+0x58);
    if(global || (fault&0xc00001fe)) {record("SMMU FAULT",1,global,fault);return 1;}
    return 0;
}
static int gpu_read(void *,unsigned w,size_t at,uint32_t *v)
{if(w>=DitingGpuWindowCount||at>windows[w].bytes-4||(at&3))return 1;*v=rd(windows[w],at);return 0;}
static int gpu_write(void *,unsigned w,size_t at,uint32_t v)
{if(w>=DitingGpuWindowCount||at>windows[w].bytes-4||(at&3))return 1;wr(windows[w],at,v);return 0;}
static int publish(void *,void *cpu,size_t bytes){clean(cpu,bytes);return 0;}
static int cp_read(void *,uint32_t reg,uint32_t *value)
{
    uint64_t pa=0x03d00000ULL+uint64_t(reg)*4;
    const unsigned order[]={DitingGpuCore,DitingGpuAo,DitingGpuRscc,DitingGpuGmu};
    for(unsigned w:order)if(pa>=DitingAdrenoWindows[w].physical && pa+4<=DitingAdrenoWindows[w].physical+windows[w].bytes)
        return gpu_read(nullptr,w,size_t(pa-DitingAdrenoWindows[w].physical),value);
    return 1;
}
static int cp_write(void *,uint32_t reg,uint32_t value)
{
    uint64_t pa=0x03d00000ULL+uint64_t(reg)*4;
    const unsigned order[]={DitingGpuCore,DitingGpuAo,DitingGpuRscc,DitingGpuGmu};
    for(unsigned w:order)if(pa>=DitingAdrenoWindows[w].physical && pa+4<=DitingAdrenoWindows[w].physical+windows[w].bytes)
        return gpu_write(nullptr,w,size_t(pa-DitingAdrenoWindows[w].physical),value);
    return 1;
}
static void run(void *,wait_result_t)
{
#if defined(DITING_GUEST_22H374)
    unsigned power_level=0; // Diting DT maximum: 900 MHz, paired DDR level11.
#else
    unsigned power_level=DitingAdrenoBootPowerLevel;
#endif
    PE_parse_boot_argn("diting_gpu_pwrlevel",&power_level,sizeof power_level);
    unsigned lock_max=1;PE_parse_boot_argn("diting_gpu_lock_max",&lock_max,sizeof lock_max);
    if(lock_max)power_level=0; // actual Diting DT900MHz and matching maximum BW
    if(power_level>=DitingAdrenoPowerLevelCount){record("SKIP invalid GPU power level",power_level);return;}
    record("startup requested; no accelerator published",0);
    if(!map(gcc,0x100000,0x1f4200)||!map(db,0x80860000,0x20000))
        {record("SKIP resource mapping",1);return;}
    DitingPowerInputs inputs={};auto input_status=DitingAdrenoReadPowerInputs(
        reinterpret_cast<const uint8_t *>(db.cpu),db.bytes,DitingAdrenoChipId,&inputs);
    if(input_status){record("SKIP command-db",input_status);return;}
    if(!vote_parents(inputs)){record("FAIL parent votes",1);return;}
    parents_ready=1;
    // GPUCC register interface must be clocked before even reading GPUCC.
    wr(gcc,0x81004,rd(gcc,0x81004)|1);IODelay(10);
    if(!(rd(gcc,0x81004)&1)){record("FAIL CFG AHB",1);return;}
    if(!map(windows[DitingGpuClock],0x03d90000,0xa000)){record("SKIP GPUCC map",1);return;}
    uint32_t gx=rd(windows[DitingGpuClock],0x905c);
    if(gx&0x80000000){record("SKIP GX already powered; require cold boot",1,gx);return;}
    DitingClockOps clock_ops={nullptr,clock_read,clock_write,barrier,now,wait,clock_prerequisite};
    DitingClockResult cr={};auto cs=DitingAdrenoEnableCxClocks(&clock_ops,&clocks,&cr);
    record("CX clocks",cs,(uint64_t(cr.offset)<<32)|cr.value,cr.phase);
    if(cs)return;
    record("Cape GPU PLL configuration",0,
        (uint64_t(rd(windows[DitingGpuClock],0x10))<<32)|rd(windows[DitingGpuClock],0x1010),
        (uint64_t(rd(windows[DitingGpuClock],0))<<32)|rd(windows[DitingGpuClock],0x1000));
    for(unsigned i=0;i<DitingGpuWindowCount;i++)if(i!=DitingGpuClock &&
        !map(windows[i],DitingAdrenoWindows[i].physical,DitingAdrenoWindows[i].bytes))
        {record("SKIP GPU window map",1,i);return;}
    uint32_t reset=rd(windows[DitingGpuGmu],GEN7_GMU_CM3_SYSRESET*4-0x68000);
    if(reset!=1){record("SKIP CM3 not held in reset",1,reset);return;}
    if(!start_smmu()){record("FAIL GMU SMMU ownership/configuration",1);return;}
    DitingCpOps cp={{nullptr,cp_write,cp_read,barrier},smmu_io,record};
    auto prepared=DitingAdrenoCpPrepare(&cp);
    if(prepared){record("FAIL GPU context preparation",prepared);return;}
    gmu.io={nullptr,healthy,gpu_read,gpu_write,barrier,publish,now,wait};
    DitingGpuGmuConfig gc={DitingAdrenoChipId,DitingAdrenoGmuFirmware,DitingAdrenoGmuFirmwareBytes,
        {reinterpret_cast<volatile uint32_t *>(cache.cpu),cache.bytes,0x4000},
        {reinterpret_cast<volatile uint32_t *>(dummy.cpu),dummy.bytes,0x60000000},
        {reinterpret_cast<volatile uint32_t *>(hfi.cpu),hfi.bytes,0x60002000},
        {reinterpret_cast<volatile uint32_t *>(logmem.cpu),logmem.bytes,0x60006000}};
    DitingGpuGmuResult gr={};auto gs=DitingAdrenoGmuStart(&gmu,&gc,&gr);
    record("GMU firmware/HFI",gs,(uint64_t(gr.last_register)<<32)|gr.last_value,gr.phase);
    if(gs)return;
    auto &pr=startup_power;auto ps=DitingAdrenoGmuStartPower(&gmu,&inputs,power_level,&pr);
    startup_power_status=ps;
    record("GMU power handshake",ps,(uint64_t(pr.command)<<32)|pr.firmware_error,pr.acknowledged);
    if(ps)return;
    record("GMU frequency/bandwidth request acknowledged",0,
        DitingAdrenoPowerLevels[power_level].hz,DitingAdrenoPowerLevels[power_level].bus);
    IOLog("[gpu clock] lock_max=%u request_hz=%u level=%u DDR=%u HFI=acknowledged OOB=held\n",lock_max,
        power_level<DitingAdrenoPowerLevelCount?DitingAdrenoPowerLevels[power_level].hz:0,power_level,
        power_level<DitingAdrenoPowerLevelCount?DitingAdrenoPowerLevels[power_level].bus:0);
    auto cp_status=DitingAdrenoCpStart(&gmu);
    record("CP/user-client startup",cp_status,gr.core_version,gr.hfi_version);
    if(!cp_status) {
        diagnostics_power_level=power_level;
        __atomic_store_n(&diagnostics_ready,1,__ATOMIC_RELEASE);
    }
    if(cp_status)record("GX clock state after CP failure",rd(windows[DitingGpuClock],0x90a8),
        (uint64_t(rd(windows[DitingGpuClock],0))<<32)|rd(windows[DitingGpuClock],0x1000),
        (uint64_t(rd(windows[DitingGpuClock],4))<<32)|rd(windows[DitingGpuClock],0x1004));
}
static void worker(void *parameter,wait_result_t result)
{
    run(parameter,result);
    DitingAdrenoStartup::Entry last{};
    for(unsigned i=0;i<DitingAdrenoStartup::Capacity;++i) {
        DitingAdrenoStartup::Entry entry{};
        if(DitingAdrenoStartup::get(startup_log,i,entry))last=entry;
    }
    const bool ok=__atomic_load_n(&diagnostics_ready,__ATOMIC_ACQUIRE)!=0;
    DitingProbeRecord(9,"gpu-startup",ok?DitingProbePass:DitingProbeFail,
        last.ready?last.phase:"startup ended without a completed CP",last.status);
    __atomic_store_n(&startup_finished,1,__ATOMIC_RELEASE);
    thread_terminate(current_thread());
    thread_block(THREAD_CONTINUE_NULL);
}
}
extern "C" unsigned diting_gpu_snapshot_text(char *out,unsigned capacity)
{
    if(!out || capacity<512)return 0;
    DitingClockSample sample{};
    const bool ready=__atomic_load_n(&diagnostics_ready,__ATOMIC_ACQUIRE)!=0;
    auto readClock=[](void *,uint32_t at,uint32_t *value)->int {
        if(!windows[DitingGpuClock].cpu || at>windows[DitingGpuClock].bytes-4)return 1;
        *value=rd(windows[DitingGpuClock],at);
        __asm__ volatile("dmb oshld":::"memory");return 0;
    };
    DitingAdrenoReadClockSample(nullptr,ready,readClock,&sample);
    const char *status=sample.status==DitingClockSamplePass?"PASS":
        sample.status==DitingClockSampleSkip?"SKIP":
        sample.status==DitingClockSampleChanged?"CHANGED":"FAIL";
    unsigned used=0;
    auto append=[&](const char *format,auto... words) {
        if(used>=capacity-1)return;
        int n=snprintf(out+used,capacity-used,format,words...);
        if(n>0)used+=(unsigned)n<capacity-used?(unsigned)n:capacity-used-1;
    };
    const unsigned level=ready?diagnostics_power_level:DitingAdrenoPowerLevelCount;
    append("GPU clock v1 status=%s uptime_us=%llu reads=%u requested_hz=%u requested_DDR=%u\n",
        status,microseconds(),sample.reads,level<DitingAdrenoPowerLevelCount?DitingAdrenoPowerLevels[level].hz:0,
        level<DitingAdrenoPowerLevelCount?DitingAdrenoPowerLevels[level].bus:0);
    unsigned retained=0;DitingAdrenoStartup::Entry last{};
    for(unsigned i=0;i<DitingAdrenoStartup::Capacity;++i) {
        DitingAdrenoStartup::Entry entry{};
        if(DitingAdrenoStartup::get(startup_log,i,entry)){last=entry;++retained;}
    }
    const bool finished=__atomic_load_n(&startup_finished,__ATOMIC_ACQUIRE)!=0;
    append("STARTUP schema=1 finished=%u retained=%u last_status=%u last_us=%llu phase=%s\n",
        unsigned(finished),retained,last.status,last.us,last.ready?last.phase:"not-requested");
    if(finished) {
        const auto &p=startup_power;
        append("GMU_POWER status=%u phase=%u command=%u header=%08x published=%u acknowledged=%u transport=%u error=%08x polls=%u boundary_polls=%u interrupt_clears=%u begin_us=%llu end_us=%llu max_poll_gap_us=%llu health_reads=%u fw=%08x hfi=%08x platform_error=%u\n",
            startup_power_status,p.phase,p.command,p.header,p.published,p.acknowledged,unsigned(p.transport),
            p.firmware_error,p.polls,p.boundary_polls,p.interrupt_clears,p.begin_us,p.end_us,
            p.max_poll_gap_us,p.health_reads,p.fw_status,p.hfi_status,p.platform_error);
    }
    if(used<capacity-1)used+=diting_memory_snapshot_text(out+used,capacity-used);
    if(sample.status!=DitingClockSamplePass) {
        append("unavailable: require completed CP startup, powered GX, and stable clock configuration\n");
        return used;
    }
    const auto &v=sample.value;
    append("GPUCC GX=%08x GFX_CBCR=%08x RDVM_CBCR=%08x GMU_CMD=%08x GMU_CFG=%08x HUB_CMD=%08x HUB_CFG=%08x\n",
        v[DitingClockGx],v[DitingClockGfx],v[DitingClockRdvm],v[DitingClockGmuCmd],
        v[DitingClockGmuCfg],v[DitingClockHubCmd],v[DitingClockHubCfg]);
    append("PLL0 MODE=%08x OPMODE=%08x STATUS=%08x L=%08x ALPHA=%08x USER=%08x\n",
        v[DitingClockPll0Mode],v[DitingClockPll0Op],v[DitingClockPll0Status],
        v[DitingClockPll0L],v[DitingClockPll0Alpha],v[DitingClockPll0User]);
    append("PLL1 MODE=%08x OPMODE=%08x STATUS=%08x L=%08x ALPHA=%08x USER=%08x\n",
        v[DitingClockPll1Mode],v[DitingClockPll1Op],v[DitingClockPll1Status],
        v[DitingClockPll1L],v[DitingClockPll1Alpha],v[DitingClockPll1User]);
    uint32_t perf=0,bw=0,ret=0,control=0,select=0,hi=0,lo=0,again=0;
    bool ok=!cp_read(nullptr,GEN7_GMU_DCVS_PERF_SETTING,&perf) &&
        !cp_read(nullptr,GEN7_GMU_DCVS_BW_SETTING,&bw) && !cp_read(nullptr,GEN7_GMU_DCVS_RETURN,&ret) &&
        !cp_read(nullptr,GEN7_RBBM_PERFCTR_CNTL,&control) && !cp_read(nullptr,GEN7_RBBM_PERFCTR_RBBM_SEL_0,&select);
    bool counter=false;
    for(unsigned i=0;ok && i<3;++i) {
        ok=!cp_read(nullptr,GEN7_RBBM_PERFCTR_RBBM_0_HI,&hi) &&
            !cp_read(nullptr,GEN7_RBBM_PERFCTR_RBBM_0_LO,&lo) &&
            !cp_read(nullptr,GEN7_RBBM_PERFCTR_RBBM_0_HI,&again);
        if(ok && hi==again){counter=true;break;}
    }
    if(ok && counter)append("GMU PERF=%08x BW=%08x RETURN=%08x RBBM_CONTROL=%08x RBBM_SELECT=%08x RBBM0=%llx\n",
        perf,bw,ret,control,select,(uint64_t(hi)<<32)|lo);
    else append("unavailable: bounded GMU/RBBM observation failed; no counter value substituted\n");
    append("Clock registers and counter readings require source-specific decoding; requested_hz is a vote.\n");
    return used;
}
extern "C" void diting_adreno_start_optional(void)
{
    unsigned requested=0;
    if(!PE_parse_boot_argn("diting_adreno_start",&requested,sizeof(requested))||requested!=1)return;
    if(__atomic_exchange_n(&attempted,1,__ATOMIC_ACQ_REL))return;
    DitingProbeRecord(9,"gpu-startup",DitingProbeRunning,"one cold startup; real CP completion required");
    thread_t thread=THREAD_NULL;auto status=kernel_thread_start(worker,nullptr,&thread);
    if(status!=KERN_SUCCESS){record("FAIL startup thread",static_cast<unsigned>(status));
        DitingProbeRecord(9,"gpu-startup",DitingProbeFail,"startup thread unavailable",status);
        __atomic_store_n(&startup_finished,1,__ATOMIC_RELEASE);return;}
    thread_deallocate(thread);
}
