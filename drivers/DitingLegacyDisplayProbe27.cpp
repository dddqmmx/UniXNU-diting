// SPDX-License-Identifier: BSD-2-Clause-Patent
// Diting iOS 27 (24B156) GOP legacy display & AppleKeyStore bring-up probe.
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <kern/task.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>
#include <machine/machine_routines.h>

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
extern kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern void native_legacy_display_static_init(void) __asm__("__GLOBAL__sub_I_NativeLegacyDisplay.cpp");
extern IOService *native_legacy_display_allocate(void);
extern void native_gop_presenter_static_init(void) __asm__("__GLOBAL__sub_I_NativeGOPPresenter.cpp");
extern IOService *native_gop_presenter_create(void);
extern void native_keystore_probe(void);
extern void diting_software_aes_probe(void);
extern IOMemoryDescriptor *native_legacy_surface_descriptor(OSObject *);
extern OSObject *native_legacy_surface_create(OSObject *, void *, OSDictionary *);
extern IOReturn native_legacy_surface_allocate(OSObject *);
extern uint64_t native_legacy_surface_width(OSObject *), native_legacy_surface_height(OSObject *);
extern uint64_t native_legacy_surface_stride(OSObject *);
extern uint32_t native_legacy_surface_format(OSObject *), native_legacy_surface_planes(OSObject *);
extern void diting_display_timing_start(IOService *display);
extern void diting_backlight_start(IOService *display);
extern void diting_display_copy_ready(uint64_t sequence);
extern void diting_memory_bandwidth_start(void);
extern void diting_adreno_start_optional(void);
extern void native_hid_input_probe(void);
extern void diting_touch27_start(void);
extern void diting_buttons27_start(void);
extern bool native_gop_gpu_offer(IOService *, void *display, IOMemoryDescriptor *, uint64_t stride, uint32_t width, uint32_t height, uint32_t format);
extern volatile uint64_t native_gop_virtual, native_gop_gpu_offers, native_gop_gpu_completions, native_gop_gpu_failures;
extern void native_gop_presenter_set_source(IOService *presenter, IOService *nub);
extern uint64_t gPhysBase, gPhysSize;
extern vm_offset_t phystokv(uint64_t);

char boot_progress_ext[8][48];
volatile uint32_t native_ufs_dpu_state = 0;
uint32_t native_ufs_dpu_map(unsigned, const uint64_t *, unsigned) { return 0; }
unsigned int diting_adreno_zap_reserved = 0;

int native_ram_block_gpu_addon(const uint8_t **data, uint64_t *bytes) {
    if (!data || !bytes) return 0;
    *data = nullptr;
    *bytes = 0;
    IORegistryEntry *map = IORegistryEntry::fromPath("/chosen/memory-map", gIODTPlane);
    if (!map) return 0;
    OSData *range = OSDynamicCast(OSData, map->getProperty("GpuAddon"));
    uint64_t physical = 0, size = 0;
    if (range && range->getLength() == 16) {
        memcpy(&physical, range->getBytesNoCopy(), 8);
        memcpy(&size, static_cast<const uint8_t *>(range->getBytesNoCopy()) + 8, 8);
    }
    map->release();
    if (!size || size > (UINT64_C(64) << 20) || (physical & 4095) ||
        physical < gPhysBase || size > gPhysSize || physical - gPhysBase > gPhysSize - size) {
        return 0;
    }
    *data = reinterpret_cast<const uint8_t *>(phystokv(physical));
    if (!*data) return 0;
    *bytes = size;
    return 1;
}

volatile uint32_t native_legacy_stage;
volatile int native_legacy_meta_status = -1;
volatile uint32_t native_legacy_init_result, native_legacy_start_result, native_legacy_probe_match;
volatile uint32_t native_legacy_parent_match, native_legacy_object_size;
volatile int native_legacy_surface_status = -1;
volatile uint32_t native_legacy_surface_found, native_legacy_dt_attached;
volatile uint32_t native_legacy_frame_count;
volatile uint32_t native_legacy_swap_accepted_count, native_legacy_swap_rejected_count;
volatile uint32_t native_legacy_client_calls;

