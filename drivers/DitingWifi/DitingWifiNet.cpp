// SPDX-License-Identifier: BSD-2-Clause-Patent
// The WiFi module's network side: the Ethernet interface the system routes through, the registry entries by which the wireless daemon and
// the network configuration find a wireless interface, and the Apple80211 requests (ioctls on that interface) the daemon uses to scan,
// join and read state. Nothing here talks to the radio: it calls the module's station interface (nw_abi.h) and turns what it answers into
// the structures the system's wireless framework reads.
//
// Layouts below are those the framework reads and writes; where a field's meaning is not known it is left zero. Every request is written
// to the log (type, direction, length) and a request that is not handled is refused, so which ones the framework needs shows in the log.
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOUserClient.h>
#include <libkern/OSAtomic.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSerialize.h>
#include <sys/kern_event.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_media.h>
#include <net/if_ether.h>
#include <net/kpi_interface.h>
extern "C" {
#include <net/bpf.h>
}
#include <net/if_types.h>
#include <netinet/in.h>
#if defined(DITING_GUEST_24B156) && defined(XNU_KERNEL_PRIVATE)
#undef XNU_KERNEL_PRIVATE
#include <sys/mbuf.h>
#define XNU_KERNEL_PRIVATE 1
#else
#include <sys/mbuf.h>
#endif
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <sys/systm.h>
#include <stdarg.h>
#include "nw_abi.h"
#include "DitingWifiProtocol27.h"

extern "C" unsigned nw_wifi_channel_list(unsigned *, unsigned);
extern "C" void native_wifi_logf(int level, const char *fmt, ...);
extern "C" void diting_wifi_wake(void);   // WifiProbe.cpp: the WiFi thread sends the queued frame now
extern "C" void proc_selfname(char *buf, int size);
extern "C" void proc_name(int pid, char *buf, int size);
extern "C" struct proc *proc_find(int pid);
extern "C" int proc_rele(struct proc *p);
extern "C" task_t proc_task(struct proc *p);
extern "C" void diting_dump_task_threads(const char *name, int pid, task_t task, int with_bt) __attribute__((weak_import));
extern "C" void diting_wifi_hw_diag(uint32_t *msi, uint32_t *blocksUsed, uint32_t *slabUsed, uint32_t *slabTotal,
                                    int *paKind, uint32_t *paValid, uint64_t *hostData,
                                    const char **bdfName, const char **regdbName) __attribute__((weak_import));
extern "C" {
extern volatile int diting_software_aes_state __attribute__((weak_import));
extern volatile uint32_t native_aes_kat_status __attribute__((weak_import));
extern volatile uint32_t native_aes_registered __attribute__((weak_import));
extern volatile uint32_t native_aes_completed __attribute__((weak_import));
extern volatile uint32_t native_aes_nonce_key_uses __attribute__((weak_import));
extern volatile uint32_t native_aes_unsupported_key __attribute__((weak_import));
}

#define SIOCSA80211 0x802869c8UL   // _IOW('i', 200, struct apple80211req)
#define SIOCGA80211 0xc02869c9UL   // _IOWR('i', 201, struct apple80211req)

// (the header declares it without C linkage; the symbol is the C one)
extern "C" int diting_kev_post(struct kev_msg *) __asm__("_kev_post_msg");

