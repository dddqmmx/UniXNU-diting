/* SPDX-License-Identifier: MIT */
/* Guest-specific stock Metal discovery over Diting's real IOAcceleratorES provider.
 * The OS loads this bundle and registers the returned native device.
 * No manual catalog mutation or iOS17 QuartzCore offsets.
 */
#import <Foundation/Foundation.h>
#import <CoreFoundation/CoreFoundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <initializer_list>
#include <mach/mach.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include "DitingMetalAbi.h"
#include "NativeBridgeMetalFrontend.h"
#include "NativeAdrenoABI.h"
#include "SharedShaderCache.h"

@interface _MTLDevice : NSObject
@end
#if DITING_GUEST_24B156
@interface NativeAdrenoMetalDevice : _MTLDevice
- (id)initWithAcceleratorPort:(unsigned)port;
@end
#else
@interface DitingAdrenoMetalDevice : _MTLDevice
- (id)initWithAcceleratorPort:(unsigned)port;
@end
#endif

namespace {
using Open=kern_return_t(*)(unsigned,mach_port_t,unsigned,unsigned *);
using Close=kern_return_t(*)(unsigned);
using Call=kern_return_t(*)(unsigned,unsigned,const void *,size_t,void *,size_t *);
using Property=CFTypeRef(*)(unsigned,CFStringRef,CFAllocatorRef,unsigned);
using Registry=kern_return_t(*)(unsigned,uint64_t *);
static id device;
static void *iokit;
}
static void report(const char *phase,int status)
{
    char line[256];snprintf(line,sizeof(line),"Diting Metal: %s status=0x%x\n",phase,status);
    (void)write(2,line,strlen(line));
    size_t count=CTL_MAXNAME;int mib[CTL_MAXNAME];
    if(!sysctlnametomib("kern.diting_diagnostic",mib,&count))
        (void)sysctl(mib,(u_int)count,NULL,NULL,line,strlen(line));
}
#include "DitingMetalScanout.h"
#include "DitingQuartzCore18.h"
#include "PrimeShaders.h"
namespace {
static void missing(const char *type,const char *selector)
{
    static unsigned count;
    if(__atomic_fetch_add(&count,1,__ATOMIC_RELAXED)>=12)return;
    char text[192];snprintf(text,sizeof(text),"missing %.70s %.90s",type,selector);
    report(text,-1);
}
#if DITING_GUEST_24B156
static bool propertyFalse(Property read,unsigned port,CFStringRef key)
{
    CFTypeRef value=read(port,key,kCFAllocatorDefault,0);
    bool no=value && CFGetTypeID(value)==CFBooleanGetTypeID() && !CFBooleanGetValue((CFBooleanRef)value);
    if(value)CFRelease(value);
    return no;
}
#else
static bool propertyTrue(Property read,unsigned port,CFStringRef key)
{
    CFTypeRef value=read(port,key,kCFAllocatorDefault,0);
    bool yes=value && CFGetTypeID(value)==CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)value);
    if(value)CFRelease(value);
    return yes;
}
#endif
static bool query(Call call,unsigned connection,unsigned selector,void *output,size_t bytes)
{
    size_t size=bytes;
    kern_return_t result=call(connection,selector,NULL,0,output,&size);
    if(result || size!=bytes){report("native ABI query refused",result?result:-1);return false;}
    return true;
}
static bool backend(unsigned port,uint64_t *registryID)
{
    char version[64]={};size_t length=sizeof(version);
    if(sysctlbyname("kern.osversion",version,&length,NULL,0) || strcmp(version,
#if DITING_GUEST_24B156
        "24B156"
#elif DITING_GUEST_22H374
        "22H374"
#else
        "20G81"
#endif
        )) {
        report("guest build differs from compiled Metal ABI",-1);return false;
    }
    if(!DitingMetalDeviceAbiAvailable(objc_getClass("_MTLDevice"))) {
        report("guest _MTLDevice ABI unavailable",-1);return false;
    }
    if(!iokit)iokit=dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",RTLD_NOW|RTLD_LOCAL);
    if(!iokit)return false;
    auto open=(Open)dlsym(iokit,"IOServiceOpen");auto close=(Close)dlsym(iokit,"IOServiceClose");
    auto call=(Call)dlsym(iokit,"IOConnectCallStructMethod");
    auto read=(Property)dlsym(iokit,"IORegistryEntryCreateCFProperty");
    auto registry=(Registry)dlsym(iokit,"IORegistryEntryGetRegistryEntryID");
    if(!open || !close || !call || !read || !registry)return false;
    /* CPReady is published only after real completed init fences. Enabled
     * requests this entry; it does not claim runtime/renderer qualification.
     * Runtime ABI and the hardware below must independently agree.
     * On 24B156, sandboxed app extensions (Poster/Widget) may be denied
     * custom IORegistry property reads even when IOServiceOpen is granted,
     * so refuse only an explicit false property and rely on the live
     * NativeAdreno ABI query below to verify CP/hardware readiness.
     */
#if DITING_GUEST_24B156
    if(propertyFalse(read,port,CFSTR("DitingAdrenoCPReady")) ||
       propertyFalse(read,port,CFSTR("DitingMetal24B156Enabled"))) {
        report("CP unavailable or Metal disabled; CPU display remains available",-1);return false;
    }
#else
    if(!propertyTrue(read,port,CFSTR("DitingAdrenoCPReady")) ||
       !propertyTrue(read,port,CFSTR(
#if DITING_GUEST_22H374
       "DitingMetal22H374Enabled"
#else
       "DitingMetal20G81Enabled"
#endif
       ))) {
        report("CP unavailable or Metal disabled; CPU display remains available",-1);return false;
    }
#endif
    unsigned connection=0;kern_return_t result=open(port,mach_task_self(),0x4147,&connection);
    if(result){report("native connection refused",result);return false;}
    NativeAdrenoAbiParams params={};NativeAdrenoAbiDeviceInfo info={};
    NativeAdrenoAbiStatus status={};NativeAdrenoAbiTimestamp time={};
    bool ready=query(call,connection,NativeAdrenoMethodQuery,&params,sizeof(params)) &&
        query(call,connection,NativeAdrenoMethodGetDeviceInfo,&info,sizeof(info)) &&
        query(call,connection,NativeAdrenoMethodGetStatus,&status,sizeof(status)) &&
        query(call,connection,NativeAdrenoMethodGetTimestamp,&time,sizeof(time));
    result=close(connection);
    if(result){report("native connection close failed",result);return false;}
    if(!ready || params.abi_version!=NativeAdrenoAbiVersion || params.input_bits<31 ||
       params.input_bits>48 || params.output_bits<32 || params.output_bits>48 ||
       info.chip_id!=0x07030002 || info.gmem_bytes!=0x200000 ||
       !info.va_size || (info.va_start&4095) || (info.va_size&4095) ||
       info.va_start>UINT64_MAX-info.va_size || status.faults || !time.ticks) {
        report("Diting hardware/ABI prerequisites failed",-1);return false;
    }
    return registry(port,registryID)==KERN_SUCCESS && *registryID;
}
static id openDevice(unsigned port)
{
    uint64_t registryID=0;if(!backend(port,&registryID))return nil;
    // DYLD injection does not load the executable through NSBundle. Locate
    // its sibling with dladdr as upstream does, independent of bundle lookup.
    Dl_info location={};
    if(!dladdr((const void *)&openDevice,&location) || !location.dli_fname)return nil;
    NSString *loader=[[@(location.dli_fname) stringByDeletingLastPathComponent]
        stringByAppendingPathComponent:@"libvulkan_freedreno.dylib"];
    if(access(loader.fileSystemRepresentation,R_OK)){report("Turnip library absent",-1);return nil;}
    NSString *process=[NSProcessInfo processInfo].processName;
    NSMutableArray<NSString *> *candidates=[NSMutableArray new];
    // Share immutable versioned entries for the same Unix user; container
    // caches remain available when the native sandbox denies the shared path.
    const std::string shared=sharedShaderCacheDirectory();
    if(!shared.empty())[candidates addObject:@(shared.c_str())];
    if(const struct passwd *user=getpwuid(getuid());user && user->pw_dir && user->pw_dir[0])
        [candidates addObject:[@(user->pw_dir) stringByAppendingPathComponent:
            [NSString stringWithFormat:@"Library/Caches/com.apple.%@/DitingMetal",process]]];
    for(int key:{_CS_DARWIN_USER_CACHE_DIR,_CS_DARWIN_USER_TEMP_DIR}) {
        char path[1024]={};
        if(confstr(key,path,sizeof(path)) && path[0])
            [candidates addObject:[@(path) stringByAppendingPathComponent:@"DitingMetal"]];
    }
    NSString *base=[NSSearchPathForDirectoriesInDomains(NSCachesDirectory,NSUserDomainMask,YES) firstObject];
    if(base)[candidates addObject:[[base stringByAppendingPathComponent:@"DitingMetal"] stringByAppendingPathComponent:process]];
    // main 461a842: let Turnip choose GMEM/sysmem for each attachment/pass.
    // Diting's BR/BV scheduling still requires concurrent binning disabled.
    // The matching Turnip build fixes linear BGRA GMEM transfers. nocb means
    // NO_CONCURRENT_BINNING; it does not disable the colour cache.
    {char args[1024]={};size_t size=sizeof args;
     if(!sysctlbyname("kern.bootargs",args,&size,nullptr,0)) {
      if(strstr(args,"diting_gpu_trace=1")){setenv("SHENG_GPU_TRACE","1",1);setenv("SHENG_DIAG","1",1);}
      if(strstr(args,"diting_gpu_trace=3"))setenv("DITING_GPU_TIMING","1",1);
      if(strstr(args,"diting_gpu_pixels=1"))setenv("SHENG_GPU_PIXELS","1",1);
      // Default to Turnip per-pass GMEM/sysmem selection (nocb),
      // with tu_gmem_transfer_swap_fixup active in Turnip.
      // 0 = forced sysmem, 1 = per-pass autotune, 2 = forced GMEM.
      const char *policy="nocb";
      if(const char *tu=strstr(args,"diting_tu=")) {
       if(tu[10]=='0')policy="sysmem,nocb";
       else if(tu[10]=='1')policy="nocb";
       else if(tu[10]=='2')policy="gmem,nocb";
      }
      setenv("TU_DEBUG",policy,1);
     } else setenv("TU_DEBUG","nocb",1);
     // Fixed USB test worker; other processes retain their selected policy.
     if(!geteuid() && !strcmp(getprogname(),"gpu-capture18")) {
      const char *test=getenv("DITING_GPU_TEST_POLICY");
      if(test && !strcmp(test,"auto"))setenv("TU_DEBUG","nocb",1);
      else if(test && !strcmp(test,"sysmem"))setenv("TU_DEBUG","sysmem,nocb",1);
      else if(test && !strcmp(test,"gmem"))setenv("TU_DEBUG","gmem,nocb",1);
     }}
    // Diting implements NativeAdreno directly, not Sheng's AGX selector tunnel.
    setenv("TU_DARWIN_SERVICE","NativeAdrenoGpuPublished",1);
    setenv("TU_DARWIN_OPEN_TYPE","0x4147",1);
    // The ABI1 clients have bounded GPU VA and all BOs are wired on Diting.
    // Keep a small reuse cache, not ten seconds of unbounded image spares.
    // This budget applies only after the runtime retires the last live owner.
    setenv("METAL2VULKAN_SPARE_IMAGE_MIB",!strcmp(getprogname(),"backboardd")?"128":"32",0);
    setenv("TU_DARWIN_BO_CACHE_MIB",!strcmp(getprogname(),"backboardd")?"96":
        !strcmp(getprogname(),"SpringBoard")?"64":"8",0);
    // Live Metal queues publish their current aggregate priority. Keep an
    // explicit user override, but never force a process-wide permanent boost.
    setenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION","1",1);
    ditingQuartzCore18DisplayPolicy();
    CFPreferencesSetAppValue(CFSTR("CAEnableDeepFramebuffer"),kCFBooleanFalse,kCFPreferencesCurrentApplication);
    CFPreferencesSetAppValue(CFSTR("CAEnableFramebufferCompression"),kCFBooleanFalse,kCFPreferencesCurrentApplication);
    CFPreferencesSetAppValue(CFSTR("CAEnableIOSurfaceCompression"),kCFBooleanFalse,kCFPreferencesCurrentApplication);
    id result=nil;
    for(NSString *cache in candidates) {
        NSError *error=nil;
        if(![[NSFileManager defaultManager] createDirectoryAtPath:cache withIntermediateDirectories:YES attributes:nil error:&error] ||
           access(cache.fileSystemRepresentation,W_OK))continue;
        @try {
            result=native_bridge_metal_open(loader.fileSystemRepresentation,cache.fileSystemRepresentation,registryID,missing);
        } @catch(NSException *exception) {
            NSLog(@"Diting Metal: device initialization failed: %@",exception);return nil;
        }
        if(result) {
            if(!strcmp(getprogname(),"backboardd")) {
                // Only a published native device, on a detached background
                // thread after UI startup; defer while real rendering is active.
                struct PrimeContext {id<MTLDevice> device;std::string cache;};
                auto *context=new PrimeContext{(id<MTLDevice>)result,cache.fileSystemRepresentation};
                pthread_attr_t attr;pthread_attr_init(&attr);
                pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
                pthread_attr_setstacksize(&attr,8u<<20);pthread_t thread;
                int started=pthread_create(&thread,&attr,[](void *arg)->void * {
                    auto *context=static_cast<PrimeContext *>(arg);
                    prime::run(context->device,context->cache,90,std::string());
                    delete context;return nullptr;
                },context);
                pthread_attr_destroy(&attr);if(started)delete context;
            }
            break;
        }
    }
#if DITING_GUEST_24B156
    if(!result) {
        // Sandboxed daemons/extensions (e.g. SBRendererService, iconservicesagent)
        // may lack any writable cache directory; allow in-memory runtime cache.
        setenv("METAL2VULKAN_SHADER_CACHE_OPTIONAL","1",1);
        const char *fallback=candidates.count?candidates.firstObject.fileSystemRepresentation:"/var/mobile/tmp/DitingMetal";
        @try {
            result=native_bridge_metal_open(loader.fileSystemRepresentation,fallback,registryID,missing);
        } @catch(NSException *exception) {
            NSLog(@"Diting Metal: fallback device initialization failed: %@",exception);return nil;
        }
    }
#endif
    report(result?"real Vulkan-backed device created":"runtime refused device",result?0:-1);
    return result;
}