static IOService *pendingLegacyDisplay;
static IOService *activeLegacyDisplay;
static IOService *activeGOPNub;
static IOService *gpuHandoffPresenter;

OSObject *native_legacy_default_surface_create(OSObject *root, uint32_t width, uint32_t height) {
    if (!root || !width || !height || width > 16384 || height > 16384) return nullptr;
    const uint64_t stride = (uint64_t(width) * 4 + 127) & ~uint64_t(127);
    const uint64_t bytes = (stride * height + 4095) & ~uint64_t(4095);
    OSDictionary *properties = OSDictionary::withCapacity(6);
    if (!properties) return nullptr;
    const char *keys[] = {"IOSurfaceWidth", "IOSurfaceHeight", "IOSurfaceBytesPerRow",
                          "IOSurfaceBytesPerElement", "IOSurfacePixelFormat", "IOSurfaceAllocSize"};
    const uint64_t values[] = {width, height, stride, 4, 0x42475241, bytes};
    bool valid = true;
    for (unsigned i = 0; i < 6 && valid; ++i) {
        OSNumber *number = OSNumber::withNumber(values[i], 64);
        valid = number && properties->setObject(keys[i], number);
        if (number) number->release();
    }
    OSObject *surface = valid ? native_legacy_surface_create(root, nullptr, properties) : nullptr;
    properties->release();
    if (!surface) {
        IOLog("DITING_FB: default surface factory failed\n");
        return nullptr;
    }
    IOReturn status = native_legacy_surface_allocate(surface);
    IOMemoryDescriptor *memory = native_legacy_surface_descriptor(surface);
    valid = status == kIOReturnSuccess && memory && memory->getLength() >= bytes &&
            native_legacy_surface_width(surface) == width && native_legacy_surface_height(surface) == height &&
            native_legacy_surface_stride(surface) == stride && native_legacy_surface_format(surface) == 0x42475241 &&
            native_legacy_surface_planes(surface) == 0;
    IOLog("DITING_FB: default surface %p status=0x%x valid=%u %ux%u stride=%llu bytes=%llu\n",
          surface, status, valid, width, height, stride, bytes);
    if (!valid) {
        surface->release();
        return nullptr;
    }
    return surface;
}

uint64_t native_legacy_surface_allocation(OSObject *surface) {
    IOMemoryDescriptor *memory = native_legacy_surface_descriptor(surface);
    return memory ? memory->getLength() : 0;
}

static volatile uint64_t gBootFbVa;
bool native_legacy_gop_mapping_observed(IOService *, IOMemoryMap *mapping, uint32_t width, uint32_t height) {
    if (mapping && mapping->getVirtualAddress()) {
        gBootFbVa = mapping->getVirtualAddress();
    }
    IOLog("DITING_FB: GOP mapped %ux%u pa=0x%llx va=0x%llx bytes=0x%llx\n",
          width, height,
          mapping ? (unsigned long long)mapping->getPhysicalAddress() : 0ULL,
          mapping ? (unsigned long long)mapping->getVirtualAddress() : 0ULL,
          mapping ? (unsigned long long)mapping->getLength() : 0ULL);
    return mapping != nullptr;
}

void native_legacy_registration_observed(IOService *) {
    IOLog("DITING_FB: IOMobileFramebuffer registered\n");
}

void native_legacy_swap_stage(unsigned) {}

extern int proc_selfpid(void);
extern void proc_selfname(char *buf, int size);

void native_legacy_client_enter(task_t, uint32_t type) {
    native_legacy_client_calls = native_legacy_client_calls + 1;
    if (native_legacy_client_calls <= 12) {
        char pname[32] = {};
        proc_selfname(pname, sizeof(pname));
        IOLog("DITING_FB: newUserClient #%u type=%u proc=%s[%d]\n",
              native_legacy_client_calls, type, pname, proc_selfpid());
    }
}

static void *gMfbClients[16];
static volatile uint32_t gMfbClientCount;
static volatile uint32_t gMfbTotalCalls;
static volatile uint32_t gMfbLogLines;
static volatile uint32_t gMfbSelCount[128];
static volatile int32_t gMfbSelErr[128];

