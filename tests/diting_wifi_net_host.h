/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Host boundaries for the actual DitingWifiNet.cpp. Copies, descriptors and
 * event delivery assert that the driver's state lock is not held.
 */
#ifndef DITING_WIFI_NET_HOST_H
#define DITING_WIFI_NET_HOST_H
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>

// Use Darwin's network errno values, even when this fixture runs on Linux.
#undef EMSGSIZE
#undef EOPNOTSUPP
#undef ENETDOWN
#undef ENOBUFS
#undef ENOTCONN
#undef ESHUTDOWN
#undef ETIMEDOUT
#define EMSGSIZE 40
#define EOPNOTSUPP 45
#define ENETDOWN 50
#define ENOBUFS 55
#define ENOTCONN 57
#define ESHUTDOWN 58
#define ETIMEDOUT 60
using errno_t = int;
using user_addr_t = uintptr_t;
using task_t = void *;
using UInt32 = uint32_t;
using IOReturn = uint32_t;
constexpr IOReturn kIOReturnSuccess = 0, kIOReturnError = 0xe00002bc,
  kIOReturnNoMemory = 0xe00002bd, kIOReturnNotPermitted = 0xe00002e2,
  kIOReturnBadArgument = 0xe00002c2, kIOReturnUnsupported = 0xe00002c7,
  kIOReturnNotReadable = 0xe00002ce, kIOReturnNotWritable = 0xe00002cf;
enum { kIODirectionIn = 1, kIODirectionOut = 2 };
inline thread_local unsigned host_lock_depth;
inline unsigned host_copyin_errors, host_copyout_errors, host_alloc_failures;
inline unsigned host_max_allocation, host_wakes;
inline std::map<void *, size_t> host_allocations;
inline void *IOMallocData(size_t n) {
  if (host_alloc_failures) { host_alloc_failures--; return nullptr; }
  void *p = std::malloc(n); assert(p);
  std::memset(p, 0xa5, n); host_allocations[p] = n;
  host_max_allocation = std::max(host_max_allocation, static_cast<unsigned>(n));
  return p;
}
inline void *IOMallocZeroData(size_t n) { void *p = IOMallocData(n); if (p) std::memset(p, 0, n); return p; }
inline void IOFreeData(void *p, size_t n) {
  assert(host_allocations.at(p) == n); host_allocations.erase(p); std::free(p);
}
inline int copyin(user_addr_t p, void *out, size_t n) {
  assert(!host_lock_depth);
  if (host_copyin_errors) { host_copyin_errors--; return EFAULT; }
  if (!p && n) return EFAULT;
  std::memcpy(out, reinterpret_cast<void *>(p), n); return 0;
}
inline int copyout(const void *in, user_addr_t p, size_t n) {
  assert(!host_lock_depth);
  if (host_copyout_errors) { host_copyout_errors--; return EFAULT; }
  if (!p && n) return EFAULT;
  std::memcpy(reinterpret_cast<void *>(p), in, n); return 0;
}
inline size_t strlcpy(char *out, const char *in, size_t n) {
  const size_t length = std::strlen(in);
  if (n) { const size_t take = std::min(length, n - 1); std::memcpy(out, in, take); out[take] = 0; }
  return length;
}
struct lck_grp_t {};
struct lck_mtx_t { std::mutex mutex; };
#define LCK_GRP_ATTR_NULL nullptr
#define LCK_ATTR_NULL nullptr
inline lck_grp_t *lck_grp_alloc_init(const char *, void *) { return new lck_grp_t; }
inline lck_mtx_t *lck_mtx_alloc_init(lck_grp_t *, void *) { return new lck_mtx_t; }
inline void lck_mtx_lock(lck_mtx_t *lock) { assert(!host_lock_depth); lock->mutex.lock(); host_lock_depth++; }
inline void lck_mtx_unlock(lck_mtx_t *lock) { assert(host_lock_depth == 1); host_lock_depth--; lock->mutex.unlock(); }
using thread_call_param_t = void *;
struct HostThreadCall {
  void (*callback)(thread_call_param_t, thread_call_param_t);
  void *argument; bool pending = false; uint64_t at = 0;
};
using thread_call_t = HostThreadCall *;
inline uint64_t host_now;
inline std::vector<thread_call_t> host_calls;
inline thread_call_t thread_call_allocate(void (*fn)(void *, void *), void *arg) {
  auto call = new HostThreadCall{fn, arg}; host_calls.push_back(call); return call;
}
inline bool thread_call_enter(thread_call_t call) { const bool old = call->pending; call->pending = true; call->at = host_now; return old; }
inline bool thread_call_enter_delayed(thread_call_t call, uint64_t at) { const bool old = call->pending; call->pending = true; call->at = at; return old; }
inline bool thread_call_cancel(thread_call_t call) { const bool old = call->pending; call->pending = false; return old; }
constexpr unsigned kMillisecondScale = 1000000;
inline void clock_interval_to_deadline(unsigned ms, unsigned, uint64_t *at) { *at = host_now + ms; }
inline void host_deliver_calls() {
  assert(!host_lock_depth);
  for (unsigned pass = 0; pass < 16; pass++) {
    bool work = false;
    for (auto call : host_calls) if (call->pending && call->at <= host_now) {
      call->pending = false; call->callback(call->argument, nullptr); work = true;
    }
    if (!work) return;
  }
  assert(false && "unbounded deferred notifications");
}

