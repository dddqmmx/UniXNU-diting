// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Generic AArch64 Darwin platform expert for a supplied Apple-format device
// tree. It derives from XNU's real IODTPlatformExpert and relies on the
// platform's own DT root name ("device-tree") for matching. No board name,
// SoC address or vendor identity is compiled into this source.
//
// The DT must contain the nodes XNU's IODT platform path consumes:
//   /chosen with a CHRP NVRAM proxy (nvram-bank-size, nvram-proxy-data)
//   /options, /cpus, /arm-io and /defaults
// The NVRAM proxy is required so the real IODTNVRAM is published before any
// security initialization. This driver does not fabricate NVRAM or suppress
// a missing one; absent inputs make configure() fail.

#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSIterator.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <uuid/uuid.h>
#include "DitingPlatformIdentity.h"

extern "C" bool diting_cpu_start(IOService *platform);
#if XNU_DITING_WIFI
extern "C" void diting_wifi_start_optional(void);
#endif
#if XNU_DITING_POWER
extern "C" void diting_power_start_optional(void);
#endif
#if XNU_DITING_ADRENO_PLATFORM
extern "C" void diting_adreno_start_optional(void);
#endif

// Observation counters describe actual published nubs, not started drivers.
extern "C" {
volatile uint32_t armdt_platform_configured = 0;
volatile uint32_t armdt_platform_child_count = 0;
volatile uint32_t armdt_platform_cpu_count = 0;
volatile uint32_t armdt_platform_bus_count = 0;
volatile uint32_t armdt_platform_defaults_count = 0;
volatile uint32_t armdt_platform_nvram_proxy = 0;
volatile uint32_t armdt_platform_device_count = 0;
volatile uint32_t armdt_platform_start_phase = 0;
// Source: 0 unavailable, 1 legacy boot argument, 2 board ADT, 3 native UUID.
// Status: 0 pending, 1 published, 2 absent, 3 invalid, 4 property write failed.
volatile uint32_t diting_platform_uuid_source = 0;
volatile uint32_t diting_platform_uuid_status = 0;
extern volatile uint32_t armcpu_start_phase, armcpu_registered, armcpu_gic_ready;
extern volatile int32_t armcpu_start_error;
uint64_t diting_platform_startup_diagnostic(void)
{
  return uint64_t(armdt_platform_start_phase & 255) |
         (uint64_t(armcpu_start_phase & 255) << 8) |
         (uint64_t(armcpu_registered & 255) << 16) |
         (uint64_t(armcpu_gic_ready & 255) << 24) |
         (uint64_t(uint32_t(armcpu_start_error)) << 32);
}
}

class ArmDTPlatform : public IODTPlatformExpert {
  OSDeclareDefaultStructors (ArmDTPlatform)
public:
  bool start (IOService *provider) override;
  bool configure (IOService *provider) override;
  const char *deleteList (void) override;
  const char *excludeList (void) override;
  long getGMTTimeOfDay (void) override;
  void setGMTTimeOfDay (long seconds) override;
private:
  long fCalendarOffset;
  bool hasNVRAMProxy (IOService *provider);
  bool createDeviceNubs (IOService *provider);
};

OSDefineMetaClassAndStructors (ArmDTPlatform, IODTPlatformExpert)

bool ArmDTPlatform::start (IOService *provider)
{
  armdt_platform_start_phase = 1;
  bool started = IODTPlatformExpert::start (provider);
  if (started) armdt_platform_start_phase = 11;
  return started;
}

// Boot-epoch clock, backed by a supplied host/firmware UTC sample
// and the real arch timer. This is a software clock, not a claimed PMIC RTC.
long ArmDTPlatform::getGMTTimeOfDay (void)
{
  long offset = __atomic_load_n (&fCalendarOffset, __ATOMIC_ACQUIRE);
  if (offset == 0) return 0;
  clock_sec_t uptime;
  clock_usec_t microseconds;
  clock_get_system_microtime (&uptime, &microseconds);
  return offset + (long) uptime;
}

void ArmDTPlatform::setGMTTimeOfDay (long seconds)
{
  clock_sec_t uptime;
  clock_usec_t microseconds;
  clock_get_system_microtime (&uptime, &microseconds);
  __atomic_store_n (&fCalendarOffset, seconds - (long) uptime, __ATOMIC_RELEASE);
}

// Serialized empty name lists: XNU's processTopLevel() treats a null pointer
// as "every entry" (remove every child), not as "nothing". An empty list
// genuinely selects no entry, so the supplied device tree is preserved.
const char *ArmDTPlatform::deleteList (void)
{
  return "()";
}

const char *ArmDTPlatform::excludeList (void)
{
  return "()";
}