static bool selectedClient()
{
    const char *process=getprogname();
    if(!process || !*process)return false;
    // Sheng uses publish/clients seed files to enable real device discovery.
    // The root-owned client seed is the single publication policy. A second
    // compiled list used to silently reject the wallpaper processes.
    // The extension view is the same root-owned seed under a system-readable
    // path. Sandboxed poster/widget clients cannot necessarily read /var/diting.
#if DITING_GUEST_24B156
    for(const char *directory:{"/System/Library/Extensions","/private/var/ngpu/addons"}) {
        char marker[256],clients[256];
        snprintf(marker,sizeof(marker),"%s/NativeAdrenoMetal.publish",directory);
        snprintf(clients,sizeof(clients),"%s/NativeAdrenoMetal.clients",directory);
        struct stat st={};
        if(lstat(marker,&st) || !S_ISREG(st.st_mode) || st.st_uid || (st.st_mode&022))continue;
        FILE *file=fopen(marker,"r");if(!file)continue;
        const bool publish=fgetc(file)=='1';fclose(file);if(!publish)return false;
        if(lstat(clients,&st) || !S_ISREG(st.st_mode) || st.st_uid || (st.st_mode&022))return true;
        file=fopen(clients,"r");if(!file)return true;
        char line[80];bool selected=false;
        while(fgets(line,sizeof(line),file)) {
            line[strcspn(line,"\r\n")]=0;
            if(!strcmp(line,process) || !strcmp(line,"*")){selected=true;break;}
        }
        fclose(file);return selected;
    }
    return false;
#else
    for(const char *directory:{"/System/Library/Extensions","/private/var/diting"}) {
        char marker[256],clients[256];
        snprintf(marker,sizeof(marker),"%s/DitingAdrenoMetal.publish",directory);
        snprintf(clients,sizeof(clients),"%s/DitingAdrenoMetal.clients",directory);
        struct stat st={};
        if(lstat(marker,&st) || !S_ISREG(st.st_mode) || st.st_uid || (st.st_mode&022))continue;
        FILE *file=fopen(marker,"r");if(!file)continue;
        const bool publish=fgetc(file)=='1';fclose(file);if(!publish)return false;
        if(lstat(clients,&st) || !S_ISREG(st.st_mode) || st.st_uid || (st.st_mode&022))continue;
        file=fopen(clients,"r");if(!file)continue;
        char line[80];bool selected=false;
        while(fgets(line,sizeof(line),file)) {
            line[strcspn(line,"\r\n")]=0;
            if(!strcmp(line,process)
#if DITING_GUEST_22H374
                || !strcmp(line,"*")
#endif
            ){selected=true;break;}
        }
        fclose(file);return selected;
    }
    return false;
#endif
}
#if DITING_GUEST_24B156
// iOS 27 DriverKit AppleUserHIDEventDriver never sets kIOHIDDigitizerEventFromEdgeTip
// (0x800), causing SpringBoard's bottom-edge Home Bar / Lock Screen unlock and
// Control Center / Back edge recognizers to reject touches (falling through to
// Notification Center "No Older Notifications"). Mark edge-originating contacts
// in backboardd's BKHIDDirectTouchEventProcessor using PAC-safe ObjC swizzling.
static uint32_t (*fnHidGetType)(void *);
static CFArrayRef (*fnHidGetChildren)(void *);
static CFIndex (*fnHidGetInt)(void *,uint32_t);
static void (*fnHidSetInt)(void *,uint32_t,CFIndex);
static double (*fnHidGetFloat)(void *,uint32_t);
static bool gEdgeContacts[256];
static bool gPrevTouch[256];
static bool gEdgeHookInstalled;
static uint32_t gEdgeTaggedCount;