void diting_mfb_uc_trace(void *client, uint32_t selector, IOReturn ret) {
    if (!client) return;
    uint32_t n = gMfbClientCount;
    bool match = false;
    for (uint32_t i = 0; i < n && i < 16; ++i) {
        if (gMfbClients[i] == client) { match = true; break; }
    }
    if (!match) return;
    gMfbTotalCalls = gMfbTotalCalls + 1;
    uint32_t idx = selector < 128 ? selector : 127;
    gMfbSelCount[idx] = gMfbSelCount[idx] + 1;
    if (ret != kIOReturnSuccess) gMfbSelErr[idx] = ret;
    uint32_t logged = gMfbLogLines;
    if (logged < 40) {
        gMfbLogLines = logged + 1;
        char pname[24] = {};
        proc_selfname(pname, sizeof(pname));
        IOLog("DITING_MFB: #%u %s[%d] sel=%u rc=0x%x\n", logged + 1, pname, proc_selfpid(), selector, ret);
    }
}

void native_legacy_client_observed(IOReturn status, IOUserClient *client) {
    if (status == kIOReturnSuccess && client) {
        uint32_t idx = gMfbClientCount;
        if (idx < 16) {
            gMfbClients[idx] = client;
            gMfbClientCount = idx + 1;
        }
    }
    if (native_legacy_client_calls <= 8) {
        IOLog("DITING_FB: newUserClient rc=0x%x client=%p\n", status, client);
    }
}

void native_legacy_pm_observed(unsigned stage, IOReturn status, unsigned long ordinal) {
    if (stage <= 3) {
        IOLog("DITING_FB: PM stage=%u status=0x%x ordinal=%lu\n", stage, status, ordinal);
    }
}

void native_legacy_frame_observed(unsigned kind, uint64_t count, uint64_t minx, uint64_t miny, uint64_t maxx, uint64_t maxy) {
    uint32_t n = native_legacy_frame_count;
    native_legacy_frame_count = n + 1;
    if (n < 16 || (n & 63) == 0) {
        IOLog("DITING_FB: frame #%u kind=%u lit=%llu box=(%llu,%llu)-(%llu,%llu)\n",
              n, kind, (unsigned long long)count,
              (unsigned long long)minx, (unsigned long long)miny,
              (unsigned long long)maxx, (unsigned long long)maxy);
    }
}

static volatile uint32_t gFrameColorLogs;
void native_legacy_frame_colors_observed(const uint32_t *colors, const uint64_t *counts, uint64_t other) {
    uint32_t n = __atomic_fetch_add(&gFrameColorLogs, 1, __ATOMIC_RELAXED);
    if (n < 8 && colors && counts) {
        IOLog("DITING_FB: colors #%u [0]=0x%08x(%llu) [1]=0x%08x(%llu) [2]=0x%08x(%llu) other=%llu\n",
              n, colors[0], (unsigned long long)counts[0],
              colors[1], (unsigned long long)counts[1],
              colors[2], (unsigned long long)counts[2],
              (unsigned long long)other);
    }
}

void native_legacy_swap_validation_observed(bool accepted, unsigned reason, const void *, const void *) {
    if (accepted) {
        native_legacy_swap_accepted_count = native_legacy_swap_accepted_count + 1;
        if (native_legacy_swap_accepted_count <= 8) {
            IOLog("DITING_FB: swap accepted #%u reason=%u\n", native_legacy_swap_accepted_count, reason);
        }
    } else {
        native_legacy_swap_rejected_count = native_legacy_swap_rejected_count + 1;
        if (native_legacy_swap_rejected_count <= 8) {
            IOLog("DITING_FB: swap rejected #%u reason=%u\n", native_legacy_swap_rejected_count, reason);
        }
    }
}

void native_legacy_surface_rejected(uint64_t w, uint64_t h, uint32_t fmt, uint32_t planes, uint64_t stride, uint64_t alloc, uint64_t bytes) {
    if (native_legacy_swap_rejected_count <= 8) {
        IOLog("DITING_FB: rejected surface %llux%llu fmt=0x%x planes=%u stride=%llu alloc=%llu bytes=%llu\n",
              (unsigned long long)w, (unsigned long long)h, fmt, planes,
              (unsigned long long)stride, (unsigned long long)alloc, (unsigned long long)bytes);
    }
}