namespace {

struct apple80211req {
  char req_if_name[16];
  int32_t req_type;
  int32_t req_val;
  uint32_t req_len;
  user_addr_t req_data __attribute__((aligned(8)));
};

// request types
enum {
  kSsid = 1, kAuthType = 2, kCipherKey = 3, kChannel = 4, kPowerSave = 5, kTxPower = 7, kRate = 8, kBssid = 9, kScanReq = 10, kScanResult = 11,
  kCardCaps = 12, kState = 13, kPhyMode = 14, kOpMode = 15, kRssi = 16, kNoise = 17, kPower = 19, kAssociate = 20, kAssociateResult = 21,
  kDisassociate = 22, kSupportedChannels = 27, kLocale = 28, kDeauth = 29, kDriverVersion = 43, kHardwareVersion = 44, kCountryCode = 51,
  kRadioInfo = 54, kWowEnabled = 70, kVirtualRole = 96, kVirtualParent = 97, kScanReqMultiple = 86, kScanCacheClear = 90, kCurrentNetwork = 103, kChannelsInfo = 207, kHwSupportedChannels = 254,
  kAbortScan = 382,
};
// events
enum { kEvPowerChanged = 1, kEvSsidChanged = 2, kEvBssidChanged = 3, kEvLinkChanged = 4, kEvAssocDone = 9, kEvScanDone = 10 };
// results of an association, as the framework numbers them
enum { kAssocPending = 0, kAssocOk = 1, kAssocFailed = 2, kAssocTimeout = 9, kAssocBadKey = 21 };

using channel = diting_wifi_channel;
struct channels_list { uint32_t version, count; channel list[401]; uint32_t unused; };           // 4824 bytes
using scan_result = diting_wifi_scan_result;                                                      // 0x8e8 (2280) bytes
using assoc_event27 = diting_wifi_assoc_event27;                                                  // 540 bytes
struct link_event { uint8_t down, unused0[3]; int32_t rssi; uint16_t snr, noise; uint8_t unused1[4]; uint8_t involuntary, unused2[3]; uint32_t reason, subreason; uint8_t unused3[4]; };
static_assert(sizeof(channels_list) == 4824, "channel list layout");
static_assert(sizeof(link_event) == 32, "link event layout");

ifnet_t gIf;
uint8_t gMac[6];
lck_grp_t *gGrp;
lck_mtx_t *gLock;
bool gPower = true;
bool gFailed;
bool gLinkSuppressed;
bool gKeyed;   // the last join used a passphrase
uint32_t gRxFrames, gRxErrors; int gRxLastError;
uint32_t gRxNoBuffer, gRxChains, gRxLongest;   // frames dropped for want of a buffer, frames that took a chain, the longest frame
struct { unsigned count, kicks, cleared, longest; bool kicked; } gStall;   // input thread stalls (check_input)
int gAssocResult = kAssocOk;
bool gAssocModern;
bool gJoinPending;
unsigned gScanCursor, gScanning;
thread_call_t gNotifications;
unsigned gPendingNotifications;
enum { NotifyPower=1, NotifyLinkDown=2 };
bool gUserJoin;   // the wireless daemon joined, left or switched the radio itself: the unattended test join steps aside
uint32_t gLogged[1024 / 32];

// Live Apple80211 / event telemetry for automated USB diagnostics.
struct IoctlErrorRec {
  int32_t type;
  uint32_t len;
  int32_t rc;
  uint32_t count;
  uint8_t isSet;
  uint8_t isUc;
};
struct IoctlTraceRec {
  char proc[16];
  int32_t type;
  uint32_t len;
  int32_t rc;
  uint32_t count;
  uint8_t isSet;
  uint8_t isUc;
};
struct WifiDiagStats {
  uint32_t total, uc, bsd;
  uint32_t caps, scanReq, scanMulti, scanRes, assoc, disassoc, powerSet, abortScan;
  uint32_t eventsOk, eventsErr;
  int32_t lastEventErr;
  uint32_t ucRefused;
  uint32_t lastRefusedSel, lastRefusedIn, lastRefusedOut;
  uint32_t ucClientsOpened;
  uint32_t ucClientProcCount;
  char ucClientProcs[8][16];
  uint32_t stackOpened;
  uint32_t stackCmds;
  char lastStackCmd[128];
  uint32_t ifMedia, ifFlags, ifOther;
  uint32_t ifMediaProcCount;
  char ifMediaProcs[8][16];
  uint32_t errCount;
  IoctlErrorRec errs[16];
  uint32_t traceCount;
  IoctlTraceRec traces[32];
} gDiag;

void record_ifmedia_caller() {
  char name[16] = {};
  proc_selfname(name, sizeof name);
  if (!name[0]) strlcpy(name, "?", sizeof name);
  gDiag.ifMedia++;
  for (uint32_t i = 0; i < gDiag.ifMediaProcCount; ++i) {
    if (!strncmp(gDiag.ifMediaProcs[i], name, sizeof gDiag.ifMediaProcs[i])) return;
  }
  if (gDiag.ifMediaProcCount < 8) {
    strlcpy(gDiag.ifMediaProcs[gDiag.ifMediaProcCount++], name, 16);
  }
}

void record_uc_opener() {
  char name[16] = {};
  proc_selfname(name, sizeof name);
  if (!name[0]) strlcpy(name, "?", sizeof name);
  gDiag.ucClientsOpened++;
  for (uint32_t i = 0; i < gDiag.ucClientProcCount; ++i) {
    if (!strncmp(gDiag.ucClientProcs[i], name, sizeof gDiag.ucClientProcs[i])) return;
  }
  if (gDiag.ucClientProcCount < 8) {
    strlcpy(gDiag.ucClientProcs[gDiag.ucClientProcCount++], name, 16);
  }
}

void record_ioctl_diag(int type, bool set, bool uc, uint32_t len, int rc) {
  char name[16] = {};
  proc_selfname(name, sizeof name);
  if (!name[0]) strlcpy(name, "?", sizeof name);
  gDiag.total++;
  if (uc) gDiag.uc++; else gDiag.bsd++;
  if (!set && type == kCardCaps) gDiag.caps++;
  else if (set && type == kScanReq) gDiag.scanReq++;
  else if (set && type == kScanReqMultiple) gDiag.scanMulti++;
  else if (!set && type == kScanResult) gDiag.scanRes++;
  else if (set && type == kAssociate) gDiag.assoc++;
  else if (set && (type == kDisassociate || type == kDeauth)) gDiag.disassoc++;
  else if (set && type == kPower) gDiag.powerSet++;
  else if (set && type == kAbortScan) gDiag.abortScan++;
  bool foundTrace = false;
  for (uint32_t i = 0; i < gDiag.traceCount; ++i) {
    auto &t = gDiag.traces[i];
    if (t.type == type && t.isSet == (set ? 1 : 0) && t.isUc == (uc ? 1 : 0) && t.rc == rc &&
        !strncmp(t.proc, name, sizeof t.proc)) {
      t.count++;
      t.len = len;
      foundTrace = true;
      break;
    }
  }
  if (!foundTrace && gDiag.traceCount < 32) {
    auto &t = gDiag.traces[gDiag.traceCount++];
    memset(&t, 0, sizeof t);
    strlcpy(t.proc, name, sizeof t.proc);
    t.type = type; t.len = len; t.rc = rc; t.count = 1u;
    t.isSet = uint8_t(set ? 1 : 0); t.isUc = uint8_t(uc ? 1 : 0);
  }
  if (rc && rc != ENOENT && !(type == kScanResult && rc == EIO)) {
    for (uint32_t i = 0; i < gDiag.errCount; ++i) {
      if (gDiag.errs[i].type == type && gDiag.errs[i].isSet == (set ? 1 : 0) && gDiag.errs[i].rc == rc) {
        gDiag.errs[i].count++;
        gDiag.errs[i].len = len;
        return;
      }
    }
    if (gDiag.errCount < 16) {
      gDiag.errs[gDiag.errCount++] = {type, len, rc, 1u, uint8_t(set ? 1 : 0), uint8_t(uc ? 1 : 0)};
    }
  }
}

void log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void log(int level, const char *fmt, ...) {
  char line[160]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
  native_wifi_logf(level, "%s", line);
}

// ---- events to the framework: class IEEE 802.11 (6), subclass 1, data starting with the interface name ----
int post_event(uint32_t code, const void *payload, size_t length) {
  char name[16]; memset(name, 0, sizeof name); snprintf(name, sizeof name, "en0");
  struct kev_msg msg; memset(&msg, 0, sizeof msg);
  msg.vendor_code = KEV_VENDOR_APPLE; msg.kev_class = 6; msg.kev_subclass = 1; msg.event_code = code;
  msg.dv[0].data_length = sizeof name; msg.dv[0].data_ptr = name;
  if (length && payload) {
    msg.dv[1].data_length = static_cast<uint32_t>(length);
    msg.dv[1].data_ptr = const_cast<void *>(payload);
  }
  const int error = diting_kev_post(&msg);
  if (error) {
    gDiag.eventsErr++;
    gDiag.lastEventErr = error;
    log(4, "wifi-net: event %u bytes %u -> err %d", code, static_cast<unsigned>(length), error);
  } else {
    gDiag.eventsOk++;
  }
  return error;
}
// link state for the network configuration: the interface's flags and the stack's own link events
void post_link(bool up) {
  if (!gIf) return;
  ifnet_set_flags(gIf, up ? IFF_RUNNING : 0, IFF_RUNNING);
  struct { struct net_event_data data; } payload; memset(&payload, 0, sizeof payload);
  payload.data.if_family = IFNET_FAMILY_ETHERNET; payload.data.if_unit = 0; snprintf(payload.data.if_name, sizeof payload.data.if_name, "en");
  struct kev_msg msg; memset(&msg, 0, sizeof msg);
  msg.vendor_code = KEV_VENDOR_APPLE; msg.kev_class = KEV_NETWORK_CLASS; msg.kev_subclass = KEV_DL_SUBCLASS; msg.event_code = up ? KEV_DL_LINK_ON : KEV_DL_LINK_OFF;
  msg.dv[0].data_length = sizeof payload; msg.dv[0].data_ptr = &payload;
  diting_kev_post(&msg);
}

void post_association_result() {
#ifdef DITING_GUEST_24B156
  const bool modern = true;
#else
  const bool modern = gAssocModern;
#endif
  if (modern) {
    assoc_event27 event{}; event.result = static_cast<uint32_t>(gAssocResult); event.has_result = 1;
    nw_wifi_status current; nw_wifi_status_get(&current);
    memcpy(event.attempts[0], current.bssid, 6);
    (void)post_event(kEvAssocDone, &event, sizeof event);
  } else {
    (void)post_event(kEvAssocDone, nullptr, 0);
  }
}

void visible_status(nw_wifi_status &st) {
  nw_wifi_status_get(&st);
  if (!gPower || gLinkSuppressed) {
    st.state = NW_STA_IDLE; st.ssid_len = 0; memset(st.ssid, 0, sizeof st.ssid);
    memset(st.bssid, 0, sizeof st.bssid); st.freq_mhz = 0; st.signal_dbm = 0;
  }
}

// Apple80211 commands own gLock. Network/socket event delivery may reenter
// interface queries and must run after that lock has been released. Keep
// only coalescible state notifications here; scan/join completions retain
// their existing explicit callbacks outside gLock.
void deliver_notifications(thread_call_param_t,thread_call_param_t) {
  for(;;) {
    lck_mtx_lock(gLock);
    const unsigned pending=gPendingNotifications;gPendingNotifications=0;
    lck_mtx_unlock(gLock);
    if(!pending)return;
    if(pending&NotifyLinkDown) {
      post_link(false);
      link_event ev{}; ev.down = 1;
      (void)post_event(kEvLinkChanged, &ev, sizeof ev);
      (void)post_event(kEvSsidChanged, nullptr, 0);
      (void)post_event(kEvBssidChanged, nullptr, 0);
    }
    if(pending&NotifyPower) (void)post_event(kEvPowerChanged,nullptr,0);
  }
}
void notify_locked(unsigned events) {
  gPendingNotifications|=events;thread_call_enter(gNotifications);
}

// ---- what the module tells us ----
void on_scan_done() {
  lck_mtx_lock(gLock); gScanning = 0; gScanCursor = 0; lck_mtx_unlock(gLock);
  (void)post_event(kEvScanDone, nullptr, 0);
}
// A scan answered from the cache finishes after the time a short scan takes: answered at once, the daemon asks again straight away.
thread_call_t gCachedScan;
void cached_scan_done(thread_call_param_t, thread_call_param_t) { on_scan_done(); }
void on_connect_done(int result) {
  lck_mtx_lock(gLock);
  const bool suppressed = !gPower || gLinkSuppressed;
  if (!suppressed) {
    gAssocResult = result == NW_RES_OK ? kAssocOk : result == NW_RES_BAD_KEY ? kAssocBadKey : result == NW_RES_TIMEOUT ? kAssocTimeout : kAssocFailed;
  }
  const bool pending = __atomic_exchange_n(&gJoinPending, false, __ATOMIC_ACQ_REL);
  lck_mtx_unlock(gLock);
  if (suppressed) {
    log(6, "wifi-net: ignore join completion after disconnect: %d", result);
    return;
  }
  log(6, "wifi-net: join finished: module result %d (assoc=%d)", result, gAssocResult);
  post_link(result == NW_RES_OK);
  if (pending || result == NW_RES_OK) post_association_result();
  if (result == NW_RES_OK) {
    (void)post_event(kEvSsidChanged, nullptr, 0); (void)post_event(kEvBssidChanged, nullptr, 0);
    nw_wifi_status st; nw_wifi_status_get(&st);
    link_event ev; memset(&ev, 0, sizeof ev); ev.rssi = st.signal_dbm; ev.noise = static_cast<uint16_t>(-95);
    ev.snr = static_cast<uint16_t>(st.signal_dbm > -95 ? st.signal_dbm + 95 : 0);
    (void)post_event(kEvLinkChanged, &ev, sizeof ev);
  }
}
void on_link_lost(int reason) {
  log(6, "wifi-net: link lost: reason %d", reason);
  gAssocResult = kAssocFailed;
  post_link(false);
  link_event ev; memset(&ev, 0, sizeof ev); ev.down = 1; ev.involuntary = 1; ev.reason = static_cast<uint32_t>(reason);
  (void)post_event(kEvLinkChanged, &ev, sizeof ev);
  (void)post_event(kEvSsidChanged, nullptr, 0);
  (void)post_event(kEvBssidChanged, nullptr, 0);
}
// What the address configuration exchanges, as it crosses the interface: DHCP messages (type, transaction, offered address) and ARP, the first
// ones of each direction, and per-kind counts for the traffic line.
struct Kinds { uint32_t arp, ip4, dhcp, ip6, other; } gKindsIn, gKindsOut;
struct ArpRec { uint8_t out, op, spa[4], tpa[4], src[6], dst[6]; };
ArpRec gArpRing[8]; uint32_t gArpRingSeq;
void note_frame(const uint8_t *e, size_t n, bool out) {
  Kinds &k = out ? gKindsOut : gKindsIn; const unsigned type = n >= 14 ? (e[12] << 8 | e[13]) : 0; const char *dir = out ? "out" : "in";
  if (type == 0x0806) {
    k.arp++; static unsigned shown[2];
    if (n >= 42) {
      ArpRec &rec = gArpRing[__atomic_fetch_add(&gArpRingSeq, 1u, __ATOMIC_RELAXED) & 7u];
      rec.out = out ? 1u : 0u; rec.op = e[21];
      memcpy(rec.spa, e + 28, 4); memcpy(rec.tpa, e + 38, 4);
      memcpy(rec.src, e + 6, 6); memcpy(rec.dst, e + 0, 6);
    }
    if (n >= 42 && shown[out]++ < 12) log(6, "wifi-net: arp %s op %u %u.%u.%u.%u asks %u.%u.%u.%u", dir, e[21], e[28], e[29], e[30], e[31], e[38], e[39], e[40], e[41]);
    return; }
  if (type == 0x86dd) { k.ip6++; return; }
  if (type != 0x0800 || n < 34) { k.other++; return; }
  k.ip4++; const uint8_t *ip = e + 14; const unsigned ihl = (ip[0] & 15) * 4;
  if (ip[9] != 17 || n < 14 + ihl + 8) return;
  const uint8_t *udp = ip + ihl; const unsigned sp = udp[0] << 8 | udp[1], dp = udp[2] << 8 | udp[3];
  if (!((sp == 67 || sp == 68) && (dp == 67 || dp == 68))) return;
  k.dhcp++; const uint8_t *b = udp + 8; const size_t bl = n - (b - e); unsigned msg = 0;
  for (size_t o = 240; bl >= 240 && o + 2 <= bl && b[o] != 255; o += b[o] ? 2 + b[o + 1] : 1) if (b[o] == 53 && o + 2 < bl) { msg = b[o + 2]; break; }
  static unsigned shown[2]; if (shown[out]++ < 24 && bl >= 240)
    log(6, "wifi-net: dhcp %s type %u xid %02x%02x%02x%02x flags %02x%02x to %02x:%02x:%02x:%02x:%02x:%02x yiaddr %u.%u.%u.%u", dir, msg, b[4], b[5], b[6], b[7], b[10], b[11],
        e[0], e[1], e[2], e[3], e[4], e[5], b[16], b[17], b[18], b[19]);
  // An offer or acknowledgement: the network settings the server hands out (mask, router, name servers, lease), to tell a server that
  // gives no IPv4 name server from a client that drops one.
  if (out || (msg != 2 && msg != 5)) return;
  char opts[124] = {}; int at = 0;   // fits the 160-byte log line
  for (size_t o = 240; o + 2 <= bl && b[o] != 255 && at >= 0 && at < static_cast<int>(sizeof opts) - 24; o += b[o] ? 2 + b[o + 1] : 1) {
    if (!b[o]) continue;
    const unsigned code = b[o], len = b[o + 1]; const uint8_t *v = b + o + 2;
    if (o + 2 + len > bl) break;
    if ((code == 1 || code == 3 || code == 6 || code == 54) && len >= 4)
      for (unsigned i = 0; i + 4 <= len && at < static_cast<int>(sizeof opts) - 24; i += 4)
        at += snprintf(opts + at, sizeof opts - at, " %u=%u.%u.%u.%u", code, v[i], v[i + 1], v[i + 2], v[i + 3]);
    else if (code == 51 && len == 4) at += snprintf(opts + at, sizeof opts - at, " 51=%u", v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3]);
    else at += snprintf(opts + at, sizeof opts - at, " %u", code);
  }
  log(6, "wifi-net: dhcp %s type %u options%s", dir, msg, opts);
}
void on_rx(const uint8_t *eth, size_t length) {
  if (!gIf || length < 14) return;
  note_frame(eth, length, false);
  // Two bytes ahead of the Ethernet header put the IP header on a 4-byte boundary. The packet may come back as a chain (a frame larger
  // than one cluster, or no large cluster free): each buffer gets only what fits in it. Setting the whole length on the first one (as
  // before) let the stack read past that buffer's end (i469: a data abort in m_copydata reading across a page).
  enum : size_t { Align = 2 };
  mbuf_t m = nullptr;
  if (mbuf_allocpacket(MBUF_DONTWAIT, length + Align, nullptr, &m) || !m) { gRxNoBuffer++; return; }
  if (length > gRxLongest) gRxLongest = static_cast<uint32_t>(length);
  if (mbuf_next(m)) gRxChains++;
  size_t done = 0; bool first = true;
  for (mbuf_t n = m; n; n = mbuf_next(n)) {
    size_t room = mbuf_maxlen(n) - mbuf_leadingspace(n);
    if (first) { if (room < Align + 14) break; mbuf_setdata(n, static_cast<uint8_t *>(mbuf_datastart(n)) + mbuf_leadingspace(n) + Align, 0); room -= Align; }
    const size_t take = length - done < room ? length - done : room;
    memcpy(mbuf_data(n), eth + done, take); mbuf_setlen(n, take); done += take; first = false;
  }
  if (done != length) { gRxNoBuffer++; mbuf_freem(m); return; }
  mbuf_pkthdr_setlen(m, length); mbuf_pkthdr_setrcvif(m, gIf);
  mbuf_pkthdr_setheader(m, mbuf_data(m)); mbuf_adj(m, 14);   // the stack wants the Ethernet header apart from the payload
  bpf_tap_in(gIf, DLT_EN10MB, m, const_cast<uint8_t *>(eth), 14);
  struct ifnet_stat_increment_param inc; memset(&inc, 0, sizeof inc); inc.packets_in = 1; inc.bytes_in = static_cast<uint32_t>(length);
  const errno_t e = ifnet_input(gIf, m, &inc);
  if (e) { gRxErrors++; gRxLastError = e; } else gRxFrames++;
}
const struct nw_wifi_events gEvents = { on_scan_done, on_connect_done, on_link_lost, on_rx };

