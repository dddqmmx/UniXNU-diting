/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#define DITING_WIFI_NET_HOST_TEST 1
#include "../drivers/DitingWifi/DitingWifiNet.cpp"

static unsigned checks, scan_calls, connect_calls, disconnect_calls;
static int scan_result_code, connect_result_code, tx_result_code;
static unsigned radio_count = 1;
static nw_wifi_status status_snapshot;
static std::vector<nw_bss_info> networks;
static std::vector<DitingWifiChannelInfo> channel_snapshot;
static std::vector<uint8_t> scan_ssid, join_ssid, join_key, transmitted;
static const nw_wifi_events *station_events;
#define CHECK(x) do { checks++; if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } } while (0)

extern "C" void native_wifi_logf(int, const char *, ...) {}
extern "C" void diting_wifi_wake() { host_wakes++; }
extern "C" int diting_kev_post(kev_msg *msg) {
  CHECK(!host_lock_depth);
  auto p = static_cast<const uint8_t *>(msg->dv[0].data_ptr);
  host_events.push_back({msg->kev_class, msg->kev_subclass, msg->event_code,
                        std::vector<uint8_t>(p, p + msg->dv[0].data_length)});
  if (host_event_hook) host_event_hook();
  return host_event_error;
}
extern "C" void nw_wifi_set_events(const nw_wifi_events *ev) { station_events = ev; }
extern "C" void nw_wifi_status_get(nw_wifi_status *st) { *st = status_snapshot; }
extern "C" int nw_wifi_bss_count() { return static_cast<int>(networks.size()); }
extern "C" int nw_wifi_bss_get(int i, nw_bss_info *b) { if (i < 0 || static_cast<size_t>(i) >= networks.size()) return -22; *b = networks[i]; return 0; }
extern "C" int nw_wifi_scan(const uint8_t *ssid, size_t n) { scan_calls++; scan_ssid.clear(); if (n) scan_ssid.assign(ssid, ssid + n); return scan_result_code; }
extern "C" int nw_wifi_connect(const uint8_t *ssid, size_t n, const uint8_t *, const char *key, size_t k) {
  connect_calls++; join_ssid.assign(ssid, ssid + n); join_key.clear(); if (k) join_key.assign(key, key + k); return connect_result_code;
}
extern "C" void nw_wifi_disconnect() { disconnect_calls++; status_snapshot.state = NW_STA_IDLE; }
extern "C" int nw_wifi_tx(const uint8_t *p, size_t n) { transmitted.assign(p, p + n); return tx_result_code; }
extern "C" void nw_wifi_tx_reasons(char *out, int n) { if (n) out[0] = 0; }
extern "C" unsigned nw_wifi_radio_count() { return radio_count; }
extern "C" unsigned nw_wifi_channel_info(DitingWifiChannelInfo *p, unsigned capacity) {
  const unsigned n = std::min(capacity, static_cast<unsigned>(channel_snapshot.size()));
  std::copy_n(channel_snapshot.begin(), n, p); return n;
}
extern "C" unsigned nw_wifi_channel_list(unsigned *p, unsigned capacity) {
  const unsigned n = std::min(capacity, static_cast<unsigned>(channel_snapshot.size()));
  for (unsigned i = 0; i < n; i++) p[i] = channel_snapshot[i].frequency_mhz; return n;
}