static volatile uint32_t gConsoleReleased;
bool native_legacy_prepare_scanout(void) {
    if (__atomic_load_n(&gConsoleReleased, __ATOMIC_ACQUIRE)) {
        return true;
    }
    if (ml_at_interrupt_context()) {
        return false;
    }
    if (__atomic_exchange_n(&gConsoleReleased, 1, __ATOMIC_ACQ_REL) == 0) {
        IOLog("DITING_FB: first scanout; releasing boot console\n");
        PE_initialize_console(nullptr, kPEGraphicsMode);
        PE_initialize_console(nullptr, kPEReleaseScreen);
    }
    return true;
}

extern volatile uint32_t native_keyevents_stage;
extern volatile uint32_t native_aks_opens, native_aks_calls;

void diting_fb_log_summary(void) {
    char mfb[160];
    size_t pos = 0;
    mfb[0] = 0;
    for (uint32_t s = 0; s < 128 && pos + 20 < sizeof(mfb); ++s) {
        uint32_t c = gMfbSelCount[s];
        if (!c) continue;
        int32_t e = gMfbSelErr[s];
        int w = e ? snprintf(mfb + pos, sizeof(mfb) - pos, "%u:%u(e%x) ", s, c, (unsigned)(e & 0xfff))
                  : snprintf(mfb + pos, sizeof(mfb) - pos, "%u:%u ", s, c);
        if (w > 0) pos += (size_t)w;
    }
    IOLog("DITING_FB: summary stage=%u clients=%u frames=%u accepted=%u rejected=%u | MFB=%u [%s] | AKS stage=%u opens=%u calls=%u\n",
          native_legacy_stage, native_legacy_client_calls, native_legacy_frame_count,
          native_legacy_swap_accepted_count, native_legacy_swap_rejected_count,
          gMfbTotalCalls, mfb,
          native_keyevents_stage, native_aks_opens, native_aks_calls);
}
extern volatile uint32_t native_legacy_transfer_calls;
static volatile uint32_t native_legacy_transfer_logs;
static volatile uint64_t native_legacy_copy_seq;
void native_legacy_transfer_observed(IOReturn rc, const void *, uint64_t bytes) {
    native_legacy_transfer_calls = native_legacy_transfer_calls + 1;
    uint32_t n = native_legacy_transfer_logs;
    if (n < 8) {
        native_legacy_transfer_logs = n + 1;
        IOLog("DITING_FB: transfer #%u rc=0x%x bytes=%llu\n", n + 1, rc, (unsigned long long)bytes);
    }
    if (rc == kIOReturnSuccess) {
        uint64_t seq = __atomic_add_fetch(&native_legacy_copy_seq, 1, __ATOMIC_ACQ_REL);
        diting_display_copy_ready(seq);
    }
}
void native_legacy_copy_timing_observed(uint64_t, uint64_t) {}
void native_legacy_retirement_observed(IOService *) {}
bool native_legacy_gpu_offer(void *display, IOMemoryDescriptor *memory, uint64_t stride, uint32_t width, uint32_t height, uint32_t format) {
    return gpuHandoffPresenter && native_gop_gpu_offer(gpuHandoffPresenter, display, memory, stride, width, height, format);
}
static volatile uint32_t gGpuFallbackLogs;
void native_legacy_gpu_fallback_observed(int status) {
    uint32_t n = __atomic_fetch_add(&gGpuFallbackLogs, 1, __ATOMIC_RELAXED);
    if (n < 8) {
        IOLog("DITING_FB: gpu fallback #%u status=0x%x\n", n + 1, status);
    }
}
static volatile uint32_t native_legacy_vsync_logs;
void native_legacy_vsync_observed(unsigned event, unsigned hz) {
    uint32_t n = native_legacy_vsync_logs;
    if (n < 8) {
        native_legacy_vsync_logs = n + 1;
        IOLog("DITING_FB: vsync event=%u hz=%u\n", event, hz);
    }
}