// ---- the interface ----
errno_t if_output(ifnet_t, mbuf_t m) {
  size_t length = mbuf_pkthdr_len(m);
  if (length >= 14 && length <= 2000) {
    uint8_t frame[2000]; mbuf_copydata(m, 0, length, frame);
    note_frame(frame, length, true);
    if (nw_wifi_tx(frame, length)) log(6, "wifi-net: a frame was dropped (queue full)");
    else {
      bpf_tap_out(gIf, DLT_EN10MB, m, nullptr, 0);
      ifnet_stat_increment_out(gIf, 1, static_cast<uint32_t>(length), 0);
      diting_wifi_wake();
    }
  }
  mbuf_freem(m);
  return 0;
}
void if_event_stub(ifnet_t, const struct kev_msg *) {}

// ---- Apple80211 ----
void fill_channel(channel &c, uint32_t mhz) {
  if (diting_wifi_fill_channel(&c, mhz) != 0) {
    c.version = 1;
    c.channel = 0;
    c.flags = 0;
  }
}
void fill_result(scan_result &r, const nw_bss_info &b, uint32_t requestBytes) {
  memset(&r, 0, sizeof r); r.version = 1; fill_channel(r.ch, b.freq_mhz); r.noise_known = 1; r.noise = -95; r.rssi = b.signal_dbm;
  r.snr = b.signal_dbm > -95 ? b.signal_dbm + 95 : 0;
  r.beacon_interval = b.beacon_int; r.capability = b.caps; memcpy(r.bssid, b.bssid, 6); r.ssid_length = b.ssid_len; memcpy(r.ssid, b.ssid, 32);
  uint64_t now_ns = 0; absolutetime_to_nanoseconds(mach_continuous_time(), &now_ns);
  diting_wifi_scan_result_stamp(&r, b.ie, b.ie_len, b.age_ms, now_ns, requestBytes);
  r.rate_count = 0;
}
int copy_out(const apple80211req &rq, const void *src, size_t size, bool kernel = false) {
  if (rq.req_len == 0 || !rq.req_data) return EINVAL;
  const size_t n = rq.req_len < size ? rq.req_len : size;
  if (kernel) { memcpy(reinterpret_cast<void *>(rq.req_data), src, n); return 0; }
  return copyout(src, rq.req_data, n);
}
// A get that gives no buffer takes a single value in req_val; one with a buffer gets {version, value}.
int answer_value(apple80211req &rq, uint32_t value, bool kernel = false) {
  if (rq.req_len == 0 || !rq.req_data) { rq.req_val = static_cast<int32_t>(value); return 0; }
  const uint32_t w[2] = { 1, value }; return copy_out(rq, w, sizeof w, kernel);
}
// The raw bytes, their length in req_len.
int answer_bytes(apple80211req &rq, const void *src, uint32_t size, bool kernel = false) {
  if (!rq.req_data) return EINVAL;
  const uint32_t n = rq.req_len < size ? rq.req_len : size; rq.req_len = size;
  if (kernel) { memcpy(reinterpret_cast<void *>(rq.req_data), src, n); return 0; }
  return copyout(src, rq.req_data, n);
}
int copy_in(const apple80211req &rq, void *dst, size_t size, bool kernel = false) {
  if (!rq.req_data) return EINVAL;
  memset(dst, 0, size);
  const size_t n = rq.req_len < size ? rq.req_len : size;
  if (kernel) { memcpy(dst, reinterpret_cast<const void *>(rq.req_data), n); return 0; }
  return copyin(rq.req_data, dst, n);
}
unsigned gResultsShown;
// The regulatory country as the access points announce it (802.11d: the Country element, id 7, of the strongest network that has one), else "XX".
void country(char cc[3]) {
  cc[0] = 'X'; cc[1] = 'X'; cc[2] = 0; int best = -200; nw_bss_info b;
  for (int i = 0, n = nw_wifi_bss_count(); i < n; ++i) {
    if (nw_wifi_bss_get(i, &b) || b.signal_dbm <= best) continue;
    for (unsigned o = 0; o + 2 <= b.ie_len && o + 2 + b.ie[o + 1] <= b.ie_len; o += 2 + b.ie[o + 1])
      if (b.ie[o] == 7 && b.ie[o + 1] >= 2 && b.ie[o + 2] >= 'A' && b.ie[o + 2] <= 'Z' && b.ie[o + 3] >= 'A' && b.ie[o + 3] <= 'Z') {
        cc[0] = static_cast<char>(b.ie[o + 2]); cc[1] = static_cast<char>(b.ie[o + 3]); best = b.signal_dbm; break; }
  }
}
int map_state(unsigned s) { switch (s) { case NW_STA_SCANNING: return 1; case NW_STA_AUTH: return 2; case NW_STA_ASSOC: case NW_STA_KEYS: return 3; case NW_STA_UP: return 4; default: return 0; } }