static Diting80211UserClient client;
static uint32_t word(const std::vector<uint8_t> &bytes, size_t at) { uint32_t v; std::memcpy(&v, bytes.data() + at, 4); return v; }
static uint16_t half(const std::vector<uint8_t> &bytes, size_t at) { uint16_t v; std::memcpy(&v, bytes.data() + at, 2); return v; }
static DitingWifiControlRecord last_record() {
  const auto &d = diting_wifi_control_diagnostic;
  CHECK(!(d.revision & 1) && d.total_records);
  return d.recent[(d.total_records - 1) % 32];
}
static void recorded(int32_t result, int type, unsigned path, unsigned output) {
  const auto r = last_record();
  CHECK(r.result == result && r.type == type && r.path == path && r.output_bytes == output);
  CHECK(r.sequence == diting_wifi_control_diagnostic.total_records);
  CHECK(!host_lock_depth && host_allocations.empty());
}
struct Native {
  Apple80211MethodRequest packet{};
  std::vector<uint8_t> bytes;
  IOMemoryDescriptor input, output;
  IOExternalMethodArguments args{};
  Native(int type, unsigned cap, bool ool = false, bool set = false) :
    bytes(cap + 32, 0x5a), input{reinterpret_cast<uint8_t *>(&packet), sizeof packet}, output{bytes.data(), cap} {
    packet.command = set ? SIOCSA80211 : SIOCGA80211;
    packet.request.req_type = type; strlcpy(packet.request.req_if_name, "en0", 16);
    // This copied userspace pointer and req_len must never be followed.
    packet.request.req_data = 1; packet.request.req_len = UINT32_MAX;
    if (ool) { args.structureInputDescriptor = &input; args.structureOutputDescriptor = &output; args.structureOutputDescriptorSize = cap; }
    else { args.structureInput = &packet; args.structureInputSize = sizeof packet; args.structureOutput = bytes.data(); args.structureOutputSize = cap; }
  }
  IOReturn run(uint32_t selector = 0) { return client.externalMethod(selector, &args, nullptr, nullptr, nullptr); }
  uint64_t size() const { return args.structureOutputDescriptor ? args.structureOutputDescriptorSize : args.structureOutputSize; }
  void payload(const void *p, unsigned n) { packet.length = n; std::memcpy(packet.bytes, p, n); }
  void guard(unsigned from) { CHECK(std::all_of(bytes.begin() + from, bytes.end(), [](uint8_t b) { return b == 0x5a; })); }
};
static int socket_request(int type, std::vector<uint8_t> &bytes, bool set = false, unsigned length = UINT32_MAX) {
  apple80211req rq{}; rq.req_type = type; rq.req_len = length == UINT32_MAX ? bytes.size() : length;
  rq.req_data = reinterpret_cast<user_addr_t>(bytes.data()); strlcpy(rq.req_if_name, "en0", 16);
  return if_ioctl(gIf, set ? SIOCSA80211 : SIOCGA80211, &rq);
}