__attribute__((noreturn, noinline)) void native_legacy_backend_guard(unsigned slot, void *, void *caller) {
    IOLog("DITING_FB: PANIC unimplemented leaf=%u caller=%p stage=%u\n", slot, caller, native_legacy_stage);
    panic("NativeLegacyDisplay: unimplemented leaf=%u caller=%p stage=%u", slot, caller, native_legacy_stage);
}

void native_legacy_display_probe(void) {
    if (native_legacy_stage) return;
    native_legacy_stage = 1;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {
        IOLog("DITING_FB: preModLoad failed\n");
        return;
    }
    native_legacy_display_static_init();
    native_gop_presenter_static_init();
    native_legacy_meta_status = OSMetaClass::postModLoad(handle);
    if (native_legacy_meta_status) {
        IOLog("DITING_FB: postModLoad failed 0x%x\n", native_legacy_meta_status);
        return;
    }
    PE_Video video = {};
    native_legacy_stage = 2;
    if (PE_current_console(&video) || !video.v_baseAddr || !video.v_width || !video.v_height ||
        video.v_depth != 32 || video.v_rotate || video.v_width > 16384 || video.v_height > 16384 ||
        video.v_rowBytes < video.v_width * 4 || video.v_rowBytes > UINT64_MAX / video.v_height) {
        IOLog("DITING_FB: PE_current_console invalid base=0x%lx %lux%lu depth=%lu row=%lu rot=%u\n",
              video.v_baseAddr, video.v_width, video.v_height, video.v_depth, video.v_rowBytes, (unsigned)video.v_rotate);
        return;
    }
    IOService *display = native_legacy_display_allocate();
    if (!display) {
        IOLog("DITING_FB: allocate failed\n");
        return;
    }
    native_legacy_object_size = display->getMetaClass()->getClassSize();
    native_legacy_parent_match = (display->metaCast("IOMobileFramebufferLegacy") &&
                                  display->metaCast("IOMobileFramebuffer") &&
                                  display->metaCast("IOService"));
    native_legacy_stage = 3;
    native_legacy_init_result = display->init();
    IOLog("DITING_FB: probe init=%u parent=%u size=0x%x video=%lux%lu@0x%lx\n",
          native_legacy_init_result, native_legacy_parent_match, native_legacy_object_size,
          video.v_width, video.v_height, video.v_baseAddr);
    if (native_legacy_init_result) {
        pendingLegacyDisplay = display;
        return;
    }
    display->release();
}