int apple80211(bool set, apple80211req &rq, bool kernel = false) {
  if (gFailed && (set || rq.req_type != kPower)) return ENETDOWN;
  nw_wifi_status st; visible_status(st);
  const int type = rq.req_type;
  // (not on the stack: channels_list is 4824 B, kScanReqMultiple is 5352 B, scan_result is 2280 B)
  union Out { uint8_t raw[6144]; channels_list chans; scan_result res; };
  Out *outp = static_cast<Out *>(IOMallocData(sizeof(Out)));
  if (!outp) return ENOMEM;
  memset(outp, 0, sizeof(Out));
  struct Free { Out *p; ~Free() { IOFreeData(p, sizeof(Out)); } } freeOut{outp};
  Out &out = *outp;
  uint32_t *w = reinterpret_cast<uint32_t *>(out.raw);
  if (set) {
    switch (type) {
    case kPower: {
      uint32_t in[6]; if (rq.req_len < 12) return EINVAL;
      if (copy_in(rq, in, sizeof in, kernel)) return EFAULT;
      gUserJoin = true; gPower = in[2] != 0;
      log(6, "wifi-net: power set %u", in[2]);
      if (!gPower) {
        gLinkSuppressed = true;
        __atomic_store_n(&gJoinPending, false, __ATOMIC_RELEASE);
        nw_wifi_disconnect(); diting_wifi_wake();
        notify_locked(NotifyLinkDown | NotifyPower);
      } else {
        gLinkSuppressed = false;
        notify_locked(NotifyPower);
      }
      return 0; }
    case kScanReq: case kScanReqMultiple: {
      if (!gPower) return ENETDOWN;
      if (gLinkSuppressed) {
        nw_wifi_status current; nw_wifi_status_get(&current);
        if (current.state >= NW_STA_AUTH) return EBUSY;
      }
      uint32_t ssidLength = 0, scanType = 0;
      const uint8_t *ssid = nullptr;
      if (type == kScanReq) {
        if (rq.req_len < 20) return EINVAL;
        const size_t copied = rq.req_len < 0x80 ? rq.req_len : 0x80;
        if (copy_in(rq, out.raw, copied, kernel)) return EFAULT;
        memcpy(&ssidLength, out.raw + 16, 4); ssid = out.raw + 20;
        if (ssidLength > 32 || rq.req_len < 20 + ssidLength) return EINVAL;
        if (copied >= DITING_WIFI_SCAN_TYPE_SINGLE + 4) memcpy(&scanType, out.raw + DITING_WIFI_SCAN_TYPE_SINGLE, 4);
      } else {
        const size_t copied = rq.req_len < 0x240 ? rq.req_len : 0x240;
        if (copied < DITING_WIFI_SCAN_TYPE_MULTI + 4) return EINVAL;
        if (copy_in(rq, out.raw, copied, kernel)) return EFAULT;
        auto *names = reinterpret_cast<diting_wifi_scan_multiple *>(out.raw + 0x400);
        if (diting_wifi_scan_multiple_parse(out.raw, copied, names)) return EINVAL;
        scanType = names->scan_type;
        diting_wifi_scan_multiple_target(names, &ssid, &ssidLength);
      }
      { static unsigned said; if (said++ < 24) log(6, "wifi-net: scan req type=%d scanType=%u ssidLen=%u held=%d", type, scanType, ssidLength, nw_wifi_bss_count()); }
      const int held = nw_wifi_bss_count();
      uint32_t cachedDelayMs = 0;
      const auto plan = diting_wifi_plan_scan(scanType, st.state >= NW_STA_AUTH ? 1 : 0, held, &cachedDelayMs);
      if (plan == DITING_WIFI_SCAN_ACTION_CACHED_EVENT) {
        if (!gCachedScan) gCachedScan = thread_call_allocate(cached_scan_done, nullptr);
        if (!gCachedScan) return ENOMEM;
        if (st.state >= NW_STA_AUTH && scanType != DITING_WIFI_SCAN_TYPE_CACHED) {
          static unsigned answered;
          if (++answered <= 8 || answered % 64 == 0) {
            log(6, "wifi-net: scan %u while joined: answered from the cache", answered);
          }
        }
        gScanning = 1; gScanCursor = 0; gResultsShown = 24;
        uint64_t at; clock_interval_to_deadline(cachedDelayMs, kMillisecondScale, &at);
        thread_call_enter_delayed(gCachedScan, at);
        return 0;
      }
      gScanning = 1; gScanCursor = 0; gResultsShown = 0;
      if (nw_wifi_scan(ssid && ssidLength ? ssid : nullptr, ssidLength)) { gScanning = 0; return EBUSY; }
      diting_wifi_wake(); return 0; }
    case kAssociate: {
      if (!gPower) return ENETDOWN;
      if (rq.req_len > sizeof(out.raw) || copy_in(rq, out.raw, rq.req_len, kernel)) return EFAULT;
      diting_wifi_assoc_params a{};
      uint32_t authLower = 0, authUpper = 0; int isModern = 0;
      if (diting_wifi_decode_assoc(out.raw, rq.req_len, &a, &authLower, &authUpper, &isModern)) {
        log(4, "wifi-net: assoc unsupported layout len=%u v=%u", rq.req_len, rq.req_len >= 4 ? diting_wifi_u32(out.raw) : 0u);
        memset(&a, 0, sizeof a); return EINVAL;
      }
      // Accept open (0), iPadOS 18 WPA2-Personal (0x8), and iOS 27 WPA2-Personal (0x10).
      if (authUpper && authUpper != 0x8u && authUpper != 0x10u) {
        log(4, "wifi-net: assoc unsupported auth lower=0x%x upper=0x%x", authLower, authUpper);
        memset(&a, 0, sizeof a); return EOPNOTSUPP;
      }
      const bool hasBssid = (a.bssid[0] | a.bssid[1] | a.bssid[2] | a.bssid[3] | a.bssid[4] | a.bssid[5]) != 0;
      const bool hasKey = a.key_length != 0 && a.key_length <= 64;
      // Drain an active connection/auth attempt before issuing a new REQ_CONNECT:
      // nw_sta.c start_connect() ignores REQ_CONNECT unless C.state is IDLE/SCANNING.
      nw_wifi_status current{}; nw_wifi_status_get(&current);
      if (current.state >= NW_STA_AUTH) {
        log(6, "wifi-net: switching network: draining state %u", current.state);
        gUserJoin = true; gLinkSuppressed = true;
        __atomic_store_n(&gJoinPending, false, __ATOMIC_RELEASE);
        nw_wifi_disconnect();
        notify_locked(NotifyLinkDown);
        lck_mtx_unlock(gLock);
        diting_wifi_wake();
        bool drained = false;
        for (unsigned waited = 0; waited < 2000; waited += 10) {
          nw_wifi_status_get(&current);
          if (current.state < NW_STA_AUTH) { drained = true; break; }
          IOSleep(10);
        }
        lck_mtx_lock(gLock);
        if (!drained) {
          log(4, "wifi-net: disconnect did not drain within 2000 ms");
          memset(&a, 0, sizeof a); return ETIMEDOUT;
        }
        if (!gPower) { memset(&a, 0, sizeof a); return ENETDOWN; }
      }
      char pass[65]; memset(pass, 0, sizeof pass); if (hasKey) memcpy(pass, a.key, a.key_length);
      // cipher 6: 32-byte derived PMK -> 64 hex digits for nw_sta.
      unsigned keyLength = a.key_length;
      if (hasKey && a.key_cipher == 6 && a.key_length == 32) {
        static const char hex[] = "0123456789abcdef";
        for (int i = 0; i < 32; i++) { pass[2 * i] = hex[a.key[i] >> 4]; pass[2 * i + 1] = hex[a.key[i] & 15]; }
        keyLength = 64;
      }
      gAssocResult = kAssocPending;
      gAssocModern = isModern != 0;
      gUserJoin = true; gLinkSuppressed = false;
      __atomic_store_n(&gJoinPending, true, __ATOMIC_RELEASE);
      log(6, "wifi-net: join len=%u auth=0x%x/0x%x ssid_len=%u bssid=%d key_len=%u cipher=%u",
          rq.req_len, authLower, authUpper, a.ssid_length, hasBssid, a.key_length, a.key_cipher);
      gKeyed = hasKey;
      int r = nw_wifi_connect(a.ssid, a.ssid_length, hasBssid ? a.bssid : nullptr, hasKey ? pass : nullptr, hasKey ? keyLength : 0);
      memset(pass, 0, sizeof pass); memset(&a, 0, sizeof a);
      if (!r) diting_wifi_wake();
      else { gAssocResult = kAssocFailed; __atomic_store_n(&gJoinPending, false, __ATOMIC_RELEASE); }
      return r ? EINVAL : 0; }
    case kDisassociate: case kDeauth:
      gUserJoin = true; gLinkSuppressed = true; gAssocResult = kAssocFailed;
      __atomic_store_n(&gJoinPending, false, __ATOMIC_RELEASE);
      log(6, "wifi-net: leave");
      nw_wifi_disconnect(); diting_wifi_wake(); notify_locked(NotifyLinkDown); return 0;
    case kAbortScan: return 0;
    case kScanCacheClear: case kLocale: case kCountryCode: case kPowerSave: case kAuthType: case kCipherKey: return 0;
    default: return EOPNOTSUPP;
    }
  }
  switch (type) {
  case kPower: w[0] = 1; w[1] = 1; w[2] = gPower && st.state != NW_STA_OFF ? 1 : 0; return copy_out(rq, w, 24, kernel);
  case kState: return answer_value(rq, static_cast<uint32_t>(gPower ? map_state(st.state) : 0), kernel);
  case kOpMode: return answer_value(rq, 1, kernel);   // station
#ifdef DITING_GUEST_24B156
  case kAuthType: w[0] = 1; w[1] = 1; w[2] = gKeyed ? 0x10 : 0; w[3] = 0; return copy_out(rq, w, 16, kernel);
#else
  case kAuthType: w[0] = 1; w[1] = 1; w[2] = gKeyed ? 0x8 : 0; w[3] = 0; return copy_out(rq, w, 16, kernel);
#endif
  case kPowerSave: return answer_value(rq, 0, kernel);
  // en0 is the physical infrastructure station, not P2P/AWDL. A physical
  // interface has role zero and no virtual parent. These discovery queries
  // must not be confused with unsupported virtual-interface creation.
  case kVirtualRole: return answer_value(rq, 0, kernel);
  case kVirtualParent: return answer_bytes(rq, out.raw, 0, kernel);
  case kWowEnabled: return answer_value(rq, 0, kernel); // no WoW suspend path exposed
  case kCardCaps: {
    // Include bit 22 (named/multi-SSID scan) and bit 46 (scan-done event);
    // without bit 22 Apple80211Scan fails immediately with -3903 before reaching the driver.
    const uint32_t bytes = rq.req_len ? (rq.req_len > 32 ? 32 : rq.req_len) : 12;
    diting_wifi_caps_fill(out.raw, bytes);
    return answer_bytes(rq, out.raw, bytes, kernel); }
  case kPhyMode: w[0] = 1; w[1] = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x40; w[2] = st.state == NW_STA_UP ? 0x8 : 0; return copy_out(rq, w, 12, kernel);
  case kSsid: if (st.state < NW_STA_ASSOC) return ENOENT; return answer_bytes(rq, st.ssid, st.ssid_len, kernel);
  case kBssid: if (st.state < NW_STA_ASSOC) return ENOENT; return answer_bytes(rq, st.bssid, 6, kernel);
  case kChannel: if (st.state < NW_STA_AUTH) return ENOENT; w[0] = 1; fill_channel(*reinterpret_cast<channel *>(out.raw + 4), st.freq_mhz); return copy_out(rq, w, 16, kernel);
  case kRssi: case kNoise: {   // {version, radios, unit (0: dBm), per chain[4], aggregate, extension per chain[4], its aggregate}
    if (type == kRssi && st.state != NW_STA_UP) return ENOENT;
    const uint32_t v = static_cast<uint32_t>(type == kRssi ? static_cast<int32_t>(st.signal_dbm) : -95);
    w[0] = 1; w[1] = 1; w[2] = 0; for (int i = 3; i < 13; i++) w[i] = v; return copy_out(rq, w, 13 * 4, kernel); }
  case kRate: return answer_value(rq, st.state == NW_STA_UP ? 54 : 0, kernel);
  case kTxPower: return answer_value(rq, 15, kernel);
  case kDriverVersion: case kHardwareVersion: { const char *v = type == kDriverVersion ? "nativewifi 1" : "QCA6490 (ath11k/WCN6855)"; strlcpy(reinterpret_cast<char *>(out.raw), v, 256); return copy_out(rq, out.raw, 256, kernel); }
  case kCountryCode: { char cc[3]; country(cc); return answer_bytes(rq, cc, 3, kernel); }   // the two letters and a NUL (the daemon asks with a 3-byte buffer)
  case kAssociateResult: return answer_value(rq, static_cast<uint32_t>(gAssocResult), kernel);
  case kSupportedChannels: case kHwSupportedChannels: {
    out.chans.version = 1; unsigned n = 0;
    uint32_t frequencies[401];unsigned actual=nw_wifi_channel_list(frequencies,401);
    for(unsigned i=0;i<actual && n<401;i++){
      if (diting_wifi_fill_channel(&out.chans.list[n], frequencies[i]) != 0) continue;
      out.chans.list[n].flags|=0x80;n++;
    }
    out.chans.count = n; return copy_out(rq, &out.chans, sizeof out.chans, kernel); }
  case kScanResult: {
    nw_bss_info b; const int count = nw_wifi_bss_count();
    if (gScanCursor >= static_cast<unsigned>(count) || nw_wifi_bss_get(static_cast<int>(gScanCursor), &b)) { gScanCursor = 0; return EIO; }   // after the last record
    gScanCursor++; fill_result(out.res, b, rq.req_len);
    if (gResultsShown < 6) { ++gResultsShown; log(6, "wifi-net: scan result %u: %u MHz, %d dBm, %.*s (req=%u)", gScanCursor, b.freq_mhz, b.signal_dbm, b.ssid_len, reinterpret_cast<const char *>(b.ssid), rq.req_len); }
    return copy_out(rq, &out.res, sizeof out.res, kernel); }
  case kCurrentNetwork: {
    if (st.state < NW_STA_ASSOC) return ENOENT;
    nw_bss_info b; const int count = nw_wifi_bss_count();
    for (int i = 0; i < count; i++) if (!nw_wifi_bss_get(i, &b) && !memcmp(b.bssid, st.bssid, 6)) {
      fill_result(out.res, b, rq.req_len); out.res.rssi = st.signal_dbm;
      out.res.snr = st.signal_dbm > -95 && st.signal_dbm < 0 ? st.signal_dbm + 95 : 0;
      return copy_out(rq, &out.res, sizeof out.res, kernel);
    }
    return ENOENT; }
  default: return EOPNOTSUPP;
  }
}

errno_t if_ioctl(ifnet_t, unsigned long cmd, void *data) {
  if(!gLock)return ENXIO;
  switch (cmd) {
  case SIOCGA80211: case SIOCSA80211: {
    apple80211req *rq = static_cast<apple80211req *>(data);
    if(!rq)return EINVAL;
    const uint32_t len = rq->req_len; const int32_t val = rq->req_val;
    const bool set=cmd==SIOCSA80211;
    // Allow full Apple80211 scan-multiple (0x14e8) and user-client sizes (up to 0x3c00).
    if(len>0x3c00u || (len && !rq->req_data))return EINVAL;
    void *bytes=len?IOMallocZeroData(len):nullptr;
    if(len && !bytes)return ENOMEM;
    struct Buffer {void *p;size_t n;~Buffer(){if(p){bzero(p,n);IOFreeData(p,n);}}} buffer{bytes,len};
    if(set && len && copyin(rq->req_data,bytes,len))return EFAULT;
    apple80211req local=*rq;
    local.req_data=reinterpret_cast<user_addr_t>(bytes);
    lck_mtx_lock(gLock);
    int e=apple80211(set,local,true);
    record_ioctl_diag(rq->req_type, set, false, len, e);
    // every request with its answer, for the first few hundred; then a type's first one only
    static unsigned shown; const int type = rq->req_type; const bool first = type >= 0 && type < 1024 && !(gLogged[type / 32] & (1u << (type % 32)));
    if (type >= 0 && type < 1024) gLogged[type / 32] |= 1u << (type % 32);
    if (shown < 300 || first || (set && (type == kScanReq || type == kScanReqMultiple || type == kAssociate || type == kDisassociate || type == kPower))) {
      ++shown; log(6, "wifi-net: request %u: type %d %s len %u val %d -> %d (len %u val %d)", shown, type, set ? "set" : "get", len, val, e, local.req_len, local.req_val);
    }
    lck_mtx_unlock(gLock);
    if(!e && !set && len) {
      const size_t n=local.req_len<len?local.req_len:len;
      if(n && copyout(bytes,rq->req_data,n))e=EFAULT;
    }
    rq->req_val=local.req_val;rq->req_len=local.req_len;
    return e;
  }
  case SIOCGIFMEDIA: {
    record_ifmedia_caller();
    struct ifmediareq *m = static_cast<struct ifmediareq *>(data);
    nw_wifi_status st; nw_wifi_status_get(&st);
    // the framework lists wireless interfaces by this media type
    m->ifm_current = IFM_IEEE80211 | IFM_AUTO; m->ifm_mask = 0; m->ifm_status = IFM_AVALID | (st.state == NW_STA_UP ? IFM_ACTIVE : 0);
    m->ifm_active = IFM_IEEE80211 | IFM_AUTO;
    const int requested=m->ifm_count;
    m->ifm_count = 1;
    // Darwin first asks for the count, then supplies ifm_ulist. Returning
    // count=1 without copying the entry leaves userspace's list uninitialized.
    if (requested > 0 && m->ifm_ulist) {
      const int medium=IFM_IEEE80211 | IFM_AUTO;
      return copyout(&medium, (user_addr_t)m->ifm_ulist, sizeof(medium));
    }
    return 0;
  }
  case SIOCSIFFLAGS: case SIOCSIFMTU: case SIOCADDMULTI: case SIOCDELMULTI: case SIOCSIFADDR: case SIOCSIFMEDIA: gDiag.ifFlags++; return 0;
  default: { gDiag.ifOther++; static unsigned said; if (said++ < 16) log(6, "wifi-net: interface request %lx not handled", cmd); return EOPNOTSUPP; }
  }
}