static void channels_and_scan() {
  channel_snapshot = {{2412, DITING_CHANNEL_PASSIVE | DITING_CHANNEL_HT40_ABOVE},
                      {5180, DITING_CHANNEL_HT40_BELOW | DITING_CHANNEL_VHT80},
                      {5260, DITING_CHANNEL_INDOOR | DITING_CHANNEL_DFS | DITING_CHANNEL_PASSIVE | DITING_CHANNEL_VHT80},
                      {4920, 0}, {5955, DITING_CHANNEL_VHT80}};
  for (bool ool : {false, true}) {
    Native query(kChannelsInfo, 0x2720, ool);
    CHECK(query.run() == 0 && query.size() == 0x2720);
    recorded(0, kChannelsInfo, DITING_WIFI_CONTROL_NATIVE, 0x2720);
    CHECK(word(query.bytes, 0) == 1 && half(query.bytes, 0x0c) == 4);
    const uint8_t expected[] = {1, 36, 52, 184};
    for (unsigned i = 0; i < 4; i++) {
      CHECK(query.bytes[0x32e + i] == expected[i]);
      CHECK(query.bytes[0x4be + i] == (i == 2));
      CHECK(query.bytes[0x64e + i] == (i == 2));
      CHECK(query.bytes[0x7de + i] == (i == 0 || i == 2));
      CHECK(query.bytes[0x96e + i] == (i < 2));
      CHECK(query.bytes[0xafe + i] == (i == 1 || i == 2));
      CHECK(word(query.bytes, 0x12d0 + 4 * i) == 0 && query.bytes[0x1aa0 + i] == 0);
    }
    CHECK(query.bytes[0x332] == 0); query.guard(0x2720);
    if (ool) CHECK(query.input.prepares == 1 && query.input.completes == 1 && query.output.prepares == 1 && query.output.completes == 1);
    Native radio(kRadioInfo, 8, ool);
    CHECK(radio.run() == 0 && radio.size() == 8 && word(radio.bytes, 0) == 1 && word(radio.bytes, 4) == 1);
    radio.guard(8);
  }
  radio_count = 0; Native absent(kRadioInfo, 8); CHECK(absent.run() == 0 && word(absent.bytes, 4) == 0); radio_count = 1;
  std::vector<uint8_t> big(0x3c00 + 32, 0x5a);
  CHECK(socket_request(kChannelsInfo, big, false, 0x3c00) == 0);
  recorded(0, kChannelsInfo, DITING_WIFI_CONTROL_SOCKET, 0x2720);
  CHECK(half(big, 0x0c) == 4 && std::all_of(big.begin() + 0x2720, big.end(), [](uint8_t b) { return b == 0x5a; }));
  Native legacyChannels(kSupportedChannels, 4824);
  CHECK(legacyChannels.run() == 0 && word(legacyChannels.bytes, 4) == 4 && word(legacyChannels.bytes, 8 + 3 * 12 + 4) == 184);
  channel_snapshot.assign(420, {5180, DITING_CHANNEL_VHT80});
  Native maximum(kChannelsInfo, 0x2720); CHECK(maximum.run() == 0 && half(maximum.bytes, 0x0c) == 400);
  CHECK(maximum.bytes[0x32e + 399] == 36 && maximum.bytes[0xafe + 399] == 1); maximum.guard(0x2720);
  channel_snapshot.clear(); Native empty(kChannelsInfo, 0x2720); CHECK(empty.run() == 0 && half(empty.bytes, 0x0c) == 0);
  channel c; CHECK(fill_channel(c, 2484) && c.channel == 14); CHECK(!fill_channel(c, 4900) && c.channel == 0);
  CHECK(!fill_channel(c, 0) && c.flags == 0); CHECK(!fill_channel(c, 2413)); CHECK(!fill_channel(c, 5955));

  nw_bss_info b{}; b.freq_mhz = 2412; b.signal_dbm = -51; b.beacon_int = 100; b.caps = 0x431;
  std::memcpy(b.bssid, "\x02\x11\x22\x33\x44\x55", 6); std::memcpy(b.ssid, "Test", 4); b.ssid_len = 4; b.age_ms = 1234;
  const uint8_t ies[] = {0, 4, 'T', 'e', 's', 't', 7, 3, 'C', 'N', ' '};
  std::memcpy(b.ie, ies, sizeof ies); b.ie_len = sizeof ies; networks = {b};
  for (bool ool : {false, true}) {
    gScanCursor = 0; Native scan(kScanResult, 0x8d8, ool); scan.packet.length = 0x8d8;
    CHECK(scan.run() == 0 && scan.size() == 0x8d8);
    CHECK(scan.bytes[0x60] == 4 && !std::memcmp(scan.bytes.data() + 0x61, "Test", 4));
    CHECK(word(scan.bytes, 0x84) == 1234 && half(scan.bytes, 0x8a) == sizeof ies);
    CHECK(!std::memcmp(scan.bytes.data() + 0x8c, ies, sizeof ies));
    CHECK(std::all_of(scan.bytes.begin() + 1164, scan.bytes.begin() + 0x8d8, [](uint8_t v) { return v == 0; }));
    scan.guard(0x8d8); recorded(0, kScanResult, DITING_WIFI_CONTROL_NATIVE, 0x8d8);
    Native end(kScanResult, 0x8d8, ool); CHECK(end.run() == EIO); recorded(EIO, kScanResult, DITING_WIFI_CONTROL_NATIVE, 0);
  }
  gScanCursor = 0; Native old(kScanResult, 1164); CHECK(old.run() == 0 && old.size() == 1164); old.guard(1164);
  gScanCursor = 0; std::vector<uint8_t> socketScan(0x8d8); CHECK(socket_request(kScanResult, socketScan) == 0);
  recorded(0, kScanResult, DITING_WIFI_CONTROL_SOCKET, 0x8d8);
  status_snapshot.state = NW_STA_UP; std::memcpy(status_snapshot.bssid, b.bssid, 6);
  Native current(kCurrentNetwork, 0x8d8); CHECK(current.run() == 0 && current.size() == 0x8d8 && current.bytes[0x60] == 4);
  Native cc(kCountryCode, 3); CHECK(cc.run() == 0 && !std::memcmp(cc.bytes.data(), "CN\0", 3));
  networks[0].ssid_len = 255; networks[0].ie_len = UINT16_MAX; std::memset(networks[0].ie, 0xa7, sizeof b.ie);
  gScanCursor = 0; Native bounded(kScanResult, 0x8d8); CHECK(bounded.run() == 0);
  CHECK(bounded.bytes[0x60] == 32 && half(bounded.bytes, 0x8a) == 384);
  CHECK(bounded.bytes[0x8c + 383] == 0xa7 && bounded.bytes[0x8c + 384] == 0);
  Native boundedCountry(kCountryCode, 3); CHECK(boundedCountry.run() == 0 && !std::memcmp(boundedCountry.bytes.data(), "XX\0", 3));
  networks = {b}; status_snapshot.state = NW_STA_IDLE;
}

