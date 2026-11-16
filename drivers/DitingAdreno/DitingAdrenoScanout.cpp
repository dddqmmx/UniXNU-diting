/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Real frame offer/map/complete, adapted to Diting's existing Mini framebuffer.
 * A separate render-server connection waits for frames, never holding the BO
 * or submission scanout_gate. Only a real completed GPU copy retires a frame.
 */
#include "DitingAdrenoScanout.h"
#include "DitingScanoutPixels.h"
#define NATIVE_GPU_TRACE_OPTIONAL
#include "submit/NativeGpuTrace.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <kern/clock.h>
#include <kern/task.h>
#include <pexpert/pexpert.h>
extern "C" void diting_ramoops_diagnostic_write(const char *,int);
extern "C" void diting_display_observe_copy(uint64_t) __attribute__((weak_import));
extern "C" void diting_display_copy_ready(uint64_t) __attribute__((weak_import));
namespace {
IOLock *scanout_gate;
IOMemoryDescriptor *scanout;
task_t client;
DitingScanoutModeInfo mode{};
// These are the console's existing reserved-RAM mapping and physical extent.
// They never expose an arbitrary physical-address reader to USB/user space.
volatile uint32_t *scanoutPixels;
uint64_t scanoutPhysical;
unsigned pixelFrames;
uint64_t pixelLast;
bool pixelCapture;
uint32_t *currentPixels;
DitingScanoutPixels::Snapshot currentPixelSnapshot;
bool waiting=false,workerReady=false,poisoned=false,idleNotify=false;
uint64_t sequence=0,completed=0,sourceUse=0;
uint32_t nextSource=1;
struct Source {IOMemoryDescriptor *owner=nullptr,*memory=nullptr;IOMemoryMap *map=nullptr;
    size_t offset=0,bytes=0;uint32_t generation=0;uint64_t used=0;} sources[3];
bool releaseSource(Source &source) {
    if(!source.memory)return true;
    if(source.map){source.map->release();source.map=nullptr;}
    const IOReturn status=source.memory->complete();
    if(status){poisoned=true;return false;} // retain ownership on a real VM error
    source.memory->release();source.owner->release();source={};return true;
}
void releaseSources() {for(auto &source:sources)if(!releaseSource(source))break;}
struct Pending {bool offered=false,delivered=false,done=false;uint64_t id=0;
    uint32_t status=0;IOMemoryDescriptor *memory=nullptr;IOMemoryMap *map=nullptr;
    uint32_t stride=0;Source *source=nullptr;} pending;
void note(const char *phase,uint64_t id,unsigned status)
{
    char text[180];int n=snprintf(text,sizeof(text),"DITING SCANOUT: %s sequence=%llu status=0x%x\n",phase,id,status);
    if(n>0){IOLog("%s",text);diting_ramoops_diagnostic_write(text,n);}
}
uint64_t deadline(unsigned ms){uint64_t d;clock_interval_to_deadline(ms,kMillisecondScale,&d);return d;}

void capturePixels(const Source &source,uint32_t stride,uint64_t id)
{
    if(!pixelCapture || !scanoutPixels || pixelFrames>=2 || (stride&3) || id>UINT64_MAX/2-1 || !&native_gpu_pixel_put ||
       !&native_gpu_trace_enabled || !native_gpu_trace_enabled())return;
    uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);
    const uint64_t now=ns/1000;
    // Leave boot rendering alone; capture two completed UI copies, once each.
    // The scanout gate and framebuffer presenter keep the next copy/CPU write
    // from racing these reads. The source descriptor remains wired/retained.
    if(now<60000000 || (pixelLast && now-pixelLast<1000000))return;
    pixelLast=now;
    constexpr unsigned side=64, count=side*side, bytes=count*4;
    auto *data=static_cast<uint32_t *>(IOMallocZero(2*bytes));
    if(!data)return;
    auto sample=[&](const volatile uint32_t *base,uint32_t row,uint32_t *out) {
        for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x)
            out[y*side+x]=base[size_t(y*mode.height/side)*(row/4)+x*mode.width/side];
    };
    IOMemoryMap *map=pixelFrames==0?source.memory->map(kIOMapAnywhere):nullptr;
    bool paired=map && map->getVirtualAddress();
    if(paired)sample(reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress()),stride,data);
    sample(scanoutPixels,uint32_t(mode.stride),data+count);
    unsigned mismatch=0, swapped=0, changed=0;
    if(paired)for(unsigned i=0;i<count;++i) {
        const uint32_t a=data[i]&0xffffff,b=data[count+i]&0xffffff;
        mismatch+=a!=b;
        swapped+=(((a&255)<<16)|(a&0xff00)|(a>>16))==b;
    }
    for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x)
        changed+=data[count+y*side+x]!=scanoutPixels[size_t(y*mode.height/side)*(mode.stride/4)+x*mode.width/side];
    if(map)map->release();
    bool stored=true;
    for(unsigned role=paired?0:1;role<2 && stored;++role) {
        NativeGpuPixelPiece piece{};auto &h=piece.header;
        h.magic=0x31585047;h.version=1;h.stage=3;h.capture=id*2+role;h.us=now;h.command=id;
        h.surface=source.generation;h.physical=role?scanoutPhysical:0;
        h.width=mode.width;h.height=mode.height;h.format=80;h.colour_space=0;
        h.source_stride=role?mode.stride:stride;h.grid_width=h.grid_height=side;
        h.row_bytes=side*4;h.bytes_per_pixel=4;h.bytes=bytes;h.fence=id;
        h.flags=1|(role?32:16)|(role && changed?4:0);
        for(unsigned at=0;at<bytes && stored;at+=piece.bytes) {
            piece.offset=at;piece.bytes=bytes-at<NativeGpuPixelChunk?bytes-at:NativeGpuPixelChunk;
            memcpy(piece.data,reinterpret_cast<uint8_t *>(data+role*count)+at,piece.bytes);
            stored=native_gpu_pixel_put(&piece,0)==0;
        }
    }
    gpuTraceEvent(GpuTraceDisplayPixel,0,id,source.generation,paired,stored,stride,mode.stride,
        scanoutPhysical,count,mismatch,swapped,changed,now);
    IOFree(data,2*bytes);
    // Partial retained samples are immutable and are never overwritten/retried.
    if(!stored)pixelCapture=false;
    ++pixelFrames;
}
}
extern "C" void diting_scanout_initialize(uint64_t address,size_t bytes,uint32_t stride,uint32_t width,uint32_t height)
{
    unsigned enabled=0;
    if(scanout_gate || !PE_parse_boot_argn("diting_gpu_scanout",&enabled,sizeof(enabled)) || enabled!=1)return;
    if(!address || (address&4095) || !width || !height || stride<uint64_t(width)*4 ||
       bytes!=uint64_t(stride)*height || bytes>SIZE_MAX-4095)return;
    const size_t rounded=(bytes+4095)&~size_t(4095);
    if(address>UINT64_MAX-rounded)return;
    /* Sheng's GOP presenter hands out its device-memory descriptor, not a
     * pageable descriptor over the console's I/O VA. Diting's console is mapped
     * by ml_io_map_unmappable(), so prepare() cannot wire that VA. Use the
     * actual firmware physical range retained by PE_current_console(). Bit 0
     * is PE_create_console's physical-address marker; v_offset is the same
     * offset initialize_screen() adds to the console VA. */
    PE_Video video{};
    if(PE_current_console(&video) || !video.v_baseAddr || (video.v_baseAddr>>63) ||
       video.v_width!=width || video.v_height!=height || video.v_rowBytes!=stride ||
       video.v_depth!=32 || video.v_rotate ||
       (video.v_length && (video.v_offset>video.v_length || rounded>video.v_length-video.v_offset)))return;
    const uint64_t base=video.v_baseAddr&~UINT64_C(3);
    if(video.v_offset>UINT64_MAX-base)return;
    const uint64_t physical=base+video.v_offset;
    if(!physical || (physical&4095) || physical>UINT64_MAX-rounded)return;
    scanout=IOMemoryDescriptor::withPhysicalAddress(physical,rounded,kIODirectionInOut);
    if(!scanout)return;
    scanout_gate=IOLockAlloc();if(!scanout_gate){scanout->release();scanout=nullptr;return;}
    mode={DitingScanoutVersion,DitingScanoutBgra,width,height,stride,rounded};
    unsigned pixels=0;PE_parse_boot_argn("diting_gpu_pixels",&pixels,sizeof(pixels));
    if(pixels==1 && width==1220 && height==2712 && stride==4880 && physical==0xb8000000) {
        scanoutPixels=reinterpret_cast<volatile uint32_t *>(address);scanoutPhysical=physical;pixelCapture=true;
        currentPixels=static_cast<uint32_t *>(IOMallocZero(DitingScanoutPixels::Count*sizeof(uint32_t)));
    }
    note("actual framebuffer handoff available",0,0);
}
extern "C" unsigned diting_scanout_pixels_text(const char *request,char *out,unsigned bytes)
{
    using namespace DitingScanoutPixels;
    Request q{};DitingDisplay::Text text{out,bytes};
    if(!parse(request,q)) {
        text.add("PIXELS schema=1 status=REJECT reason=use-display-pixels-or-bounded-grid-range\n");return text.used;
    }
    if(!scanout_gate || !scanoutPixels || !currentPixels || scanoutPhysical!=0xb8000000) {
        text.add("PIXELS schema=1 status=SKIP reason=reserved-console-reader-unavailable\n");return text.used;
    }
    IOLockLock(scanout_gate);
    if(q.capture) {
        // The presenter gate serializes source lifetime and completed copies.
        // Do not read a framebuffer still owned by an outstanding GPU write.
        if(pending.offered && (!pending.done || pending.status)) {
            IOLockUnlock(scanout_gate);
            text.add("PIXELS schema=1 status=BUSY reason=scanout-copy-unretired\n");return text.used;
        }
        uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);
        const uint64_t start=ns/1000;
        auto at=[](unsigned index) {
            const unsigned y=index/Side,x=index%Side;
            return size_t(y*mode.height/Side)*(mode.stride/4)+x*mode.width/Side;
        };
        for(unsigned i=0;i<Count;++i)currentPixels[i]=scanoutPixels[at(i)];
        __asm__ volatile("dmb oshld" ::: "memory");
        unsigned changed=0;
        for(unsigned i=0;i<Count;++i)changed+=currentPixels[i]!=scanoutPixels[at(i)];
        absolutetime_to_nanoseconds(mach_absolute_time(),&ns);
        currentPixelSnapshot={currentPixelSnapshot.capture+1,completed,start,ns/1000,changed};
    }
    const unsigned used=currentPixelSnapshot.capture?format(out,bytes,currentPixelSnapshot,currentPixels,q):0;
    IOLockUnlock(scanout_gate);
    if(used)return used;
    text.add("PIXELS schema=1 status=SKIP reason=capture-current-console-first\n");return text.used;
}
extern "C" IOReturn diting_scanout_connect(task_t owner)
{
    if(!scanout_gate || !owner)return kIOReturnNotReady;
    IOLockLock(scanout_gate);IOReturn result=kIOReturnBusy;
    if(!client && !poisoned){task_reference(owner);client=owner;result=kIOReturnSuccess;}
    IOLockUnlock(scanout_gate);return result;
}
extern "C" void diting_scanout_disconnect(task_t owner)
{
    if(!scanout_gate)return;
    IOLockLock(scanout_gate);
    if(client==owner) {
        waiting=false;workerReady=false;
        if(pending.offered && pending.delivered && !pending.done) {
            poisoned=true; // unknown DMA lifetime: retain source/mapping.
            note("client closed with outstanding GPU copy; retain pages",pending.id,kIOReturnAborted);
        } else if(pending.offered && !pending.delivered) {
            pending.done=true;pending.status=kIOReturnAborted;
        }
        client=nullptr;task_deallocate(owner);
        if(!poisoned && !pending.offered)releaseSources();
        IOLockWakeup(scanout_gate,&pending,false);
    }
    IOLockUnlock(scanout_gate);
}
extern "C" IOMemoryDescriptor *diting_scanout_memory(task_t owner)
{
    if(!scanout_gate)return nullptr;
    IOLockLock(scanout_gate);auto *memory=client==owner && !poisoned?scanout:nullptr;
    if(memory)memory->retain();IOLockUnlock(scanout_gate);return memory;
}
extern "C" IOReturn diting_scanout_observe(DitingScanoutModeInfo *info,IOMemoryDescriptor **memory)
{
    // The root diagnostics connection enforces a read-only CPU map. This
    // observer neither claims the presenter nor imports/retire its surfaces.
    if(memory)*memory=nullptr;
    if(!scanout_gate || (!info && !memory))return kIOReturnNotReady;
    IOLockLock(scanout_gate);
    IOReturn status=scanout?kIOReturnSuccess:kIOReturnNotReady;
    if(scanout) {
        if(info)*info=mode;
        if(memory){scanout->retain();*memory=scanout;}
    }
    IOLockUnlock(scanout_gate);return status;
}
extern "C" bool diting_scanout_quarantined(void)
{
    if(!scanout_gate)return false;
    IOLockLock(scanout_gate);bool blocked=poisoned;IOLockUnlock(scanout_gate);return blocked;
}
extern "C" void diting_scanout_idle(void)
{
    if(!scanout_gate)return;
    IOLockLock(scanout_gate);
    if(client && !poisoned && !pending.offered) {
        workerReady=false;idleNotify=true;
        IOLockWakeup(scanout_gate,&pending,false);
    }
    IOLockUnlock(scanout_gate);
}
extern "C" IOReturn diting_scanout_call(task_t owner,unsigned op,const void *in,size_t inBytes,void *out,size_t outBytes)
{
    if(!scanout_gate)return kIOReturnNotReady;
    IOLockLock(scanout_gate);
    if(client!=owner || poisoned){IOLockUnlock(scanout_gate);return kIOReturnNotReady;}
    if(op==DitingScanoutResetSources && !inBytes && !outBytes) {
        // The matching presenter has already dropped its imports and drained
        // their autorelease pool. Stop new offers before checking retirement.
        workerReady=false;
        if(pending.offered){IOLockUnlock(scanout_gate);return kIOReturnBusy;}
        releaseSources();bool ok=!poisoned;
        IOLockUnlock(scanout_gate);return ok?kIOReturnSuccess:kIOReturnNotReady;
    }
    if(op==DitingScanoutMode && !inBytes && out && outBytes==sizeof(mode)) {
        memcpy(out,&mode,sizeof(mode));IOLockUnlock(scanout_gate);return kIOReturnSuccess;
    }
    if(op==DitingScanoutComplete && in && inBytes==sizeof(DitingScanoutDone) && !outBytes) {
        const auto &q=*static_cast<const DitingScanoutDone *>(in);
        if(q.reserved || !pending.offered || !pending.delivered || pending.done || q.sequence!=pending.id) {
            IOLockUnlock(scanout_gate);return kIOReturnBadArgument;
        }
        gpuTraceEvent(GpuTraceDisplayFence,0,q.sequence,q.status?2:1,q.sequence,q.sequence,q.status?0:q.sequence,0,0,0,q.status);
        pending.status=q.status;pending.done=true;
        if(q.status==DitingScanoutNotSubmitted)workerReady=false;
        if(q.status && q.status!=DitingScanoutNotSubmitted)poisoned=true;
        IOLockWakeup(scanout_gate,&pending,false);IOLockUnlock(scanout_gate);return kIOReturnSuccess;
    }
    if(op!=DitingScanoutWait || !in || inBytes!=sizeof(DitingScanoutWaitInfo) || !out || outBytes!=sizeof(DitingScanoutFrame)) {
        IOLockUnlock(scanout_gate);return kIOReturnBadArgument;
    }
    if(waiting){IOLockUnlock(scanout_gate);return kIOReturnBusy;}
    uint64_t ms=static_cast<const DitingScanoutWaitInfo *>(in)->timeout_ms;
    const uint64_t until=deadline(unsigned(ms>1000?1000:ms));waiting=true;workerReady=true;
    while(client==owner && !poisoned && !idleNotify && (!pending.offered || pending.delivered)) {
        if(IOLockSleepDeadline(scanout_gate,&pending,until,THREAD_ABORTSAFE)!=THREAD_AWAKENED) {
            waiting=false;IOLockUnlock(scanout_gate);return kIOReturnTimeout;
        }
    }
    waiting=false;
    if(client!=owner || poisoned){IOLockUnlock(scanout_gate);return kIOReturnNotReady;}
    if(idleNotify){idleNotify=false;workerReady=false;IOLockUnlock(scanout_gate);return kIOReturnAborted;}
    Source *source=pending.source;
    if(!source || !source->memory){IOLockUnlock(scanout_gate);return kIOReturnNotReady;}
    if(!source->map)source->map=source->memory->createMappingInTask(owner,0,kIOMapAnywhere);
    IOMemoryMap *map=source->map;
    if(!map || !map->getVirtualAddress()) {
        if(map){map->release();source->map=nullptr;}
        workerReady=false;pending.done=true;pending.status=kIOReturnNoMemory;
        IOLockWakeup(scanout_gate,&pending,false);IOLockUnlock(scanout_gate);return kIOReturnNoMemory;
    }
    pending.map=map;pending.delivered=true;
    *static_cast<DitingScanoutFrame *>(out)={pending.id,map->getVirtualAddress(),map->getLength(),pending.stride,
        uint32_t(mode.width),uint32_t(mode.height),mode.format,source->generation};
    IOLockUnlock(scanout_gate);return kIOReturnSuccess;
}
//0: caller keeps CPU path.1: completed real copy.2: DMA uncertain: keep the
// previous screen and pinned pages, never race CPU scanout against the GPU.
extern "C" int diting_scanout_frame(IOMemoryDescriptor *source,size_t offset,uint32_t stride,uint32_t width,uint32_t height)
{
    if(!scanout_gate || !source || stride<uint64_t(width)*4 || width!=mode.width || height!=mode.height || (offset&4095))return 0;
    IOLockLock(scanout_gate);
    if(poisoned){IOLockUnlock(scanout_gate);return 2;}
    if(!client || !workerReady || pending.offered){IOLockUnlock(scanout_gate);return 0;}
    size_t bytes=(size_t(stride)*height+4095)&~size_t(4095);
    if(offset>source->getLength() || bytes>source->getLength()-offset){IOLockUnlock(scanout_gate);return 0;}
    // Retain the compositor's bounded surface pool across frames. Mapping a
    // fresh subrange every frame made the presenter import and allocate again
    // on every swap, eventually hitting device memory pressure after wake.
    Source *slot=nullptr;
    for(auto &entry:sources) {
        if(entry.owner==source && entry.offset==offset && entry.bytes==bytes){slot=&entry;break;}
        if(!slot || !entry.owner || (slot->owner && entry.used<slot->used))slot=&entry;
    }
    if(slot->owner!=source || slot->offset!=offset || slot->bytes!=bytes) {
        if(!nextSource || !releaseSource(*slot)){IOLockUnlock(scanout_gate);return poisoned?2:0;}
        auto *memory=IOSubMemoryDescriptor::withSubRange(source,offset,bytes,kIODirectionInOut);
        if(!memory){IOLockUnlock(scanout_gate);return 0;}
        if(memory->prepare()!=kIOReturnSuccess){memory->release();IOLockUnlock(scanout_gate);return 0;}
        source->retain();*slot={source,memory,nullptr,offset,bytes,nextSource++,++sourceUse};
    }else slot->used=++sourceUse;
    pending={true,false,false,++sequence,0,slot->memory,slot->map,stride,slot};
    gpuTraceEvent(GpuTraceDisplay,0,pending.id,7,uint64_t(uintptr_t(source)),0,0,slot->generation,0,0,1);
    gpuTraceEvent(GpuTraceDisplaySurface,0,pending.id,0,width,height,stride,mode.format,bytes,slot->memory->getLength());
    IOLockWakeup(scanout_gate,&pending,false);const uint64_t until=deadline(2000);
    while(!pending.done && !poisoned)
        if(IOLockSleepDeadline(scanout_gate,&pending,until,THREAD_UNINT)!=THREAD_AWAKENED)break;
    if(pending.delivered && (!pending.done || (pending.status && pending.status!=DitingScanoutNotSubmitted))) {
        poisoned=true;note("GPU copy not retired; retain pages and previous frame",pending.id,pending.status?:kIOReturnTimeout);
        IOLockUnlock(scanout_gate);return 2;
    }
    if(!pending.delivered)workerReady=false; // a vanished waiter cannot stall every subsequent CPU frame
    bool ok=pending.done && !pending.status;
    gpuTraceEvent(GpuTraceDisplayLatch,0,pending.id,pending.status,0,ok?slot->generation:0,0);
    if(ok && &diting_display_copy_ready)diting_display_copy_ready(pending.id);
    if(ok && &diting_display_observe_copy)diting_display_observe_copy(pending.id);
    if(ok)capturePixels(*slot,stride,pending.id);
    if(ok && completed++<3)note("GPU frame completed",pending.id,0);
    // v2 cache owns the preparation and mapping until eviction/disconnect;
    // only a confirmed copy (or never submitted frame) reaches this point.
    pending={};
    if(!client)releaseSources();
    IOLockUnlock(scanout_gate);return ok?1:0;
}