void native_legacy_display_late_probe(void) {
    diting_software_aes_probe();
    native_keystore_probe();
    IOLog("DITING_AKS: keystore probe done\n");
    IOService *display = pendingLegacyDisplay;
    if (!display) {
        return;
    }
    pendingLegacyDisplay = nullptr;
    native_legacy_surface_status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOSurface");
    OSDictionary *matching = OSDictionary::withCapacity(1);
    {
        const OSObject *names[] = {OSString::withCString("IOCoreSurfaceRoot"), OSString::withCString("IOSurfaceRoot")};
        OSArray *list = names[0] && names[1] ? OSArray::withObjects(names, 2) : nullptr;
        if (!matching || !list || !matching->setObject(gIONameMatchKey, list)) OSSafeReleaseNULL(matching);
        OSSafeReleaseNULL(list);
        for (const OSObject *n : names) if (n) n->release();
    }
    IOService *surface = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
    native_legacy_surface_found = surface != nullptr;
    OSSafeReleaseNULL(surface);
    OSSafeReleaseNULL(matching);
    PE_Video video = {};
    if (!native_legacy_surface_status && native_legacy_surface_found && !PE_current_console(&video) &&
        video.v_baseAddr && video.v_width && video.v_height && video.v_depth == 32 && !video.v_rotate) {
        native_legacy_stage = 4;
        IOService *nub = new IOService;
        bool nubAttached = false, nubStarted = false, attached = false, started = false;
        IORegistryEntry *displayTreeParent = nullptr;
        bool dtAttached = false;
        IOService *parent = IOService::getResourceService();
        IODeviceMemory *memory = IODeviceMemory::withRange(video.v_baseAddr, video.v_rowBytes * video.v_height);
        OSArray *ranges = OSArray::withCapacity(1);
        if (nub && memory && ranges && parent && nub->init()) {
            ranges->setObject(memory);
            nub->setDeviceMemory(ranges);
            nub->setName("NativeGOPFramebuffer");
            nubAttached = nub->attach(parent);
            if (nubAttached) nubStarted = nub->start(parent);
            if (nubStarted) {
                displayTreeParent = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
                const char type[] = "display-subsystem";
                OSData *data = OSData::withBytes(type, sizeof(type));
                if (displayTreeParent && data && nub->setProperty("device_type", data)) {
                    nub->setName("gop-display", gIODTPlane);
                    dtAttached = nub->attachToParent(displayTreeParent, gIODTPlane);
                }
                OSSafeReleaseNULL(data);
                // Publish physical display pixel density (70 mm active width on Diting: 1220 px -> 442.6857 / 443 PPI)
                // so IOMobileFramebufferLegacy::getDotPitch and MobileGestalt main-screen-pitch return real geometry
                // instead of 0 (which collapses UIScreen._nativePointsPerMillimeter and clips the keyboard dock).
                // Encode IEEE-754 float32 bits as integer to keep kernel code strictly -mgeneral-regs-only.
                const uint32_t dotPitch = static_cast<uint32_t>((video.v_width * 254ULL + 350ULL) / 700ULL);
                uint32_t dotPitchFloatBits = 0x43dd57c5u; // 442.685714f for 1220 px / 70 mm
                if (video.v_width != 1220 && dotPitch > 0) {
                    int msb = 31 - __builtin_clz(dotPitch);
                    uint32_t mant = (msb > 23) ? (dotPitch >> (msb - 23)) : (dotPitch << (23 - msb));
                    dotPitchFloatBits = (static_cast<uint32_t>(msb + 127) << 23) | (mant & 0x7fffffu);
                }
                if (OSData *pitchData = OSData::withBytes(&dotPitch, sizeof(dotPitch))) {
                    nub->setProperty("dot-pitch", pitchData);
                    pitchData->release();
                }
                if (OSData *pitchFloatData = OSData::withBytes(&dotPitchFloatBits, sizeof(dotPitchFloatBits))) {
                    nub->setProperty("dot-pitch-float", pitchFloatData);
                    pitchFloatData->release();
                }
                native_legacy_dt_attached = dtAttached;
                SInt32 score = 0;
                native_legacy_probe_match = dtAttached && display->probe(nub, &score) == display;
                if (native_legacy_probe_match) attached = display->attach(nub);
                if (attached) {
                    native_legacy_stage = 5;
                    started = display->start(nub);
                    native_legacy_start_result = started;
                }
                if (started) {
                    activeLegacyDisplay = display;
                    activeGOPNub = nub;
                    native_legacy_stage = 10;
                    diting_backlight_start(display);
                    unsigned fbGpu = 0;
                    if (PE_parse_boot_argn("sheng-fb-gpu", &fbGpu, sizeof(fbGpu)) && fbGpu == 1) {
                        IOService *presenter = native_gop_presenter_create();
                        if (presenter && presenter->init()) {
                            native_gop_presenter_set_source(presenter, nub);
                            if (presenter->attach(parent) && presenter->start(parent)) {
                                gpuHandoffPresenter = presenter;
                                IOLog("DITING_FB: GPU handoff presenter ready\n");
                            } else {
                                presenter->detach(parent);
                                presenter->release();
                            }
                        } else if (presenter) {
                            presenter->release();
                        }
                    }
                    diting_memory_bandwidth_start();
                    {
                        const uint64_t zap = 0x8b91a000ULL, zapEnd = zap + 0x2000ULL;
                        if (zapEnd <= gPhysBase || zap >= gPhysBase + gPhysSize) {
                            diting_adreno_zap_reserved = 1;
                        }
                    }
                    diting_adreno_start_optional();
                    native_hid_input_probe();
                    diting_touch27_start();
                    diting_buttons27_start();
                    OSSafeReleaseNULL(displayTreeParent);
                    IOLog("DITING_FB: published GOP display %lux%lu base=0x%lx dt=%u\n",
                          video.v_width, video.v_height, video.v_baseAddr, dtAttached);
                    return;
                }
            }
        }
        IOLog("DITING_FB: late probe failed stage=%u surface=%d/%u dt=%u probe=%u start=%u\n",
              native_legacy_stage, native_legacy_surface_status, native_legacy_surface_found,
              native_legacy_dt_attached, native_legacy_probe_match, native_legacy_start_result);
        if (started) display->stop(nub);
        if (attached) display->detach(nub);
        if (dtAttached) nub->detachFromParent(displayTreeParent, gIODTPlane);
        if (nubStarted) nub->stop(parent);
        if (nubAttached) nub->detach(parent);
        OSSafeReleaseNULL(ranges);
        OSSafeReleaseNULL(memory);
        OSSafeReleaseNULL(nub);
        OSSafeReleaseNULL(displayTreeParent);
    } else {
        IOLog("DITING_FB: surface unavailable status=0x%x found=%u\n",
              native_legacy_surface_status, native_legacy_surface_found);
    }
    display->release();
}