// ---- registry entries (see the top of the file in the build: the classes are registered late) ----
}  // namespace

// 22H374's IO80211 library first binds through the interface user client.
// Its selector 0 tunnels the SAME Apple80211 requests as the socket path;
// refusing IOServiceOpen leaves modern CoreWiFi without a usable binding.
// Layout derived from the selected runtime, not an older IO80211 vtable.
struct Apple80211MethodRequest {
  uint32_t command, length;
  uint8_t bytes[0x3c00];
  apple80211req request;
};
static_assert(sizeof(Apple80211MethodRequest)==0x3c30 &&
              __builtin_offsetof(Apple80211MethodRequest,request)==0x3c08,
              "22H374 Apple80211 selector 0 layout");

class Diting80211UserClient : public IOUserClient {
  OSDeclareDefaultStructors(Diting80211UserClient);
  static uint32_t capacity(IOExternalMethodArguments *a) {
    return a->structureOutputDescriptor ? a->structureOutputDescriptorSize : a->structureOutputSize;
  }
  static bool output(IOExternalMethodArguments *a,const void *data,uint32_t bytes) {
    if(bytes>capacity(a))return false;
    if(a->structureOutputDescriptor) {
      // MIG supplies an unwired virtual descriptor for OOL requests.
      // writeBytes walks physical segments; it does not fault/wire pages.
      // Pair ownership locally and finish before returning to the caller.
      auto memory=a->structureOutputDescriptor;
      if(bytes) {
        IOReturn result=memory->prepare(kIODirectionIn);
        if(result!=kIOReturnSuccess)return false;
        const bool copied=memory->writeBytes(0,data,bytes)==bytes;
        memory->complete(kIODirectionIn);
        if(!copied)return false;
      }
      a->structureOutputDescriptorSize=bytes;
    } else {
      if(bytes && !a->structureOutput)return false;
      if(bytes)memcpy(a->structureOutput,data,bytes);
      a->structureOutputSize=bytes;
    }
    return true;
  }
public:
  bool initWithTask(task_t task,void *security,UInt32 type,OSDictionary *properties) override {
    return type==0 && IOUserClient::initWithTask(task,security,type,properties);
  }
  IOReturn clientClose() override { terminate();return kIOReturnSuccess; }
  IOReturn externalMethod(uint32_t selector,IOExternalMethodArguments *a,
      IOExternalMethodDispatch *,OSObject *,void *) override {
    if(!a || a->scalarInputCount || a->scalarOutputCount) {
      gDiag.ucRefused++; gDiag.lastRefusedSel = selector;
      return kIOReturnBadArgument;
    }
    const uint64_t inputBytes=a->structureInputDescriptor ?
        a->structureInputDescriptor->getLength() : a->structureInputSize;
    if(selector==1 || selector==5 || selector==7) {
      if(inputBytes || capacity(a)!=1)return kIOReturnBadArgument;
      // sel 1: standard ioctl tunnel supported (1).
      // sel 5: IOUC event pipe supported (0 -> framework listens on PF_SYSTEM kernel event socket).
      // sel 7: DriverKit driver (0).
      const uint8_t supported=selector==1 ? 1 : 0;
      return output(a,&supported,1)?kIOReturnSuccess:kIOReturnBadArgument;
    }
    if(selector!=0) {
      gDiag.ucRefused++; gDiag.lastRefusedSel = selector;
      gDiag.lastRefusedIn = static_cast<uint32_t>(inputBytes);
      gDiag.lastRefusedOut = capacity(a);
      static unsigned said;
      if (said++ < 16) log(6, "wifi-net: native client unsupported sel=%u in=%llu out=%u", selector, static_cast<unsigned long long>(inputBytes), capacity(a));
      return kIOReturnUnsupported;
    }
    if(inputBytes!=sizeof(Apple80211MethodRequest) || capacity(a)>0x3c00 || !gLock) {
      gDiag.ucRefused++; gDiag.lastRefusedSel = selector;
      gDiag.lastRefusedIn = static_cast<uint32_t>(inputBytes);
      gDiag.lastRefusedOut = capacity(a);
      static unsigned said;
      if (said++ < 16) log(6, "wifi-net: native client bad size in=%llu out=%u", static_cast<unsigned long long>(inputBytes), capacity(a));
      return kIOReturnBadArgument;
    }
    auto packet=static_cast<Apple80211MethodRequest *>(IOMallocData(sizeof(Apple80211MethodRequest)));
    if(!packet)return kIOReturnNoMemory;
    struct Free {Apple80211MethodRequest *p;~Free(){
      auto bytes=reinterpret_cast<volatile uint8_t *>(p);
      for(size_t i=0;i<sizeof(*p);i++)bytes[i]=0;
      IOFreeData(p,sizeof(*p));}} release{packet};
    if(a->structureInputDescriptor) {
      // The native envelope is 15KiB, larger than MIG's in-band limit.
      // services88 read this descriptor without prepare(), which can reach
      // IOGMD's fatal "not wired for the IODMACommand" path. The pages
      // belong to this one request, never to the radio's DMA arena.
      auto memory=a->structureInputDescriptor;
      IOReturn result=memory->prepare(kIODirectionOut);
      if(result!=kIOReturnSuccess)return result;
      const bool copied=memory->readBytes(0,packet,sizeof(*packet))==sizeof(*packet);
      memory->complete(kIODirectionOut);
      if(!copied)return kIOReturnNotReadable;
    } else {
      if(!a->structureInput)return kIOReturnBadArgument;
      memcpy(packet,a->structureInput,sizeof(*packet));
    }
    if((packet->command!=SIOCGA80211 && packet->command!=SIOCSA80211) ||
       packet->length>sizeof(packet->bytes) ||
       strncmp(packet->request.req_if_name,"en0",sizeof(packet->request.req_if_name)))
      return kIOReturnBadArgument;
    const bool set=packet->command==SIOCSA80211;
    const uint32_t outCapacity=capacity(a);
    // The library puts payload length at +4 and payload at +8. The copied
    // user pointer at +3c28 is never followed. Scalar GETs return req_val;
    // bind's version sanity query instead supplies a 256-byte output only.
    const bool scalar=!set && !packet->length && outCapacity==sizeof(uint32_t);
    apple80211req rq=packet->request;
    if(set) {
      rq.req_len=packet->length ? packet->length : rq.req_len;
      if(rq.req_len>sizeof(packet->bytes))return kIOReturnBadArgument;
    } else if(scalar) {
      rq.req_len=0;
    } else {
      rq.req_len=packet->length ? packet->length : outCapacity;
    }
    if(!set && rq.req_len>outCapacity)return kIOReturnBadArgument;
    rq.req_data=rq.req_len?reinterpret_cast<user_addr_t>(packet->bytes):0;
    int result;
    {
      struct Guard {Guard(){lck_mtx_lock(gLock);}~Guard(){lck_mtx_unlock(gLock);}} guard;
      result=apple80211(set,rq,true);
      record_ioctl_diag(rq.req_type, set, true, rq.req_len, result);
    }
    static unsigned logged;
    const bool interesting = set && (rq.req_type == kScanReq || rq.req_type == kScanReqMultiple ||
        rq.req_type == kAssociate || rq.req_type == kDisassociate || rq.req_type == kPower ||
        rq.req_type == kCipherKey || rq.req_type == kAuthType);
    const bool unexpectedErr = result != 0 && result != ENOENT && !(rq.req_type == kScanResult && result == EIO);
    if(logged < 96 || interesting || (unexpectedErr && logged < 256)) {
      logged++;
      log(6,"wifi-net: native client type %d %s len=%u out=%u -> %d",
          rq.req_type,set?"set":"get",rq.req_len,outCapacity,result);
    }
    // IO80211's wrapper preserves these errno values; do not turn an
    // unsupported request or failed association into a successful result.
    if(result)return static_cast<IOReturn>(result);
    if(set)return output(a,nullptr,0)?kIOReturnSuccess:kIOReturnBadArgument;
    const uint32_t bytes=scalar?sizeof(rq.req_val):(rq.req_len<outCapacity?rq.req_len:outCapacity);
    return output(a,scalar?static_cast<const void *>(&rq.req_val):packet->bytes,bytes)?
        kIOReturnSuccess:kIOReturnBadArgument;
  }
};
OSDefineMetaClassAndStructors(Diting80211UserClient,IOUserClient);
class IO80211Controller : public IOService {OSDeclareDefaultStructors(IO80211Controller);};
OSDefineMetaClassAndStructors(IO80211Controller,IOService);
class IONetworkInterface : public IOService {OSDeclareDefaultStructors(IONetworkInterface);};
OSDefineMetaClassAndStructors(IONetworkInterface,IOService);
class IO80211Interface : public IONetworkInterface {
  OSDeclareDefaultStructors(IO80211Interface);
public:
  IOReturn newUserClient(task_t task,void *security,UInt32 type,OSDictionary *properties,IOUserClient **out) override {
    if(type!=0)return kIOReturnUnsupported;
    auto client=OSTypeAlloc(Diting80211UserClient);
    if(!client || !client->initWithTask(task,security,type,properties)) {
      OSSafeReleaseNULL(client);return kIOReturnNotPermitted;
    }
    if(!client->attach(this)){client->release();return kIOReturnError;}
    if(!client->start(this)){client->detach(this);client->release();return kIOReturnError;}
    if (gLock) { lck_mtx_lock(gLock); record_uc_opener(); lck_mtx_unlock(gLock); }
    *out=client;return kIOReturnSuccess;
  }
};
OSDefineMetaClassAndStructors(IO80211Interface,IONetworkInterface);

// The network configuration's interface namer waits for a service of this class, opens it and only then publishes that IOKit is quiet;
// everything that lists interfaces (the wireless daemon among them) waits for that. Interfaces here are named when they are made, so the
// commands it sends (as properties of its connection) are only logged.
class IONetworkStackUserClient : public IOUserClient {
  OSDeclareDefaultStructors(IONetworkStackUserClient);
 public:
  IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE {
    gDiag.stackCmds++;
    char text[200] = "?";
    if (OSSerialize *s = OSSerialize::withCapacity(512)) { if (properties && properties->serialize(s)) strlcpy(text, s->text(), sizeof text); s->release(); }
    strlcpy(gDiag.lastStackCmd, text, sizeof gDiag.lastStackCmd);
    static unsigned said;
    if (said++ < 16) {
      log(6, "wifi-net: network stack command %s", text);
    }
    return kIOReturnSuccess;
  }
  IOReturn clientClose() APPLE_KEXT_OVERRIDE { terminate(); return kIOReturnSuccess; }
};
OSDefineMetaClassAndStructors(IONetworkStackUserClient, IOUserClient);
class IONetworkStack : public IOService {
  OSDeclareDefaultStructors(IONetworkStack);
 public:
  IOReturn newUserClient(task_t task, void *security, UInt32 type, OSDictionary *, IOUserClient **handler) APPLE_KEXT_OVERRIDE {
    IONetworkStackUserClient *client = OSTypeAlloc(IONetworkStackUserClient);
    if (!client || !client->initWithTask(task, security, type)) { OSSafeReleaseNULL(client); return kIOReturnNoMemory; }
    if (!client->attach(this)) { client->release(); return kIOReturnError; }
    if (!client->start(this)) { client->detach(this); client->release(); return kIOReturnError; }
    gDiag.stackOpened++;
    log(6, "wifi-net: the network stack was opened (type %u)", static_cast<unsigned>(type));
    *handler = client; return kIOReturnSuccess;
  }
};
OSDefineMetaClassAndStructors(IONetworkStack, IOService);