static bool tagEdgeDigitizerChild(void *child)
{
    if(!child || fnHidGetType(child)!=11)return false;
    const CFIndex idx=fnHidGetInt(child,0xB0005u);
    if(idx<0 || idx>=256)return false;
    const CFIndex mask=fnHidGetInt(child,0xB0007u);
    const bool touch=fnHidGetInt(child,0xB0009u)!=0;
    const bool down=(touch && !gPrevTouch[idx]);
    if(down) {
        const double x=fnHidGetFloat(child,0xB0000u),y=fnHidGetFloat(child,0xB0001u);
        gEdgeContacts[idx]=(x<(18.0/375.0) || x>(1.0-18.0/375.0) ||
                            y<(28.0/812.0) || y>(1.0-64.0/812.0));
        if(gEdgeTaggedCount<8) {
            ++gEdgeTaggedCount;
            char msg[128];
            snprintf(msg,sizeof(msg),"edge-touch down idx=%ld mask=0x%lx x=%.3f y=%.3f edge=%d",
                     long(idx),long(mask),x,y,int(gEdgeContacts[idx]));
            report(msg,0);
        }
    }
    const bool edged=gEdgeContacts[idx];
    if(edged)fnHidSetInt(child,0xB0007u,mask|0x800);
    gPrevTouch[idx]=touch;
    if(!touch)gEdgeContacts[idx]=false;
    return edged;
}
static void tagEdgeDigitizerEvent(void *ev)
{
    if(!ev || !fnHidGetType || fnHidGetType(ev)!=11)return;
    CFArrayRef children=fnHidGetChildren(ev);
    if(children && CFArrayGetCount(children)>0) {
        bool any=false;
        const CFIndex n=CFArrayGetCount(children);
        for(CFIndex i=0;i<n;++i)any|=tagEdgeDigitizerChild((void *)CFArrayGetValueAtIndex(children,i));
        if(any)fnHidSetInt(ev,0xB0007u,fnHidGetInt(ev,0xB0007u)|0x800);
    } else {
        (void)tagEdgeDigitizerChild(ev);
    }
}
static long long ditingProcessTouchEvent(id self,SEL _cmd,void **eventPtr,id sender,id dispatcher)
{
    (void)_cmd;
    if(eventPtr && *eventPtr)tagEdgeDigitizerEvent(*eventPtr);
    using OrigFn=long long(*)(id,SEL,void **,id,id);
    return ((OrigFn)objc_msgSend)(self,sel_registerName("diting_processEvent:sender:dispatcher:"),eventPtr,sender,dispatcher);
}
static bool tryInstallEdgeTouchHook()
{
    if(gEdgeHookInstalled)return true;
    if(!iokit)iokit=dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",RTLD_NOW|RTLD_LOCAL);
    if(!iokit)return false;
    fnHidGetType=(uint32_t(*)(void *))dlsym(iokit,"IOHIDEventGetType");
    fnHidGetChildren=(CFArrayRef(*)(void *))dlsym(iokit,"IOHIDEventGetChildren");
    fnHidGetInt=(CFIndex(*)(void *,uint32_t))dlsym(iokit,"IOHIDEventGetIntegerValue");
    fnHidSetInt=(void(*)(void *,uint32_t,CFIndex))dlsym(iokit,"IOHIDEventSetIntegerValue");
    fnHidGetFloat=(double(*)(void *,uint32_t))dlsym(iokit,"IOHIDEventGetFloatValue");
    if(!fnHidGetType || !fnHidGetChildren || !fnHidGetInt || !fnHidSetInt || !fnHidGetFloat)return false;
    Class cls=objc_getClass("BKHIDDirectTouchEventProcessor");
    if(!cls)return false;
    SEL origSel=sel_registerName("processEvent:sender:dispatcher:");
    SEL swapSel=sel_registerName("diting_processEvent:sender:dispatcher:");
    Method origMethod=class_getInstanceMethod(cls,origSel);
    if(!origMethod)return false;
    if(!class_addMethod(cls,swapSel,(IMP)ditingProcessTouchEvent,method_getTypeEncoding(origMethod)))return false;
    Method swapMethod=class_getInstanceMethod(cls,swapSel);
    if(!swapMethod)return false;
    method_exchangeImplementations(origMethod,swapMethod);
    gEdgeHookInstalled=true;
    report("edge-touch FromEdgeTip hook installed on BKHIDDirectTouchEventProcessor",0);
    return true;
}