unsigned diting_scanout_pixels_text(const char *cmd, char *out, unsigned capacity) {
    if (!out || capacity < 64) return 0;
    PE_Video video = {};
    uint64_t va = native_gop_virtual ? native_gop_virtual : gBootFbVa;
    if (!va || PE_current_console(&video) || !video.v_width || !video.v_height || video.v_rowBytes < video.v_width * 4) {
        int n = snprintf(out, capacity, "PIXELS available=0 va=0x%llx offers=%llu comp=%llu fail=%llu acc=%u rej=%u\n",
                         (unsigned long long)va, (unsigned long long)native_gop_gpu_offers,
                         (unsigned long long)native_gop_gpu_completions, (unsigned long long)native_gop_gpu_failures,
                         native_legacy_swap_accepted_count, native_legacy_swap_rejected_count);
        return n > 0 ? (unsigned(n) < capacity ? unsigned(n) : capacity - 1) : 0;
    }
    const volatile uint8_t *base = reinterpret_cast<const volatile uint8_t *>(va);
    const uint32_t w = (uint32_t)video.v_width, h = (uint32_t)video.v_height;
    const uint64_t stride = (uint64_t)video.v_rowBytes;
    if (cmd && !strncmp(cmd, "display pixels band ", 20)) {
        // Format: "display pixels band Y0 ROWS STEP" -> RGB332 hex rows (2 hex chars per pixel).
        unsigned y0 = 0, rows = 4, step = 4;
        const char *p = cmd + 20;
        auto parseU = [](const char *&s, unsigned &v) -> bool {
            while (*s == ' ') ++s;
            if (*s < '0' || *s > '9') return false;
            unsigned acc = 0;
            while (*s >= '0' && *s <= '9') { acc = acc * 10u + unsigned(*s - '0'); ++s; }
            v = acc;
            return true;
        };
        if (!parseU(p, y0) || !parseU(p, rows) || !parseU(p, step) || step < 2 || step > 16 || rows == 0) {
            int n = snprintf(out, capacity, "BAND err=usage\n");
            return n > 0 ? unsigned(n) : 0;
        }
        if (rows > 8) rows = 8;
        const uint32_t cols = (w + step - 1) / step;
        static const char kHex[] = "0123456789abcdef";
        unsigned used = 0;
        int hdr = snprintf(out, capacity, "BAND w=%u h=%u y0=%u step=%u cols=%u\n", w, h, y0, step, cols);
        if (hdr > 0) used = unsigned(hdr) < capacity ? unsigned(hdr) : capacity - 1;
        for (uint32_t r = 0; r < rows; ++r) {
            const uint32_t y = y0 + r * step;
            if (y >= h) break;
            if (used + cols * 2 + 12 >= capacity) break;
            int rh = snprintf(out + used, capacity - used, "R%04u ", y);
            if (rh > 0) used += unsigned(rh);
            const volatile uint32_t *row = reinterpret_cast<const volatile uint32_t *>(base + y * stride);
            for (uint32_t x = 0; x < w; x += step) {
                const uint32_t px = row[x];
                const uint8_t b = uint8_t(px & 0xffu), g = uint8_t((px >> 8) & 0xffu), rr = uint8_t((px >> 16) & 0xffu);
                const uint8_t c = uint8_t((rr & 0xe0u) | ((g >> 3) & 0x1cu) | (b >> 6));
                out[used++] = kHex[c >> 4];
                out[used++] = kHex[c & 0xfu];
            }
            out[used++] = '\n';
        }
        out[used] = 0;
        return used;
    }
    if (cmd && !strcmp(cmd, "display pixels grid")) {
        static const char kRamp[] = " .:-=+*#%@";
        unsigned used = 0;
        int hdr = snprintf(out, capacity, "GRID 24x48 %ux%u offers=%llu\n", w, h, (unsigned long long)native_gop_gpu_offers);
        if (hdr > 0) used = unsigned(hdr) < capacity ? unsigned(hdr) : capacity - 1;
        for (uint32_t gy = 0; gy < 48 && used + 32 < capacity; ++gy) {
            const uint32_t y = (gy * h + h / 96) / 48;
            const volatile uint32_t *row = reinterpret_cast<const volatile uint32_t *>(base + y * stride);
            int r = snprintf(out + used, capacity - used, "%02u ", (gy * 100) / 48);
            if (r > 0) used += unsigned(r);
            for (uint32_t gx = 0; gx < 24 && used + 2 < capacity; ++gx) {
                const uint32_t x = (gx * w + w / 48) / 24;
                const uint32_t px = row[x];
                const uint32_t b = px & 0xffu, g = (px >> 8) & 0xffu, rr = (px >> 16) & 0xffu;
                const uint32_t luma = (rr * 3u + g * 6u + b) / 10u;
                out[used++] = kRamp[(luma * 9u) / 255u];
            }
            out[used++] = '\n';
        }
        out[used] = 0;
        return used;
    }
    uint32_t total = 0, lit = 0, unique[8] = {}, ucnt[8] = {}, ulen = 0, other = 0;
    uint32_t samples[8] = {};
    for (uint32_t gy = 0; gy < 32; ++gy) {
        const uint32_t y = (gy * h + h / 64) / 32;
        const volatile uint32_t *row = reinterpret_cast<const volatile uint32_t *>(base + y * stride);
        for (uint32_t gx = 0; gx < 32; ++gx) {
            const uint32_t x = (gx * w + w / 64) / 32;
            const uint32_t px = row[x];
            ++total;
            if ((px & 0x00ffffffU) != 0) ++lit;
            bool found = false;
            for (uint32_t k = 0; k < ulen; ++k) {
                if (unique[k] == px) { ++ucnt[k]; found = true; break; }
            }
            if (!found) {
                if (ulen < 8) { unique[ulen] = px; ucnt[ulen] = 1; ++ulen; }
                else ++other;
            }
        }
    }
    for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t y = ((i + 1) * h) / 9;
        const uint32_t x = w / 2;
        samples[i] = reinterpret_cast<const volatile uint32_t *>(base + y * stride)[x];
    }
    int n = snprintf(out, capacity,
        "PIXELS available=1 %ux%u stride=%llu offers=%llu comp=%llu fail=%llu acc=%u rej=%u grid=%u lit=%u\n"
        "COLORS u=%u other=%u [0]=%08x(%u) [1]=%08x(%u) [2]=%08x(%u) [3]=%08x(%u) [4]=%08x(%u) [5]=%08x(%u)\n"
        "MIDCOL %08x %08x %08x %08x %08x %08x %08x %08x\n",
        w, h, (unsigned long long)stride,
        (unsigned long long)native_gop_gpu_offers, (unsigned long long)native_gop_gpu_completions,
        (unsigned long long)native_gop_gpu_failures, native_legacy_swap_accepted_count,
        native_legacy_swap_rejected_count, total, lit,
        ulen, other,
        unique[0], ucnt[0], unique[1], ucnt[1], unique[2], ucnt[2],
        unique[3], ucnt[3], unique[4], ucnt[4], unique[5], ucnt[5],
        samples[0], samples[1], samples[2], samples[3], samples[4], samples[5], samples[6], samples[7]);
    return n > 0 ? (unsigned(n) < capacity ? unsigned(n) : capacity - 1) : 0;
}
}