static void controls_and_events() {
  for (unsigned selector : {1u, 5u, 7u}) {
    uint8_t supported = 0xa5; IOExternalMethodArguments args{}; args.structureOutput = &supported; args.structureOutputSize = 1;
    CHECK(client.externalMethod(selector, &args, nullptr, nullptr, nullptr) == 0 && supported == (selector == 1));
    recorded(0, -1, DITING_WIFI_CONTROL_NATIVE, 1);
    args.structureInputSize = 1; CHECK(client.externalMethod(selector, &args, nullptr, nullptr, nullptr) == kIOReturnBadArgument);
  }
  uint32_t power[6] = {1, 1, 0, 0, 0, 0};
  Native off(kPower, 0, true, true); off.payload(power, sizeof power);
  CHECK(off.run() == 0 && !gPower && disconnect_calls == 1 && host_events.empty());
  recorded(0, kPower, DITING_WIFI_CONTROL_NATIVE, 0); CHECK((last_record().flags & 3) == 1);
  unsigned reentries = 0;
  host_event_hook = [&] { Native state(kPower, 24); CHECK(state.run() == 0 && word(state.bytes, 8) == static_cast<unsigned>(gPower)); reentries++; };
  host_deliver_calls(); CHECK(reentries == 3 && host_events.size() == 3);
  CHECK(host_events[0].code == KEV_DL_LINK_OFF && host_events[1].code == kEvLinkChanged && host_events[2].code == kEvPowerChanged);
  CHECK(host_events[2].payload.size() == 16 && !std::memcmp(host_events[2].payload.data(), "en0", 4));
  Native scanOff(kScanReq, 0, false, true); uint8_t scan[0x80]{}; scanOff.payload(scan, 20); CHECK(scanOff.run() == ENETDOWN);
  power[2] = 1; std::vector<uint8_t> on(reinterpret_cast<uint8_t *>(power), reinterpret_cast<uint8_t *>(power) + sizeof power);
  CHECK(socket_request(kPower, on, true) == 0 && gPower); recorded(0, kPower, DITING_WIFI_CONTROL_SOCKET, 0);
  host_event_error = EIO; host_deliver_calls(); CHECK(last_record().path == DITING_WIFI_CONTROL_EVENT && last_record().result == EIO);
  host_event_error = 0; host_event_hook = {};
  Native scanRequest(kScanReq, 0, true, true); uint32_t n = 4; std::memcpy(scan + 16, &n, 4); std::memcpy(scan + 20, "Test", 4); scanRequest.payload(scan, 24);
  CHECK(scanRequest.run() == 0 && scan_calls == 1 && scan_ssid == std::vector<uint8_t>({'T','e','s','t'}));
  station_events->scan_done(); CHECK(last_record().path == DITING_WIFI_CONTROL_EVENT && last_record().selector == kEvScanDone);
  scan_result_code = -16; CHECK(scanRequest.run() == EBUSY && !gScanning); scan_result_code = 0;
  n = 33; std::memcpy(scan + 16, &n, 4); scanRequest.payload(scan, 24); CHECK(scanRequest.run() == EINVAL && scan_calls == 2);
  assoc_request24 assoc{}; assoc.version = 1; assoc.auth_lower = 1; assoc.auth_upper = 8; assoc.ssid_length = 4; std::memcpy(assoc.ssid, "Test", 4);
  assoc.key_version = 1; assoc.key_length = 32; assoc.key_cipher = 6; std::memset(assoc.key, 0xab, 32);
  Native join(kAssociate, 0, true, true); join.payload(&assoc, sizeof assoc); CHECK(join.run() == 0 && connect_calls == 1 && join_key.size() == 64);
  for (unsigned i = 0; i < 64; i++) CHECK(join_key[i] == (i % 2 ? 'b' : 'a'));
  CHECK(join_ssid == std::vector<uint8_t>({'T','e','s','t'}));
  assoc.auth_upper = 0x40000; join.payload(&assoc, sizeof assoc); CHECK(join.run() == EOPNOTSUPP && connect_calls == 1);
  assoc.auth_upper = 8; assoc.ssid_length = 33; join.payload(&assoc, sizeof assoc); CHECK(join.run() == EINVAL && connect_calls == 1);
  assoc_request old{}; old.version = 1; old.auth_lower = 1; old.ssid_length = 4; std::memcpy(old.ssid, "Test", 4);
  join.payload(&old, sizeof old); CHECK(join.run() == 0 && connect_calls == 2 && join_key.empty());
  connect_result_code = -95; CHECK(join.run() == EOPNOTSUPP); connect_result_code = 0;
  IO80211Interface interface; IOUserClient *bound = nullptr;
  CHECK(interface.newUserClient(nullptr, nullptr, 1, nullptr, &bound) == kIOReturnUnsupported && !bound);
  CHECK(interface.newUserClient(nullptr, nullptr, 0, nullptr, &bound) == 0 && bound);
  CHECK(bound->clientClose() == 0); bound->release();
}