struct IOMemoryDescriptor {
  uint8_t *data; uint64_t length; bool wired = false;
  unsigned prepares = 0, completes = 0, direction = 0;
  IOReturn prepare_result = 0, complete_result = 0;
  bool short_read = false, short_write = false;
  uint64_t getLength() const { return length; }
  IOReturn prepare(unsigned dir) {
    assert(!host_lock_depth && !wired); prepares++; direction = dir;
    if (!prepare_result) wired = true; return prepare_result;
  }
  IOReturn complete(unsigned dir) {
    assert(!host_lock_depth && wired && direction == dir); completes++; wired = false; return complete_result;
  }
  uint64_t readBytes(uint64_t at, void *out, uint64_t n) {
    assert(!host_lock_depth && wired && direction == kIODirectionOut && at + n <= length);
    const uint64_t take = short_read && n ? n - 1 : n; std::memcpy(out, data + at, take); return take;
  }
  uint64_t writeBytes(uint64_t at, const void *in, uint64_t n) {
    assert(!host_lock_depth && wired && direction == kIODirectionIn && at + n <= length);
    const uint64_t take = short_write && n ? n - 1 : n; std::memcpy(data + at, in, take); return take;
  }
};
struct IOExternalMethodArguments {
  uint32_t scalarInputCount = 0, scalarOutputCount = 0;
  const void *structureInput = nullptr; uint32_t structureInputSize = 0;
  IOMemoryDescriptor *structureInputDescriptor = nullptr;
  void *structureOutput = nullptr; uint32_t structureOutputSize = 0;
  IOMemoryDescriptor *structureOutputDescriptor = nullptr; uint64_t structureOutputDescriptorSize = 0;
};
struct IOExternalMethodDispatch {};
class OSSerialize;
class OSObject {
  unsigned refs = 1;
public:
  virtual ~OSObject() = default;
  void retain() { refs++; }
  void release() { if (!--refs) delete this; }
  virtual bool serialize(OSSerialize *) { return false; }
};
class OSDictionary : public OSObject {};
class IOUserClient;
class IOService : public OSObject {
public:
  virtual bool init() { return true; }
  virtual bool attach(IOService *) { return true; }
  virtual bool start(IOService *) { return true; }
  virtual void detach(IOService *) {}
  virtual void registerService() {}
  virtual void setName(const char *) {}
  virtual IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **) { return kIOReturnUnsupported; }
  template<class T> void setProperty(const char *, T) {}
  static IOService *getPlatform() { static IOService platform; return &platform; }
};
class IOUserClient : public IOService {
public:
  virtual bool initWithTask(task_t, void *, UInt32, OSDictionary * = nullptr) { return true; }
  virtual IOReturn clientClose() { return 0; }
  virtual IOReturn externalMethod(uint32_t, IOExternalMethodArguments *, IOExternalMethodDispatch *, OSObject *, void *) { return kIOReturnUnsupported; }
  virtual IOReturn setProperties(OSObject *) { return kIOReturnUnsupported; }
  void terminate() {}
};
class OSData : public OSObject {
public: static OSData *withBytes(const void *, size_t) { return new OSData; }
};
class OSNumber : public OSObject {
public: static OSNumber *withNumber(unsigned long long, unsigned) { return new OSNumber; }
};
class OSSerialize : public OSObject {
public: static OSSerialize *withCapacity(unsigned) { return new OSSerialize; }
  const char *text() { return "host fixture"; }
};
#define OSDeclareDefaultStructors(T) public: T() = default; ~T() override = default
#define OSDefineMetaClassAndStructors(T, P)
#define OSTypeAlloc(T) new T
#define OSSafeReleaseNULL(p) do { if (p) { (p)->release(); (p) = nullptr; } } while (0)
#define APPLE_KEXT_OVERRIDE override

