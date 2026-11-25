// SPDX-License-Identifier: BSD-2-Clause-Patent
// Registers the software AES-CBC IOAESAccelerator provider (NativeAES27.cpp) on
// iOS 27 (24B156) so AppleImage4's matching on /defaults completes and IOKit goes
// quiet, unblocking configd InterfaceNamer (en0) and WiFiKit/CoreWiFi.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/OSKextLib.h>
#include <kern/thread.h>
#include <mach/kmod.h>

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
void native_aes27_static_init(void) __asm__("__GLOBAL__sub_I_NativeAES27.cpp");
// 0 not run, 1 started (NativeAES registered), negative: step that failed.
volatile int diting_software_aes_state = 0;
}

namespace {

void fail(int step)
{
  diting_software_aes_state = -step;
  IOLog("DitingSoftwareAES: not started (%d)\n", -step);
  thread_terminate(current_thread());
}

void worker(void *, wait_result_t)
{
  const OSReturn loaded = OSKextLoadKextWithIdentifier("com.apple.iokit.IOCryptoAcceleratorFamily");
  if (loaded != kOSReturnSuccess && loaded != kOSKextReturnStartStopError) fail(1);
  void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
  if (!handle) fail(2);
  native_aes27_static_init();
  if (OSMetaClass::postModLoad(handle)) fail(3);
  OSObject *object = OSMetaClass::allocClassWithName("NativeAES");
  IOService *aes = OSDynamicCast(IOService, object);
  IOService *parent = IOService::getResourceService();
  if (!aes || !parent || !aes->init()) { OSSafeReleaseNULL(object); fail(4); }
  if (!aes->attach(parent)) { aes->release(); fail(5); }
  if (!aes->start(parent)) { aes->detach(parent); aes->release(); fail(6); }
  aes->release();
  diting_software_aes_state = 1;
  IOLog("DitingSoftwareAES: IOAESAccelerator registered\n");
  thread_terminate(current_thread());
}

}  // namespace

extern "C" void diting_software_aes_probe(void)
{
  thread_t thread = nullptr;
  if (kernel_thread_start(worker, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
  else diting_software_aes_state = -7;
}