static void rejected_requests() {
  CHECK(client.externalMethod(0, nullptr, nullptr, nullptr, nullptr) == kIOReturnBadArgument);
  Native q(kPower, 24, true);
  q.args.scalarInputCount = 1; CHECK(q.run() == kIOReturnBadArgument); q.args.scalarInputCount = 0;
  q.args.scalarOutputCount = 1; CHECK(q.run() == kIOReturnBadArgument); q.args.scalarOutputCount = 0;
  CHECK(q.run(3) == kIOReturnUnsupported); recorded(static_cast<int32_t>(kIOReturnUnsupported), -1, DITING_WIFI_CONTROL_NATIVE, 0);
  q.input.length--; CHECK(q.run() == kIOReturnBadArgument); q.input.length++;
  q.input.length += 1ULL << 32; CHECK(q.run() == kIOReturnBadArgument && !q.input.prepares); q.input.length -= 1ULL << 32;
  q.args.structureOutputDescriptorSize = (1ULL << 32) + 24; CHECK(q.run() == kIOReturnBadArgument); q.args.structureOutputDescriptorSize = 24;
  q.args.structureOutputDescriptorSize = 0x3c01; CHECK(q.run() == kIOReturnBadArgument); q.args.structureOutputDescriptorSize = 24;
  q.packet.command = 0; CHECK(q.run() == kIOReturnBadArgument); q.packet.command = SIOCGA80211;
  q.packet.length = 0x3c01; CHECK(q.run() == kIOReturnBadArgument); q.packet.length = 25; CHECK(q.run() == kIOReturnBadArgument); q.packet.length = 0;
  strlcpy(q.packet.request.req_if_name, "en1", 16); CHECK(q.run() == kIOReturnBadArgument); strlcpy(q.packet.request.req_if_name, "en0", 16);
  q.input.prepare_result = kIOReturnError; const unsigned completed = q.input.completes;
  CHECK(q.run() == kIOReturnError && q.input.completes == completed); q.input.prepare_result = 0;
  q.input.short_read = true; CHECK(q.run() == kIOReturnNotReadable && !q.input.wired); q.input.short_read = false;
  q.input.complete_result = kIOReturnError; CHECK(q.run() == kIOReturnError && !q.input.wired); q.input.complete_result = 0;
  q.output.prepare_result = kIOReturnError; CHECK(q.run() == kIOReturnError && !q.output.completes); q.output.prepare_result = 0;
  q.output.short_write = true; CHECK(q.run() == kIOReturnNotWritable && !q.output.wired); q.output.short_write = false;
  recorded(static_cast<int32_t>(kIOReturnNotWritable), kPower, DITING_WIFI_CONTROL_NATIVE, 0);
  q.output.complete_result = kIOReturnError; CHECK(q.run() == kIOReturnError && !q.output.wired); q.output.complete_result = 0;
  host_alloc_failures = 1; CHECK(q.run() == kIOReturnNoMemory); recorded(static_cast<int32_t>(kIOReturnNoMemory), -1, DITING_WIFI_CONTROL_NATIVE, 0);
  Native missing(kPower, 24); missing.args.structureInput = nullptr; CHECK(missing.run() == kIOReturnBadArgument);
  missing.args.structureInput = &missing.packet; missing.args.structureOutput = nullptr; CHECK(missing.run() == kIOReturnBadArgument);
  Native unsupported(250, 8); CHECK(unsupported.run() == 45); recorded(45, 250, DITING_WIFI_CONTROL_NATIVE, 0);
  std::vector<uint8_t> data(24);
  host_copyin_errors = 1; CHECK(socket_request(kPower, data, true) == EFAULT); recorded(EFAULT, kPower, DITING_WIFI_CONTROL_SOCKET, 0);
  host_copyout_errors = 1; CHECK(socket_request(kPower, data) == EFAULT); recorded(EFAULT, kPower, DITING_WIFI_CONTROL_SOCKET, 0);
  host_alloc_failures = 1; CHECK(socket_request(kPower, data) == ENOMEM); recorded(ENOMEM, kPower, DITING_WIFI_CONTROL_SOCKET, 0);
  CHECK(socket_request(kPower, data, false, 0x3c01) == EINVAL); recorded(EINVAL, kPower, DITING_WIFI_CONTROL_SOCKET, 0);
  CHECK(if_ioctl(gIf, SIOCGA80211, nullptr) == EINVAL); recorded(EINVAL, -1, DITING_WIFI_CONTROL_SOCKET, 0);
  apple80211req nullBuffer{}; nullBuffer.req_type = kPower; nullBuffer.req_len = 24;
  CHECK(if_ioctl(gIf, SIOCGA80211, &nullBuffer) == EINVAL); recorded(EINVAL, kPower, DITING_WIFI_CONTROL_SOCKET, 0);
  ifmediareq media{}; CHECK(if_ioctl(gIf, SIOCGIFMEDIA, &media) == 0 && media.ifm_count == 1);
  int entry = -1; media.ifm_ulist = &entry; CHECK(if_ioctl(gIf, SIOCGIFMEDIA, &media) == 0 && entry == (IFM_IEEE80211 | IFM_AUTO));
  apple80211req scalar{}; scalar.req_type = kState;
  CHECK(if_ioctl(gIf, SIOCGA80211, &scalar) == 0 && scalar.req_val == 0);
  Native nativeScalar(kState, 4); CHECK(nativeScalar.run() == 0 && word(nativeScalar.bytes, 0) == 0); recorded(0, kState, DITING_WIFI_CONTROL_NATIVE, 4);
}