struct net_event_data { uint32_t if_family, if_unit; char if_name[16]; };
struct kev_msg {
  uint32_t vendor_code, kev_class, kev_subclass, event_code;
  struct { uint32_t data_length; void *data_ptr; } dv[5];
};
enum { KEV_VENDOR_APPLE = 1, KEV_NETWORK_CLASS = 1, KEV_DL_SUBCLASS = 2,
       KEV_DL_LINK_ON = 16, KEV_DL_LINK_OFF = 17 };
struct HostEvent { uint32_t klass, subclass, code; std::vector<uint8_t> payload; };
inline std::vector<HostEvent> host_events;
inline int host_event_error;
inline std::function<void()> host_event_hook;

struct HostIfnet;
using ifnet_t = HostIfnet *;
struct HostMbuf {
  std::vector<uint8_t> storage; size_t offset = 0, len = 0, packet_length = 0;
  HostMbuf *next = nullptr; void *header = nullptr; ifnet_t rcvif = nullptr;
};
using mbuf_t = HostMbuf *;
inline size_t host_cluster_size = 512;
inline unsigned host_mbuf_failures, host_mbuf_count;
inline errno_t host_input_error;
inline std::vector<uint8_t> host_bpf_rx, host_bpf_tx, host_input_payload;
enum { MBUF_DONTWAIT = 0, DLT_EN10MB = 1 };
inline errno_t mbuf_allocpacket(unsigned, size_t n, void *, mbuf_t *out) {
  if (host_mbuf_failures) { host_mbuf_failures--; return ENOBUFS; }
  mbuf_t *tail = out;
  while (n) { const size_t take = std::min(n, host_cluster_size); *tail = new HostMbuf; (*tail)->storage.resize(take); host_mbuf_count++; tail = &(*tail)->next; n -= take; }
  return 0;
}
inline mbuf_t mbuf_next(mbuf_t m) { return m->next; }
inline size_t mbuf_maxlen(mbuf_t m) { return m->storage.size(); }
inline size_t mbuf_leadingspace(mbuf_t m) { return m->offset; }
inline void *mbuf_datastart(mbuf_t m) { return m->storage.data(); }
inline void *mbuf_data(mbuf_t m) { return m->storage.data() + m->offset; }
inline void mbuf_setdata(mbuf_t m, void *p, size_t len) { m->offset = static_cast<uint8_t *>(p) - m->storage.data(); m->len = len; assert(m->offset + len <= m->storage.size()); }
inline void mbuf_setlen(mbuf_t m, size_t len) { assert(m->offset + len <= m->storage.size()); m->len = len; }
inline size_t mbuf_pkthdr_len(mbuf_t m) { return m->packet_length; }
inline void mbuf_pkthdr_setlen(mbuf_t m, size_t n) { m->packet_length = n; }
inline void mbuf_pkthdr_setrcvif(mbuf_t m, ifnet_t ifp) { m->rcvif = ifp; }
inline void mbuf_pkthdr_setheader(mbuf_t m, void *p) { m->header = p; }
inline void mbuf_adj(mbuf_t m, size_t n) { assert(m->len >= n); m->offset += n; m->len -= n; m->packet_length -= n; }
inline void mbuf_freem(mbuf_t m) { while (m) { auto next = m->next; delete m; host_mbuf_count--; m = next; } }
inline errno_t mbuf_copydata(mbuf_t m, size_t offset, size_t n, void *out) {
  auto bytes = static_cast<uint8_t *>(out);
  while (m && n) {
    if (offset >= m->len) { offset -= m->len; m = m->next; continue; }
    const size_t take = std::min(n, m->len - offset); std::memcpy(bytes, static_cast<uint8_t *>(mbuf_data(m)) + offset, take); bytes += take; n -= take; offset = 0; m = m->next;
  }
  return n ? EMSGSIZE : 0;
}
inline std::vector<uint8_t> host_mbuf_bytes(mbuf_t m) { std::vector<uint8_t> data(mbuf_pkthdr_len(m)); assert(!mbuf_copydata(m, 0, data.size(), data.data())); return data; }
struct ifnet_stat_increment_param { uint32_t packets_in, bytes_in; };
struct ifnet_stats_param { uint64_t packets_in = 0, packets_out = 0, bytes_in = 0, bytes_out = 0, errors_out = 0; };
struct HostIfnet { uint32_t flags = 0; ifnet_stats_param stats; };
inline errno_t ifnet_set_flags(ifnet_t p, uint32_t flags, uint32_t mask) { p->flags = (p->flags & ~mask) | (flags & mask); return 0; }
inline void ifnet_stat_increment_out(ifnet_t p, uint32_t packets, uint32_t bytes, uint32_t errors) { p->stats.packets_out += packets; p->stats.bytes_out += bytes; p->stats.errors_out += errors; }
inline errno_t ifnet_input(ifnet_t p, mbuf_t m, ifnet_stat_increment_param *inc) {
  assert(m->rcvif == p && m->header); host_input_payload = host_mbuf_bytes(m); mbuf_freem(m);
  if (!host_input_error) { p->stats.packets_in += inc->packets_in; p->stats.bytes_in += inc->bytes_in; } return host_input_error;
}
inline void bpf_tap_in(ifnet_t, unsigned, mbuf_t m, void *, unsigned) { host_bpf_rx = host_mbuf_bytes(m); }
inline void bpf_tap_out(ifnet_t, unsigned, mbuf_t m, void *, unsigned) { host_bpf_tx = host_mbuf_bytes(m); }
inline errno_t bpf_attach(ifnet_t, unsigned, unsigned, void *, void *) { return 0; }
enum { IFF_UP = 1, IFF_BROADCAST = 2, IFF_RUNNING = 0x40, IFF_SIMPLEX = 0x800, IFF_MULTICAST = 0x8000,
  IFT_ETHER = 6, IFNET_FAMILY_ETHERNET = 1, IFNET_SUBFAMILY_WIFI = 1,
  IFNET_INIT_CURRENT_VERSION = 1, IFNET_INIT_LEGACY = 1, IFNET_INIT_NX_NOAUTO = 2, IFEF_ENQUEUE_MULTI = 1,
  IFM_IEEE80211 = 0x80, IFM_AUTO = 0, IFM_AVALID = 1, IFM_ACTIVE = 2 };