namespace {

IOService *publish(IOService *service, IOService *provider, const char *name) {
  if (!service || !service->attach(provider)) { OSSafeReleaseNULL(service); return nullptr; }
  service->setName(name);
  if (!service->start(provider)) { service->detach(provider); service->release(); return nullptr; }
  service->registerService();
  return service;
}
void publish_registry(const char *ifname) {
  IOService *platform = IOService::getPlatform();
  if (IONetworkStack *stack = OSTypeAlloc(IONetworkStack)) {
    if (stack->init()) {
      stack->setProperty("NativeSandboxGrantProcess", "configd");
      if (!publish(stack, platform, "IONetworkStack")) log(3, "wifi-net: the network stack service did not start"); else stack->release();
    } else {
      stack->release();
      log(3, "wifi-net: the network stack service did not start");
    }
  }
  IO80211Controller *controller = OSTypeAlloc(IO80211Controller);
  IO80211Interface *wlan = OSTypeAlloc(IO80211Interface);
  if (!platform || !controller || !wlan || !controller->init() || !wlan->init()) { OSSafeReleaseNULL(controller); OSSafeReleaseNULL(wlan); log(3, "wifi-net: registry objects not made"); return; }
  if (OSData *mac = OSData::withBytes(gMac, 6)) { controller->setProperty("IOMACAddress", mac); mac->release(); }
  controller->setProperty("IOBuiltin", true);
#ifndef DITING_GUEST_24B156
  // iPadOS 18 uses /diting/wifi-bootstrap for SC service creation. On iOS 27
  // (matching UniXNU-sheng), let stock configd InterfaceNamer register en0
  // natively through IONetworkStack so SystemConfiguration, IPConfiguration
  // (DHCP), and Settings WiFiKit discover the real IEEE80211 service.
  controller->setProperty("SelfNamed", true);
#endif
  controller = static_cast<IO80211Controller *>(publish(controller, platform, "DitingWLANController"));
  if (!controller) { wlan->release(); log(3, "wifi-net: the controller did not start"); return; }
  wlan->setProperty("IOInterfaceName", ifname); wlan->setProperty("BSD Name", ifname); wlan->setProperty("IOInterfaceNamePrefix", "en");
  if (OSNumber *n = OSNumber::withNumber(static_cast<unsigned long long>(IFT_ETHER), 32)) { wlan->setProperty("IOInterfaceType", n); n->release(); }
  if (OSNumber *n = OSNumber::withNumber(0ULL, 32)) { wlan->setProperty("IOInterfaceUnit", n); n->release(); }
  if (OSData *mac = OSData::withBytes(gMac, 6)) {wlan->setProperty("IOMACAddress", mac);mac->release();}
  wlan->setProperty("IOBuiltin", true); wlan->setProperty("IOPrimaryInterface", true); wlan->setProperty("IO80211InterfaceRole", "Infrastructure");
  wlan->setProperty("NativeSandboxGrantProcess","wifid,wifip2pd,configd,airportd,networkd,locationd,SpringBoard,Preferences,sharingd,wifivelocityd,CommCenter,symptomsd,nehelper,WiFiCloudAssets");
  wlan->setProperty("IOUserClientClass","Diting80211UserClient");
  if (!publish(wlan, controller, "DitingWLAN")) log(3, "wifi-net: the interface service did not start"); else wlan->release();
  controller->release();
}

// en0's IPv4 addresses and packet counts, for the log.
void log_traffic() {
  if (!gIf) return;
  char text[64] = "none"; ifaddr_t *list = nullptr;
  if (!ifnet_get_address_list_family(gIf, &list, AF_INET) && list) {
    if (list[0]) { struct sockaddr_in a; memset(&a, 0, sizeof a);
      if (!ifaddr_address(list[0], reinterpret_cast<struct sockaddr *>(&a), sizeof a)) { const uint8_t *b = reinterpret_cast<const uint8_t *>(&a.sin_addr); snprintf(text, sizeof text, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]); } }
    ifnet_free_address_list(list); }
  struct ifnet_stats_param stats; memset(&stats, 0, sizeof stats); ifnet_stat(gIf, &stats);
  char tx[160]; nw_wifi_tx_reasons(tx, sizeof tx);
  log(6, "wifi-net: en0 address %s, packets in %llu out %llu; frames to the stack %u, refused %u (last %d)", text, static_cast<unsigned long long>(stats.packets_in),
      static_cast<unsigned long long>(stats.packets_out), gRxFrames, gRxErrors, gRxLastError);
  log(6, "wifi-net: tx %s", tx);
  log(6, "wifi-net: in arp %u ip4 %u dhcp %u ip6 %u other %u", gKindsIn.arp, gKindsIn.ip4, gKindsIn.dhcp, gKindsIn.ip6, gKindsIn.other);
  log(6, "wifi-net: out arp %u ip4 %u dhcp %u ip6 %u other %u", gKindsOut.arp, gKindsOut.ip4, gKindsOut.dhcp, gKindsOut.ip6, gKindsOut.other);
  log(6, "wifi-net: input stalls %u, kicks %u, cleared after a kick %u, longest %u s", gStall.count, gStall.kicks, gStall.cleared, gStall.longest);
  log(6, "wifi-net: rx no buffer %u, chained %u, longest frame %u", gRxNoBuffer, gRxChains, gRxLongest);
}

}  // namespace

// Install worker callbacks before nw_wifi_start. The interface and its MAC
// are published only after the real station is ready. RX already drops
// frames before gIf exists; those buffers remain owned by the station.
extern "C" bool diting_wifi_net_prepare(void) {
  if (!gGrp) gGrp = lck_grp_alloc_init("diting-wifi-net", LCK_GRP_ATTR_NULL);
  if (!gGrp) return false;
  if (!gLock) gLock = lck_mtx_alloc_init(gGrp, LCK_ATTR_NULL);
  if (!gLock) return false;
  if (!gNotifications) gNotifications=thread_call_allocate(deliver_notifications,nullptr);
  if (!gNotifications) return false;
  nw_wifi_set_events(&gEvents);
  return true;
}

// Failure completes outstanding asynchronous requests; wifid must not wait
// indefinitely for scan/join events from a stopped firmware dispatcher.
extern "C" void diting_wifi_net_failed(void) {
  if (!gLock) return;
  lck_mtx_lock(gLock);
  __atomic_store_n(&gFailed,true,__ATOMIC_RELEASE); gPower = false;
  bool scanning = gScanning != 0;
  gScanning = 0; gScanCursor = 0; gAssocResult = kAssocFailed;
  if (gCachedScan) thread_call_cancel(gCachedScan);
  lck_mtx_unlock(gLock);
  if (!gIf) return;
  post_link(false);
  if (scanning) post_event(kEvScanDone, nullptr, 0);
  post_event(kEvAssocDone, nullptr, 0);
  post_event(kEvPowerChanged, nullptr, 0);
}

// Called once the module has a station interface and a hardware address.
extern "C" void diting_wifi_net_start(const uint8_t mac[6]) {
  if (gIf) return;
  if (!diting_wifi_net_prepare()) { log(3, "wifi-net: event setup unavailable"); return; }
  memcpy(gMac, mac, 6);
  struct ifnet_init_eparams p; memset(&p, 0, sizeof p);
  p.ver = IFNET_INIT_CURRENT_VERSION; p.len = sizeof p; p.flags = IFNET_INIT_LEGACY | IFNET_INIT_NX_NOAUTO; p.name = "en"; p.unit = 0; p.family = IFNET_FAMILY_ETHERNET; p.subfamily = IFNET_SUBFAMILY_WIFI; p.type = IFT_ETHER;
  p.output = if_output; p.demux = ether_demux; p.add_proto = ether_add_proto; p.del_proto = ether_del_proto; p.check_multi = ether_check_multi; p.framer = ether_frameout;
  p.ioctl = if_ioctl; p.event = if_event_stub; static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }; p.broadcast_addr = bcast; p.broadcast_len = 6;
  // Without NX_NOAUTO the stack puts a compatibility nexus in front of a Wi-Fi "en" interface, and its input no longer goes through the
  // interface's own input thread.
  ifnet_t ifp = nullptr; errno_t e = ifnet_allocate_extended(&p, &ifp);
  if (e || !ifp) { log(3, "wifi-net: ifnet allocation failed: %d", e); return; }
  ifnet_set_mtu(ifp, 1500); ifnet_set_flags(ifp, IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST, 0xffff); ifnet_set_baudrate(ifp, 54000000ULL);
  ifnet_set_hdrlen(ifp, 14); ifnet_set_addrlen(ifp, 6); ifnet_set_eflags(ifp, IFEF_ENQUEUE_MULTI, IFEF_ENQUEUE_MULTI);
  e = ifnet_attach(ifp, nullptr);
  if (e) { log(3, "wifi-net: ifnet attach failed: %d", e); ifnet_release(ifp); return; }
  ifnet_set_lladdr(ifp, mac, 6);
  // A driver of its own interface attaches the packet filter itself: the DHCP client sends through it while the interface has no address.
  e = bpf_attach(ifp, DLT_EN10MB, 14, nullptr, nullptr); if (e) log(3, "wifi-net: packet filter attach failed: %d", e);
  gIf = ifp; ifnet_set_flags(ifp, IFF_UP, IFF_UP);
  publish_registry("en0");
  log(6, "wifi-net: en0 is up, hardware address %02x:%02x:%02x:%02x:%02x:%02x, capabilities 0x%x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
      ifnet_capabilities_enabled(ifp));
}