static void traffic_and_failure() {
  std::vector<uint8_t> frame(1500, 0x69); frame[12] = 0x86; frame[13] = 0xdd;
  station_events->rx(frame.data(), frame.size());
  CHECK(host_bpf_rx == frame && host_input_payload == std::vector<uint8_t>(frame.begin() + 14, frame.end()));
  CHECK(!host_mbuf_count && gRxFrames == 1 && gRxChains == 1);
  mbuf_t mbuf = nullptr; CHECK(!mbuf_allocpacket(0, frame.size(), nullptr, &mbuf));
  size_t at = 0; for (auto m = mbuf; m; m = m->next) { const size_t n = std::min(m->storage.size(), frame.size() - at); std::memcpy(mbuf_data(m), frame.data() + at, n); mbuf_setlen(m, n); at += n; }
  mbuf_pkthdr_setlen(mbuf, frame.size()); tx_result_code = -105;
  CHECK(if_output(gIf, mbuf) == 55 && host_bpf_tx == frame && !host_mbuf_count);
  tx_result_code = 0;
  gScanning = 1; const unsigned before = host_events.size(); diting_wifi_net_failed();
  CHECK(gFailed && !gPower && !gScanning && host_events.size() == before + 4);
  Native power(kPower, 24); CHECK(power.run() == 0 && word(power.bytes, 8) == 0);
  Native state(kState, 8); CHECK(state.run() == 50); recorded(50, kState, DITING_WIFI_CONTROL_NATIVE, 0);
  uint32_t enable[6] = {1, 1, 1}; Native set(kPower, 0, false, true); set.payload(enable, sizeof enable);
  CHECK(set.run() == 50 && !gPower); recorded(50, kPower, DITING_WIFI_CONTROL_NATIVE, 0); CHECK((last_record().flags & 5) == 5);
  gFailed = false; gPower = true;
}