// iOS 27 CoreBrightness selects VMBLControl (mode 3, which drops all brightness
// requests and disables physical CBDisplayContaineriOS color/backlight control)
// whenever ADT vmm-present causes MobileGestalt to report IsVirtualDevice=true.
// Port of upstream scripts/appsync/BacklightMode.m using PAC-safe ObjC swizzling:
// remap mode 3 -> 1 (physical BLControl -> AppleARMBacklight) in backboardd only.
static bool gBacklightModeHookInstalled;
static BOOL ditingInitBLControl(id self,SEL _cmd,long long mode)
{
    (void)_cmd;
    const long long next=(mode==3)?1:mode;
    using Fn=BOOL(*)(id,SEL,long long);
    const BOOL ok=((Fn)objc_msgSend)(self,sel_registerName("diting_initBLControl:"),next);
    char msg[128];
    snprintf(msg,sizeof(msg),"CoreBrightness initBLControl mode=%lld->%lld ok=%d",mode,next,int(ok));
    report(msg,ok?0:-1);
    return ok;
}
static bool tryInstallBacklightModeHook()
{
    if(gBacklightModeHookInstalled || strcmp(getprogname(),"backboardd"))return gBacklightModeHookInstalled;
    const bool preloaded=objc_getClass("BrightnessSystemInternal")!=nil;
    (void)dlopen("/System/Library/PrivateFrameworks/CoreBrightness.framework/CoreBrightness",RTLD_NOW);
    Class cls=objc_getClass("BrightnessSystemInternal");
    if(!cls){report("CoreBrightness BrightnessSystemInternal unavailable",-1);return false;}
    SEL origSel=sel_registerName("initBLControl:");
    SEL swapSel=sel_registerName("diting_initBLControl:");
    Method origMethod=class_getInstanceMethod(cls,origSel);
    char argType[16]="";
    if(origMethod)method_getArgumentType(origMethod,2,argType,sizeof(argType));
    if(!origMethod || method_getNumberOfArguments(origMethod)!=3 || !argType[0] || !strchr("qQlL",argType[0])) {
        report("CoreBrightness initBLControl: ABI mismatch",-1);
        return false;
    }
    if(!class_addMethod(cls,swapSel,(IMP)ditingInitBLControl,method_getTypeEncoding(origMethod)))return false;
    Method swapMethod=class_getInstanceMethod(cls,swapSel);
    if(!swapMethod)return false;
    method_exchangeImplementations(origMethod,swapMethod);
    gBacklightModeHookInstalled=true;
    char msg[128];
    snprintf(msg,sizeof(msg),"CoreBrightness BacklightMode hook installed (preloaded=%d)",int(preloaded));
    report(msg,0);
    return true;
}
__attribute__((constructor)) static void ditingBackboardEarlyHooks()
{
    if(!strcmp(getprogname(),"backboardd")) {
        (void)tryInstallBacklightModeHook();
    }
}
#endif
static void startPresentation(id candidate,unsigned port)
{
    if(strcmp(getprogname(),"backboardd"))return;
#if DITING_GUEST_24B156
    if(!tryInstallEdgeTouchHook()) {
        for(unsigned sec:{1u,3u,6u})
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,int64_t(sec)*NSEC_PER_SEC),
                dispatch_get_main_queue(),^{(void)tryInstallEdgeTouchHook();});
    }