namespace {
static void wifi_append(char *out, unsigned cap, unsigned &used, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void wifi_append(char *out, unsigned cap, unsigned &used, const char *fmt, ...) {
  if (!out || used + 1 >= cap) return;
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(out + used, cap - used, fmt, ap);
  va_end(ap);
  if (n > 0) used += (unsigned(n) < cap - used ? unsigned(n) : cap - used - 1);
}
static const char *sta_name(uint32_t s) {
  switch (s) {
  case NW_STA_OFF: return "OFF"; case NW_STA_IDLE: return "IDLE"; case NW_STA_SCANNING: return "SCANNING";
  case NW_STA_AUTH: return "AUTH"; case NW_STA_ASSOC: return "ASSOC"; case NW_STA_KEYS: return "KEYS";
  case NW_STA_UP: return "UP"; default: return "UNKNOWN";
  }
}
static void format_ipv4(char out[24]) {
  strlcpy(out, "none", 24);
  if (!gIf) return;
  ifaddr_t *list = nullptr;
  if (!ifnet_get_address_list_family(gIf, &list, AF_INET) && list) {
    if (list[0]) {
      struct sockaddr_in a{};
      if (!ifaddr_address(list[0], reinterpret_cast<struct sockaddr *>(&a), sizeof a)) {
        const uint8_t *b = reinterpret_cast<const uint8_t *>(&a.sin_addr);
        snprintf(out, 24, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
      }
    }
    ifnet_free_address_list(list);
  }
}
static const char *bss_sec(const nw_bss_info &b) {
  bool hasRsn = false, hasWpa = false;
  for (unsigned o = 0; o + 2 <= b.ie_len && o + 2 + b.ie[o + 1] <= b.ie_len; o += 2 + b.ie[o + 1]) {
    if (b.ie[o] == 48) hasRsn = true;
    else if (b.ie[o] == 221 && b.ie[o + 1] >= 4 && b.ie[o + 2] == 0x00 && b.ie[o + 3] == 0x50 && b.ie[o + 4] == 0xf2 && b.ie[o + 5] == 0x01) hasWpa = true;
  }
  if (hasRsn) return "WPA2";
  if (hasWpa) return "WPA";
  return (b.caps & 0x10u) ? "PRIV" : "OPEN";
}
static void dump_bss(char *out, unsigned cap, unsigned &used) {
  const int count = nw_wifi_bss_count();
  wifi_append(out, cap, used, "WIFI_BSS_COUNT count=%d\n", count);
  nw_bss_info b{};
  for (int i = 0; i < count && i < 32 && used + 128 < cap; ++i) {
    if (nw_wifi_bss_get(i, &b)) continue;
    char ssid[33]; unsigned sl = b.ssid_len > 32 ? 32 : b.ssid_len;
    for (unsigned k = 0; k < sl; ++k) ssid[k] = (b.ssid[k] >= 0x20 && b.ssid[k] < 0x7f) ? static_cast<char>(b.ssid[k]) : '?';
    ssid[sl] = 0;
    wifi_append(out, cap, used, "WIFI_BSS idx=%d freq=%u rssi=%d age_ms=%u sec=%s bssid=%02x:%02x:%02x:%02x:%02x:%02x ssid=%s\n",
                i, b.freq_mhz, b.signal_dbm, b.age_ms, bss_sec(b),
                b.bssid[0], b.bssid[1], b.bssid[2], b.bssid[3], b.bssid[4], b.bssid[5], ssid[0] ? ssid : "<hidden>");
  }
}
}  // namespace

extern "C" unsigned diting_wifi_usb_command(const char *commandLine, char *out, unsigned cap) {
  if (!out || cap < 64) return 0;
  unsigned used = 0;
  const char *arg = commandLine ? commandLine : "";
  if (!strncmp(arg, "wifi", 4)) arg += 4;
  while (*arg == ' ') ++arg;

  if (!gIf || !gLock) {
    wifi_append(out, cap, used, "WIFI_STATUS ready=0 stage=%u scans=%u bss=%u last_err=%d\n",
                nw_wifi_stats.stage, nw_wifi_stats.scans, nw_wifi_stats.bss_seen, nw_wifi_stats.last_err);
    return used;
  }

  if (!strcmp(arg, "selftest")) {
    uint8_t *buf = static_cast<uint8_t *>(IOMallocZeroData(0x1600));
    if (!buf) { wifi_append(out, cap, used, "WIFI_SELFTEST status=FAIL reason=nomem\n"); return used; }
    struct Free { uint8_t *p; ~Free() { IOFreeData(p, 0x1600); } } freeBuf{buf};

    int rCaps = -1, rPow = -1, rCh = -1, rCc = -1, rScanMulti = -1, rScanRes = -1, rAuth = -1, rAbort = -1, rKev = -1;
    uint32_t capsHas22 = 0, capsHas46 = 0, powVal = 0, chCount = 0, resCount = 0, authUpper = 0;
    uint64_t resTs = 0;
    char cc[4] = {};

    lck_mtx_lock(gLock);
    const unsigned savedCursor = gScanCursor;
    const unsigned savedScanning = gScanning;
    // 1. kCardCaps (23 bytes, iOS 27 size)
    { apple80211req rq{}; strlcpy(rq.req_if_name, "en0", sizeof rq.req_if_name); rq.req_type = kCardCaps; rq.req_len = 23; rq.req_data = reinterpret_cast<user_addr_t>(buf);
      rCaps = apple80211(false, rq, true);
      capsHas22 = (buf[22 / 8] >> (22 % 8)) & 1u;
      capsHas46 = (buf[46 / 8] >> (46 % 8)) & 1u; }
    // 2. kPower (24 bytes)
    { memset(buf, 0, 32); apple80211req rq{}; strlcpy(rq.req_if_name, "en0", sizeof rq.req_if_name); rq.req_type = kPower; rq.req_len = 24; rq.req_data = reinterpret_cast<user_addr_t>(buf);
      rPow = apple80211(false, rq, true);
      powVal = diting_wifi_u32(buf + 8); }
    // 3. kSupportedChannels (4824 bytes)
    { memset(buf, 0, sizeof(channels_list)); apple80211req rq{}; strlcpy(rq.req_if_name, "en0", sizeof rq.req_if_name); rq.req_type = kSupportedChannels; rq.req_len = sizeof(channels_list); rq.req_data = reinterpret_cast<user_addr_t>(buf);
      rCh = apple80211(false, rq, true);
      chCount = diting_wifi_u32(buf + 4); }
    // 4. kCountryCode (3 bytes)
    { memset(buf, 0, 8); apple80211req rq{}; strlcpy(rq.req_if_name, "en0", sizeof rq.req_if_name); rq.req_type = kCountryCode; rq.req_len = 3; rq.req_data = reinterpret_cast<user_addr_t>(buf);
      rCc = apple80211(false, rq, true);
      memcpy(cc, buf, 3); cc[3] = 0; }
    // 5. kScanReqMultiple (0x14e8 bytes, cached type=3, 1 wildcard)
    { memset(buf, 0, DITING_WIFI_SCAN_MULTIPLE_BYTES);
      uint32_t one = 1, cached = DITING_WIFI_SCAN_TYPE_CACHED;
      memcpy(buf + DITING_WIFI_SCAN_SSID_COUNT, &one, 4);
      memcpy(buf + DITING_WIFI_SCAN_TYPE_MULTI, &cached, 4);
      apple80211req rq{}; strlcpy(rq.req_if_name, "en0", sizeof rq.req_if_name); rq.req_type = kScanReqMultiple; rq.req_len = DITING_WIFI_SCAN_MULTIPLE_BYTES; rq.req_data = reinterpret_cast<user_addr_t>(buf);
      rScanMulti = apple80211(true, rq, true); }
    // 6. kScanResult (0x8e8 bytes, verify timestamp at 0x8d0)
    { memset(buf, 0, DITING_WIFI_SCAN_RESULT_BYTES);
      apple80211req rq{}; strlcpy(rq.req_if_name, "en0", sizeof rq.req_if_name); rq.req_type = kScanResult; rq.req_len = DITING_WIFI_SCAN_RESULT_BYTES; rq.req_data = reinterpret_cast<user_addr_t>(buf);
      rScanRes = apple80211(false, rq, true);
      if (!rScanRes) {
        resCount = static_cast<uint32_t>(nw_wifi_bss_count());
        memcpy(&resTs, buf + DITING_WIFI_SCAN_TIMESTAMP, sizeof(resTs));
      } }
    // 7. kAuthType (16 bytes) + kAbortScan
    { memset(buf, 0, 16); apple80211req rq{}; strlcpy(rq.req_if_name, "en0", sizeof rq.req_if_name); rq.req_type = kAuthType; rq.req_len = 16; rq.req_data = reinterpret_cast<user_addr_t>(buf);
      rAuth = apple80211(false, rq, true);
      authUpper = diting_wifi_u32(buf + 8); }
    { apple80211req rq{}; strlcpy(rq.req_if_name, "en0", sizeof rq.req_if_name); rq.req_type = kAbortScan;
      rAbort = apple80211(true, rq, true); }
    if (!savedScanning) {
      if (gCachedScan) thread_call_cancel(gCachedScan);
      gScanning = 0;
    }
    gScanCursor = savedCursor;
    lck_mtx_unlock(gLock);

    // 8. Verify 540-byte KEV_IEEE80211_CLASS cluster event (harmless test event code 0xff00).
    { assoc_event27 testEv{}; testEv.has_result = 0;
      rKev = post_event(0xff00u, &testEv, sizeof testEv); }

    const bool pass = !rCaps && capsHas22 && capsHas46 && !rPow && powVal == 1 && !rCh && chCount > 0 &&
                      !rCc && !rScanMulti && !rScanRes && resCount > 0 && resTs != 0 && !rAuth && !rAbort && !rKev;
    wifi_append(out, cap, used,
                "WIFI_SELFTEST status=%s caps=%d(b22=%u,b46=%u) power=%d(v=%u) ch=%d(n=%u) cc=%d(%s) scanMulti=%d scanRes=%d(n=%u,ts=%llu) auth=%d(u=0x%x) abort=%d kev540=%d\n",
                pass ? "PASS" : "FAIL", rCaps, capsHas22, capsHas46, rPow, powVal, rCh, chCount,
                rCc, cc[0] ? cc : "??", rScanMulti, rScanRes, resCount, static_cast<unsigned long long>(resTs),
                rAuth, authUpper, rAbort, rKev);
    return used;
  }

  if (!strcmp(arg, "scan")) {
    lck_mtx_lock(gLock);
    const int rc = nw_wifi_scan(nullptr, 0);
    if (!rc) { gScanning = 1; gScanCursor = 0; }
    lck_mtx_unlock(gLock);
    if (!rc) diting_wifi_wake();
    wifi_append(out, cap, used, "WIFI_SCAN queued=%d scans_so_far=%u\n", rc == 0 ? 1 : 0, nw_wifi_stats.scans);
    dump_bss(out, cap, used);
    return used;
  }

  if (!strcmp(arg, "bss")) {
    dump_bss(out, cap, used);
    return used;
  }

  if (!strncmp(arg, "join ", 5)) {
    const char *p = arg + 5;
    while (*p == ' ') ++p;
    char ssid[33] = {}, psk[65] = {};
    unsigned sl = 0, pl = 0;
    while (*p && *p != ' ' && sl < 32) ssid[sl++] = *p++;
    while (*p == ' ') ++p;
    while (*p && *p != '\r' && *p != '\n' && pl < 64) psk[pl++] = *p++;
    if (!sl) { wifi_append(out, cap, used, "WIFI_JOIN status=EINVAL usage=wifi join <ssid> [psk]\n"); return used; }
    lck_mtx_lock(gLock);
    gUserJoin = true; gLinkSuppressed = false; gKeyed = pl > 0;
    gAssocResult = kAssocPending; gAssocModern = true;
    __atomic_store_n(&gJoinPending, true, __ATOMIC_RELEASE);
    const int rc = nw_wifi_connect(reinterpret_cast<const uint8_t *>(ssid), sl, nullptr, pl ? psk : nullptr, pl);
    lck_mtx_unlock(gLock);
    memset(psk, 0, sizeof psk);
    if (!rc) diting_wifi_wake();
    wifi_append(out, cap, used, "WIFI_JOIN ssid=%s keyed=%u rc=%d\n", ssid, pl > 0 ? 1u : 0u, rc);
    return used;
  }

  if (!strcmp(arg, "leave")) {
    lck_mtx_lock(gLock);
    gUserJoin = true; gLinkSuppressed = true; gAssocResult = kAssocFailed;
    __atomic_store_n(&gJoinPending, false, __ATOMIC_RELEASE);
    nw_wifi_disconnect();
    notify_locked(NotifyLinkDown);
    lck_mtx_unlock(gLock);
    diting_wifi_wake();
    wifi_append(out, cap, used, "WIFI_LEAVE ok=1\n");
    return used;
  }

  if (!strcmp(arg, "power 0") || !strcmp(arg, "power 1")) {
    const uint32_t on = arg[6] == '1' ? 1u : 0u;
    uint32_t in[6] = {1u, 1u, on, 0u, 0u, 0u};
    apple80211req rq{};
    strlcpy(rq.req_if_name, "en0", sizeof rq.req_if_name);
    rq.req_type = kPower; rq.req_len = sizeof in;
    rq.req_data = reinterpret_cast<user_addr_t>(in);
    lck_mtx_lock(gLock);
    const int rc = apple80211(true, rq, true);
    lck_mtx_unlock(gLock);
    wifi_append(out, cap, used, "WIFI_POWER set=%u rc=%d power=%u\n", on, rc, gPower ? 1u : 0u);
    return used;
  }

  if (!strncmp(arg, "stacks ", 7)) {
    const char *p = arg + 7;
    while (*p == ' ') ++p;
    int pid = 0; bool ok = false;
    while (*p >= '0' && *p <= '9') {
      ok = true;
      pid = pid * 10 + (*p++ - '0');
      if (pid > 99999) break;
    }
    if (!ok || pid <= 0 || !&diting_dump_task_threads) {
      wifi_append(out, cap, used, "WIFI_STACKS status=EINVAL\n");
      return used;
    }
    char pname[32] = {};
    proc_name(pid, pname, sizeof pname);
    struct proc *pr = proc_find(pid);
    if (!pr) {
      wifi_append(out, cap, used, "WIFI_STACKS pid=%d status=ESRCH\n", pid);
      return used;
    }
    task_t tk = proc_task(pr);
    if (tk) {
      diting_dump_task_threads(pname[0] ? pname : "?", pid, tk, 0);
      diting_dump_task_threads(pname[0] ? pname : "?", pid, tk, 1);
    }
    proc_rele(pr);
    wifi_append(out, cap, used, "WIFI_STACKS pid=%d name=%s dumped=%u\n",
                pid, pname[0] ? pname : "?", tk ? 1u : 0u);
    return used;
  }

  if (!strcmp(arg, "diag")) {
    // 1. Report IOKit busy state (InterfaceNamer / SCNetworkInterfaceCopyAll waits for quiet).
    IOService *root = IOService::getServiceRoot();
    const UInt32 busy = root ? root->getBusyState() : 0;
    wifi_append(out, cap, used, "WIFI_IOKIT busy=%u leaves=", static_cast<unsigned>(busy));
    unsigned shownLeaves = 0;
    if (busy) {
      if (IORegistryIterator *it = IORegistryIterator::iterateOver(gIOServicePlane, kIORegistryIterateRecursively)) {
        while (OSObject *o = it->getNextObject()) {
          IOService *svc = OSDynamicCast(IOService, o);
          if (!svc || !svc->getBusyState()) continue;
          bool busyChild = false;
          if (OSIterator *ch = svc->getChildIterator(gIOServicePlane)) {
            while (OSObject *c = ch->getNextObject()) {
              IOService *cs = OSDynamicCast(IOService, c);
              if (cs && cs->getBusyState()) { busyChild = true; break; }
            }
            ch->release();
          }
          if (busyChild) continue;
          wifi_append(out, cap, used, "%s%s(%s)", shownLeaves ? "," : "",
                      svc->getName() ? svc->getName() : "?",
                      svc->getMetaClass() ? svc->getMetaClass()->getClassName() : "?");
          if (++shownLeaves >= 8) break;
        }
        it->release();
      }
    }
    if (!shownLeaves) wifi_append(out, cap, used, "none");
    wifi_append(out, cap, used, "\n");
    if (&diting_wifi_hw_diag) {
      uint32_t msi = 0, blks = 0, sUsed = 0, sTot = 0, paV = 0;
      uint64_t hData = 0;
      int paK = 0;
      const char *bdf = "none", *regdb = "none";
      diting_wifi_hw_diag(&msi, &blks, &sUsed, &sTot, &paK, &paV, &hData, &bdf, &regdb);
      wifi_append(out, cap, used, "WIFI_HW msi=%u blocks=%u slab=%u/%u bdf=%s regdb=%s pa=%d(valid=%u,data=0x%llx)\n",
                  msi, blks, sUsed, sTot, bdf, regdb, paK, paV, static_cast<unsigned long long>(hData));
    }
    const uint32_t arpSeq = __atomic_load_n(&gArpRingSeq, __ATOMIC_RELAXED);
    if (arpSeq) {
      const uint32_t show = arpSeq < 8u ? arpSeq : 8u;
      wifi_append(out, cap, used, "WIFI_ARP_RECENT seq=%u", arpSeq);
      for (uint32_t i = arpSeq - show; i < arpSeq && used + 80 < cap; ++i) {
        const ArpRec &r = gArpRing[i & 7u];
        wifi_append(out, cap, used, " [%s op=%u %u.%u.%u.%u->%u.%u.%u.%u %02x:%02x->%02x:%02x]",
                    r.out ? "TX" : "RX", r.op,
                    r.spa[0], r.spa[1], r.spa[2], r.spa[3],
                    r.tpa[0], r.tpa[1], r.tpa[2], r.tpa[3],
                    r.src[4], r.src[5], r.dst[4], r.dst[5]);
      }
      wifi_append(out, cap, used, "\n");
    }
    if (&diting_software_aes_state) {
      wifi_append(out, cap, used, "WIFI_AES state=%d kat=%u reg=%u done=%u nonce=%u unsup=%u\n",
                  diting_software_aes_state,
                  &native_aes_kat_status ? native_aes_kat_status : 0u,
                  &native_aes_registered ? native_aes_registered : 0u,
                  &native_aes_completed ? native_aes_completed : 0u,
                  &native_aes_nonce_key_uses ? native_aes_nonce_key_uses : 0u,
                  &native_aes_unsupported_key ? native_aes_unsupported_key : 0u);
    }

    IOPlatformExpert *pe = IOService::getPlatform();
    IOService *peProv = pe ? pe->getProvider() : nullptr;
    OSString *platUuid = peProv ? OSDynamicCast(OSString, peProv->getProperty(kIOPlatformUUIDKey)) : nullptr;
    wifi_append(out, cap, used, "WIFI_PLATFORM uuid=%s\n",
                (platUuid && platUuid->getCStringNoCopy()) ? platUuid->getCStringNoCopy() : "none");

    char cc[3]; country(cc);
    wifi_append(out, cap, used, "WIFI_STACK opened=%u cmds=%u last=%s if_media=%u if_flags=%u if_other=%u cc=%s if_media_procs=",
                gDiag.stackOpened, gDiag.stackCmds, gDiag.lastStackCmd[0] ? gDiag.lastStackCmd : "none",
                gDiag.ifMedia, gDiag.ifFlags, gDiag.ifOther, cc);
    if (!gDiag.ifMediaProcCount) {
      wifi_append(out, cap, used, "none\n");
    } else {
      for (uint32_t i = 0; i < gDiag.ifMediaProcCount; ++i) {
        wifi_append(out, cap, used, "%s%s", i ? "," : "", gDiag.ifMediaProcs[i]);
      }
      wifi_append(out, cap, used, "\n");
    }
    lck_mtx_lock(gLock);
    wifi_append(out, cap, used, "WIFI_CLIENTS opened=%u procs=", gDiag.ucClientsOpened);
    if (!gDiag.ucClientProcCount) {
      wifi_append(out, cap, used, "none");
    } else {
      for (uint32_t i = 0; i < gDiag.ucClientProcCount; ++i) {
        wifi_append(out, cap, used, "%s%s", i ? "," : "", gDiag.ucClientProcs[i]);
      }
    }
    wifi_append(out, cap, used, "\n");
    wifi_append(out, cap, used, "WIFI_TRACE_COUNT count=%u\n", gDiag.traceCount);
    for (uint32_t i = 0; i < gDiag.traceCount && used + 96 < cap; ++i) {
      const auto &t = gDiag.traces[i];
      wifi_append(out, cap, used, "WIFI_TRACE proc=%s path=%s type=%d op=%s len=%u rc=%d count=%u\n",
                  t.proc, t.isUc ? "uc" : "bsd", t.type, t.isSet ? "S" : "G", t.len, t.rc, t.count);
    }
    lck_mtx_unlock(gLock);
    return used;
  }

  // Default: "wifi" or "wifi status"
  nw_wifi_status st{}; nw_wifi_status_get(&st);
  char ip[24]; format_ipv4(ip);
  struct ifnet_stats_param stats{}; ifnet_stat(gIf, &stats);
  char ssid[33]; unsigned sl = st.ssid_len > 32 ? 32 : st.ssid_len;
  for (unsigned k = 0; k < sl; ++k) ssid[k] = (st.ssid[k] >= 0x20 && st.ssid[k] < 0x7f) ? static_cast<char>(st.ssid[k]) : '?';
  ssid[sl] = 0;
  wifi_append(out, cap, used,
              "WIFI_STATUS ready=1 state=%s(%u) power=%u mac=%02x:%02x:%02x:%02x:%02x:%02x bssid=%02x:%02x:%02x:%02x:%02x:%02x ssid=%s freq=%u rssi=%d channels=%u if_flags=0x%x ipv4=%s\n",
              sta_name(st.state), st.state, gPower ? 1u : 0u,
              gMac[0], gMac[1], gMac[2], gMac[3], gMac[4], gMac[5],
              st.bssid[0], st.bssid[1], st.bssid[2], st.bssid[3], st.bssid[4], st.bssid[5],
              ssid[0] ? ssid : "<none>", st.freq_mhz, st.signal_dbm, st.channels,
              static_cast<unsigned>(ifnet_flags(gIf)), ip);
  wifi_append(out, cap, used,
              "WIFI_RADIO stage=%u scans=%u bss=%d frames=%u beacons=%u last_err=%d assoc_res=%d join_pending=%u\n",
              nw_wifi_stats.stage, nw_wifi_stats.scans, nw_wifi_bss_count(),
              nw_wifi_stats.frames, nw_wifi_stats.beacons, nw_wifi_stats.last_err,
              gAssocResult, __atomic_load_n(&gJoinPending, __ATOMIC_ACQUIRE) ? 1u : 0u);
  wifi_append(out, cap, used,
              "WIFI_TRAFFIC pkts_in=%llu pkts_out=%llu rx_ok=%u rx_err=%u rx_nobuf=%u in_arp=%u in_ip4=%u in_dhcp=%u in_ip6=%u out_arp=%u out_ip4=%u out_dhcp=%u out_ip6=%u\n",
              static_cast<unsigned long long>(stats.packets_in), static_cast<unsigned long long>(stats.packets_out),
              gRxFrames, gRxErrors, gRxNoBuffer,
              gKindsIn.arp, gKindsIn.ip4, gKindsIn.dhcp, gKindsIn.ip6,
              gKindsOut.arp, gKindsOut.ip4, gKindsOut.dhcp, gKindsOut.ip6);
  wifi_append(out, cap, used,
              "WIFI_IOCTL total=%u uc=%u bsd=%u caps=%u scan_req=%u scan_multi=%u scan_res=%u assoc=%u disassoc=%u power_set=%u abort=%u ev_ok=%u ev_err=%u last_ev_err=%d uc_refused=%u(sel=%u,in=%u,out=%u)\n",
              gDiag.total, gDiag.uc, gDiag.bsd, gDiag.caps, gDiag.scanReq, gDiag.scanMulti, gDiag.scanRes,
              gDiag.assoc, gDiag.disassoc, gDiag.powerSet, gDiag.abortScan,
              gDiag.eventsOk, gDiag.eventsErr, gDiag.lastEventErr,
              gDiag.ucRefused, gDiag.lastRefusedSel, gDiag.lastRefusedIn, gDiag.lastRefusedOut);
  if (gDiag.errCount) {
    wifi_append(out, cap, used, "WIFI_ERRORS");
    for (uint32_t i = 0; i < gDiag.errCount; ++i) {
      const auto &e = gDiag.errs[i];
      wifi_append(out, cap, used, " %s:%d(%s,len=%u)->%d[x%u]",
                  e.isUc ? "uc" : "bsd", e.type, e.isSet ? "S" : "G", e.len, e.rc, e.count);
    }
    wifi_append(out, cap, used, "\n");
  }
  return used;
}