static void bounded_history() {
  const auto first = diting_wifi_control_diagnostic.first[0];
  const unsigned total = diting_wifi_control_diagnostic.total_records;
  const unsigned failed = diting_wifi_control_diagnostic.failed_records;
  for (unsigned i = 0; i < 70; i++) { Native request(250, 8); CHECK(request.run() == 45); }
  const auto &d = diting_wifi_control_diagnostic;
  CHECK(d.version == 1 && d.byte_size == 2688 && d.total_records == total + 70 && d.failed_records == failed + 70);
  CHECK(!std::memcmp(&first, &d.first[0], sizeof first) && d.first[31].sequence == 32);
  for (unsigned i = 0; i < 32; i++) { const unsigned seq = d.total_records - i; const auto &r = d.recent[(seq - 1) % 32]; CHECK(r.sequence == seq && r.type == 250 && r.result == 45); }
  for (unsigned i = 0; i < 16; i++) CHECK(d.failures[i].sequence > d.total_records - 16 && d.failures[i].result == 45);
  CHECK(d.native_requests + d.socket_requests + d.event_posts == d.total_records);
  CHECK(d.native_types[kChannelsInfo / 32] & (1u << (kChannelsInfo % 32)));
  CHECK(d.socket_types[kChannelsInfo / 32] & (1u << (kChannelsInfo % 32)));
  CHECK(d.selectors[1] == 2 && d.selectors[5] == 2 && d.selectors[7] == 2 && d.event_posts > 0);
  CHECK(!(d.revision & 1) && host_max_allocation <= sizeof(Apple80211MethodRequest));
}

int main(int argc, char **argv) {
  CHECK(diting_wifi_net_prepare() && station_events && client.initWithTask(nullptr, nullptr, 0, nullptr));
  HostIfnet net; gIf = &net; status_snapshot.state = NW_STA_IDLE;
  channels_and_scan(); controls_and_events(); rejected_requests(); traffic_and_failure(); bounded_history();
  CHECK(host_allocations.empty() && !host_mbuf_count && !host_lock_depth);
  if (argc == 2) {
    FILE *file = std::fopen(argv[1], "wb"); CHECK(file);
    CHECK(std::fwrite(&diting_wifi_control_diagnostic, sizeof diting_wifi_control_diagnostic, 1, file) == 1);
    CHECK(std::fclose(file) == 0);
  }
  for (auto call : host_calls) delete call; host_calls.clear(); gNotifications = gCachedScan = nullptr;
  delete gLock; delete gGrp; gLock = nullptr; gGrp = nullptr; gIf = nullptr;
  std::printf("PASS: %u actual network/Apple80211 ABI checks, ASan/UBSan\n", checks);
}