#endif
    using Ref=kern_return_t(*)(unsigned);
    auto retain=(Ref)dlsym(iokit,"IOObjectRetain"),release=(Ref)dlsym(iokit,"IOObjectRelease");
    if(!retain || !release || retain(port))return;
    // Leave the stock catalog's construction lock before creating the
    // presenter. Its actual client retains the device and scanout backing.
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE,0),^{
        ditingscanout::start(iokit,port,(id<MTLDevice>)candidate);release(port);
    });
    // Bounded observations of real application work, not synthetic renders.
    // RAM cache journals disappear on REC; retain these few status lines.
    for(unsigned seconds:{5u,20u,60u})
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,int64_t(seconds)*NSEC_PER_SEC),
            dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{
                char status[240]={};native_bridge_metal_status(status,sizeof(status));
                report(status,0);
                if(seconds>=20) {
                    char rows[4096]={};native_bridge_metal_telemetry(rows,sizeof rows);
                    char *line=rows;
                    while(*line) {
                        char *end=strchr(line,'\n');if(end)*end=0;
                        report(line,0);if(!end)break;line=end+1;
                    }
                }
            });
}
}
#if DITING_GUEST_24B156
@implementation NativeAdrenoMetalDevice
- (id)initWithAcceleratorPort:(unsigned)port
{
    self=[super init];if(!self || !port || !selectedClient())return nil;
    @synchronized([NativeAdrenoMetalDevice class]) {
        if(!device) {
            device=openDevice(port);
            if(device){report("real device returned to matching stock discovery",0);startPresentation(device,port);}
        }
        return device;
    }
}
@end
#else
@implementation DitingAdrenoMetalDevice
- (id)initWithAcceleratorPort:(unsigned)port
{
    self=[super init];if(!self || !port || !selectedClient())return nil;
    @synchronized([DitingAdrenoMetalDevice class]) {
        if(!device) {
            device=openDevice(port);
            if(device){report("real device returned to matching stock discovery",0);startPresentation(device,port);}
        }
        return device;
    }
}
@end
#endif