constexpr unsigned long SIOCGIFMEDIA = 1, SIOCSIFFLAGS = 2, SIOCSIFMTU = 3,
  SIOCADDMULTI = 4, SIOCDELMULTI = 5, SIOCSIFADDR = 6, SIOCSIFMEDIA = 7;
struct ifmediareq { int ifm_current, ifm_mask, ifm_status, ifm_active, ifm_count; int *ifm_ulist; };
inline int ether_demux() { return 0; }
inline int ether_add_proto() { return 0; }
inline int ether_del_proto() { return 0; }
inline int ether_check_multi() { return 0; }
inline int ether_frameout() { return 0; }
struct ifnet_init_eparams {
  unsigned ver, len, flags, unit, family, subfamily, type; const char *name;
  errno_t (*output)(ifnet_t, mbuf_t);
  int (*demux)(), (*add_proto)(), (*del_proto)(), (*check_multi)(), (*framer)();
  errno_t (*ioctl)(ifnet_t, unsigned long, void *); void (*event)(ifnet_t, const kev_msg *);
  const uint8_t *broadcast_addr; unsigned broadcast_len;
};
inline errno_t ifnet_allocate_extended(ifnet_init_eparams *, ifnet_t *out) { *out = new HostIfnet; return 0; }
inline errno_t ifnet_attach(ifnet_t, void *) { return 0; }
inline void ifnet_release(ifnet_t p) { delete p; }
inline void ifnet_set_mtu(ifnet_t, unsigned) {}
inline void ifnet_set_baudrate(ifnet_t, uint64_t) {}
inline void ifnet_set_hdrlen(ifnet_t, unsigned) {}
inline void ifnet_set_addrlen(ifnet_t, unsigned) {}
inline void ifnet_set_eflags(ifnet_t, unsigned, unsigned) {}
inline void ifnet_set_lladdr(ifnet_t, const uint8_t *, unsigned) {}
inline unsigned ifnet_capabilities_enabled(ifnet_t) { return 0; }
struct HostIfaddr {};
using ifaddr_t = HostIfaddr *;
inline int ifnet_get_address_list_family(ifnet_t, ifaddr_t **out, unsigned) { *out = nullptr; return 0; }
inline int ifaddr_address(ifaddr_t, sockaddr *, size_t) { return ENOENT; }
inline void ifnet_free_address_list(ifaddr_t *) {}
inline void ifnet_stat(ifnet_t p, ifnet_stats_param *out) { *out = p->stats; }
#endif