bool ArmDTPlatform::hasNVRAMProxy (IOService *provider)
{
  IORegistryEntry *chosen = provider->childFromPath ("chosen", gIODTPlane);
  if (chosen == nullptr) {
    return false;
  }
  OSData *bankSize = OSDynamicCast (OSData, chosen->getProperty ("nvram-bank-size"));
  OSData *proxyData = OSDynamicCast (OSData, chosen->getProperty ("nvram-proxy-data"));
  bool valid = (bankSize != nullptr) && (bankSize->getLength () == sizeof (uint32_t)) &&
               (proxyData != nullptr) && (proxyData->getLength () != 0);
  chosen->release ();
  return valid;
}

bool ArmDTPlatform::createDeviceNubs (IOService *provider)
{
  IORegistryEntry *bus = provider->childFromPath ("arm-io", gIODTPlane);
  if (bus == nullptr) {
    return false;
  }
  // IODTPlatformExpert's IOPlatformDevice nubs resolve resources through
  // their provider's IOPlatformExpert::getNubResources. They must therefore
  // be attached to this platform expert, not to the arm-io IOPlatformDevice
  // (ArmVirtPlatform uses the same parent relationship). The device-tree
  // parent remains arm-io, so reg/ranges resolution still follows the bus.
  // Publish every real arm-io child, not only the first pattern match. The
  // GIC lives here, and the UFS host/PHY nodes must be visible to their native
  // drivers too. processTopLevel() still uses deleteList()/excludeList().
  OSIterator *iterator = bus->getChildIterator (gIODTPlane);
  bus->release ();
  if (iterator == nullptr) {
    return false;
  }
  uint32_t count = 0;
  IORegistryEntry *entry;
  while ((entry = OSDynamicCast (IORegistryEntry, iterator->getNextObject ())) != nullptr) {
    IOService *nub = createNub (entry);
    if (nub == nullptr) {
      continue;
    }
    if (!nub->attach (this)) {
      nub->release ();
      continue;
    }
    nub->registerService ();
    IOLog ("ArmDTPlatform: published DT nub %s\n", nub->getName ());
    nub->release ();
    count++;
  }
  iterator->release ();
  armdt_platform_device_count = count;
  return true;
}

