/* SPDX-License-Identifier: MIT */
/* Sheng's native Metal accelerator publication, bound to Diting's 20G81
 * parent ABI. All user clients execute the same real NativeAdreno backend. */
#include "DitingIOAcceleratorABI.h"
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <mach/kmod.h>
extern "C" void DitingIOAcceleratorMetaConstruct(void *);
#if defined(DITING_GUEST_24B156)
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
#endif
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void diting_ramoops_diagnostic_write(const char *,int);
#if defined(DITING_GUEST_24B156)
extern "C" void DitingAdrenoAcceleratorStaticInit(void) __asm__("__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp");
#else
extern "C" void DitingAdrenoAcceleratorStaticInit(void);
#endif
extern IOReturn DitingAdrenoNewUserClient(IOService *,task_t,void *,UInt32,OSDictionary *,IOUserClient **);
extern bool DitingAdrenoConfigureService(IOService *);

extern "C" IOService *native_adreno_accelerator_allocate(void);
extern "C" unsigned native_adreno_accelerator_instance_size(void);

IOReturn DitingAdrenoPublishMetalAccelerator()
{
#if defined(DITING_GUEST_24B156)
    (void)OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily");
#endif
    const OSSymbol *name=OSSymbol::withCString("IOAcceleratorES");
    const OSMetaClass *parent=name?OSMetaClass::getMetaClassWithName(name):nullptr;
    if(name)name->release();
    /* Diting implements IOSurface in the native kernel. Loading all of Apple's
     * IOGPUFamily dependencies would initialize a second IOSurfaceRoot. Run
     * only the original20G81 empty IOAcceleratorES metaclass constructor;
     * its C2/D2 and vtable also remain the actual original implementations.
     * No Apple IOGPU/AGX hardware provider is started or impersonated. */
    void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if(!handle)return kIOReturnNoResources;
#if defined(DITING_GUEST_24B156)
    if(!parent || parent->getClassSize()!=sizeof(IOAcceleratorES)) {
        (void)OSMetaClass::postModLoad(handle);return kIOReturnUnsupported;
    }
#else
    if(!parent)DitingIOAcceleratorMetaConstruct((void *)&IOAcceleratorES::gMetaClass);
    else if(parent->getClassSize()!=sizeof(IOAcceleratorES)) {
        (void)OSMetaClass::postModLoad(handle);return kIOReturnUnsupported;
    }
#endif
    DitingAdrenoAcceleratorStaticInit();
    IOReturn result=OSMetaClass::postModLoad(handle);
    if(result)return result;
    auto *service=native_adreno_accelerator_allocate();
    if(!service)return kIOReturnNoMemory;
    if(!service->init() || !DitingAdrenoConfigureService(service)) {
        service->release();return kIOReturnNotReady;
    }
#if defined(DITING_GUEST_24B156)
    service->setName("DitingAdreno730Metal");
    service->setProperty("MetalPluginName","NativeAdrenoMetal");
    service->setProperty("MetalPluginClassName","NativeAdrenoMetalDevice");
#else
    service->setName("DitingAdreno730Metal");
    service->setProperty("MetalPluginName","DitingAdrenoMetal");
    service->setProperty("MetalPluginClassName","DitingAdrenoMetalDevice");
#endif
    if(!service->attach(IOService::getPlatform())) {service->release();return kIOReturnNotAttached;}
    service->registerService();
    const char message[]="DITING METAL: real IOAcceleratorES provider published for matching guest discovery\n";
    IOLog("%s",message);diting_ramoops_diagnostic_write(message,sizeof(message)-1);
    return kIOReturnSuccess;
}