bool ArmDTPlatform::configure (IOService *provider)
{
  armdt_platform_start_phase = 2;
  __atomic_store_n (&fCalendarOffset, 0, __ATOMIC_RELAXED);
  IORegistryEntry *chosen = provider->childFromPath ("chosen", gIODTPlane);
  OSData *sample = chosen ? OSDynamicCast (OSData, chosen->getProperty ("boot-epoch")) : nullptr;
  uint64_t epoch = 0;
  if (sample && sample->getLength () == sizeof (epoch))
    memcpy (&epoch, sample->getBytesNoCopy (), sizeof (epoch));
  if (chosen) chosen->release ();
  if (epoch >= 978307200ULL && epoch <= 4102444800ULL) {
    setGMTTimeOfDay ((long) epoch);
    IOLog ("ArmDTPlatform: software calendar anchored to boot-epoch %llu\n", epoch);
  }
  IORegistryEntry *cpus = provider->childFromPath ("cpus", gIODTPlane);
  IORegistryEntry *bus = provider->childFromPath ("arm-io", gIODTPlane);
  IORegistryEntry *defaults = provider->childFromPath ("defaults", gIODTPlane);
  IORegistryEntry *options = provider->childFromPath ("options", gIODTPlane);
  bool structure = (cpus != nullptr) && (bus != nullptr) &&
                   (defaults != nullptr) && (options != nullptr);
  if (cpus != nullptr) { cpus->release (); }
  if (bus != nullptr) { bus->release (); }
  if (defaults != nullptr) { defaults->release (); }
  if (options != nullptr) { options->release (); }
  if (!structure) {
    IOLog ("ArmDTPlatform: required /cpus, /arm-io, /defaults or /options node is absent\n");
    return false;
  }
  if (!hasNVRAMProxy (provider)) {
    IOLog ("ArmDTPlatform: /chosen does not supply a usable CHRP NVRAM proxy\n");
    return false;
  }
  armdt_platform_start_phase = 3;

  // The inherited implementation publishes real IODTNVRAM from /chosen,
  // then attaches the CPU and top-level nubs. No identity or resource is
  // invented here; failure is returned instead of replaced with a stub.
  if (!IODTPlatformExpert::configure (provider)) {
    IOLog ("ArmDTPlatform: inherited DT configuration failed\n");
    return false;
  }
  armdt_platform_start_phase = 4;

  // gethostuuid() waits for this real platform resource. Diting has no Apple
  // ECID/chip-id pair, so native Apple UUID generation has no input. Accept
  // an explicitly supplied, stable software platform UUID derived by the
  // local build from this phone's SoC identity. Do not invent an Apple ECID
  // or let IPConfiguration hold configd's startup indefinitely.
  char platformUUID[37] = {}, validatedUUID[37] = {};uuid_t parsed;
  OSString *nativeUUID = OSDynamicCast(OSString,provider->getProperty(kIOPlatformUUIDKey));
  bool identityValid = false;
  if (nativeUUID) {
    identityValid = diting_platform_identity_copy(nativeUUID->getCStringNoCopy(),
        nativeUUID->getLength()+1,validatedUUID);
    diting_platform_uuid_source = 3;
  } else if (PE_parse_boot_argn("diting_platform_uuid",platformUUID,sizeof platformUUID)) {
    identityValid = diting_platform_identity_copy(platformUUID,sizeof platformUUID,validatedUUID);
    diting_platform_uuid_source = 1;
  } else {
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen",gIODTPlane);
    OSObject *suppliedIdentity = chosen ? chosen->getProperty("diting-platform-uuid") : nullptr;
    OSData *identity = OSDynamicCast(OSData,suppliedIdentity);
    if (suppliedIdentity) {
      identityValid = identity && diting_platform_identity_copy(identity->getBytesNoCopy(),
          identity->getLength(),validatedUUID);
      diting_platform_uuid_source = 2;
    }
    if (chosen) chosen->release();
  }
  identityValid = identityValid && uuid_parse(validatedUUID,parsed)==0 && !uuid_is_null(parsed);
  if (identityValid && (nativeUUID || provider->setProperty(kIOPlatformUUIDKey,validatedUUID))) {
    IOService::publishResource(kIOPlatformUUIDKey,provider->getProperty(kIOPlatformUUIDKey));
    diting_platform_uuid_status = 1;
    IOLog("ArmDTPlatform: platform UUID resource published source=%u\n",diting_platform_uuid_source);
  } else {
    diting_platform_uuid_status = identityValid ? 4 : (diting_platform_uuid_source ? 3 : 2);
    IOLog("ArmDTPlatform: platform UUID unavailable source=%u status=%u; network initialization unready\n",
        diting_platform_uuid_source,diting_platform_uuid_status);
  }

  OSIterator *iterator = getChildIterator (gIOServicePlane);
  if (iterator == nullptr) {
    IOLog ("ArmDTPlatform: child enumeration unavailable\n");
    return false;
  }
  uint32_t count = 0;
  uint32_t cpuCount = 0;
  uint32_t busCount = 0;
  uint32_t defaultsCount = 0;
  IORegistryEntry *child;
  while ((child = OSDynamicCast (IORegistryEntry, iterator->getNextObject ())) != nullptr) {
    count++;
    OSData *type = OSDynamicCast (OSData, child->getProperty ("device_type"));
    if ((type != nullptr) && (type->getLength () == 4) &&
        (memcmp (type->getBytesNoCopy (), "cpu", 4) == 0)) {
      cpuCount++;
    }
    if (strcmp (child->getName (), "arm-io") == 0) {
      busCount++;
    }
    if (strcmp (child->getName (), "defaults") == 0) {
      defaultsCount++;
    }
  }
  iterator->release ();
  armdt_platform_child_count = count;
  armdt_platform_cpu_count = cpuCount;
  armdt_platform_bus_count = busCount;
  armdt_platform_defaults_count = defaultsCount;
  armdt_platform_nvram_proxy = 1;
  armdt_platform_start_phase = 5;
  // Publish the bus child devices (interrupt controller, VirtIO MMIO, ...)
  // as real IOPlatformDevice nubs. No driver is started here.
  if (!createDeviceNubs (provider)) {
    IOLog ("ArmDTPlatform: arm-io device publication failed\n");
    return false;
  }
  armdt_platform_start_phase = 6;
  // All CPU nubs now exist. Register them together before PSCI CPU_ON.
  if (!diting_cpu_start(this)) {
    IOLog("ArmDTPlatform: boot CPU startup failed\n");
    return false;
  }
  armdt_platform_configured = 1;
  armdt_platform_start_phase = 7;
#if XNU_DITING_POWER
  diting_power_start_optional();
#endif
  armdt_platform_start_phase = 8;
#if XNU_DITING_WIFI
  diting_wifi_start_optional();
#endif
  IOLog ("ArmDTPlatform: registered platform cpus=%u arm-io=%u devices=%u nvram-proxy=%u\n",
         cpuCount, busCount, armdt_platform_device_count, armdt_platform_nvram_proxy);
  registerService ();
  armdt_platform_start_phase = 9;
#if XNU_DITING_ADRENO_PLATFORM
  diting_adreno_start_optional();
#endif
  armdt_platform_start_phase = 10;
  return true;
}
