// SPDX-License-Identifier: BSD-2-Clause-Patent
// Apple80211 wire protocol helpers shared by DitingWifiNet.cpp and host tests.
// Covers iPadOS 18 (22H374) and iOS 27 (24B156) scan capabilities, multi-SSID
// scan requests, 0x8e8 timestamped scan results, 592/656/900-byte association
// requests, and 540-byte iOS 27 asynchronous association completion events.
#ifndef DITING_WIFI_PROTOCOL27_H
#define DITING_WIFI_PROTOCOL27_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define DITING_WIFI_CAP_NAMED_SCAN 22u
#define DITING_WIFI_CAP_SCAN_DONE_EVENT 46u

#define DITING_WIFI_SCAN_MULTIPLE_BYTES 0x14e8u
#define DITING_WIFI_SCAN_SSID_COUNT 0x8u
#define DITING_WIFI_SCAN_SSIDS 0xcu
#define DITING_WIFI_SCAN_SSID_ENTRY 40u
#define DITING_WIFI_SCAN_SSID_MAX 10u
#define DITING_WIFI_SCAN_TYPE_SINGLE 0x34u
#define DITING_WIFI_SCAN_TYPE_MULTI 0x200u
#define DITING_WIFI_SCAN_TYPE_CACHED 3u
#define DITING_WIFI_CACHED_SCAN_DELAY_MS 50u
#define DITING_WIFI_JOINED_SCAN_DELAY_MS 2000u
#define DITING_WIFI_AUTH_EMPTY_SCAN_DELAY_MS 500u

enum diting_wifi_scan_action {
    DITING_WIFI_SCAN_ACTION_AIR = 0,
    DITING_WIFI_SCAN_ACTION_CACHED_EVENT = 1,
};

#define DITING_WIFI_SCAN_RESULT_BYTES 0x8e8u
#define DITING_WIFI_SCAN_ELEMENTS 0x8cu
#define DITING_WIFI_SCAN_ELEMENTS_BYTES 2048u
#define DITING_WIFI_SCAN_TIMESTAMP 0x8d0u

struct diting_wifi_channel {
    uint32_t version, channel, flags;
};

struct diting_wifi_scan_result {
    uint32_t version;
    struct diting_wifi_channel ch;
    uint16_t noise_known;
    int16_t noise, snr, rssi;
    uint16_t beacon_interval, capability;
    uint8_t bssid[6];
    uint8_t rate_count, unused0;
    uint32_t rates[15];
    uint8_t ssid_length, ssid[32], unused1[3];
    uint32_t age_ms;
    uint8_t flags, unused2;
    uint16_t elements_length;
    uint8_t elements[DITING_WIFI_SCAN_ELEMENTS_BYTES];
    uint8_t unused3[68];
    uint64_t timestamp;
    uint8_t unused4[16];
};

struct diting_wifi_scan_multiple {
    uint32_t named;
    uint32_t wildcard;
    uint32_t scan_type;
    uint8_t length[DITING_WIFI_SCAN_SSID_MAX];
    uint8_t ssid[DITING_WIFI_SCAN_SSID_MAX][32];
};

struct diting_wifi_assoc_params {
    uint32_t ssid_length;
    uint8_t ssid[32];
    uint8_t bssid[6], unused[2];
    uint32_t key_version, key_length, key_cipher;
    uint16_t key_flags, key_index;
    uint8_t key[64];
};

struct diting_wifi_assoc_req_legacy {
    uint32_t version;
    uint16_t mode, auth_lower, auth_upper, unused;
    struct diting_wifi_assoc_params network;
    uint8_t rest[0x250 - 0x88];
};

struct diting_wifi_assoc_req27 {
    uint32_t version;
    uint16_t mode, unused;
    uint32_t auth_lower, auth_upper;
    struct diting_wifi_assoc_params network;
    uint8_t rest[0x290 - 0x8c];
};

struct diting_wifi_assoc_req24 {
    uint32_t version;
    uint16_t mode, unused;
    uint32_t auth_lower, auth_upper;
    struct diting_wifi_assoc_params network;
    uint8_t rest[0x384 - 0x8c];
};

struct diting_wifi_assoc_event27 {
    uint8_t attempts[10][52];
    uint32_t reserved[2], reason, result, has_result;
};

#ifdef __cplusplus
static_assert(sizeof(diting_wifi_scan_result) == DITING_WIFI_SCAN_RESULT_BYTES &&
              __builtin_offsetof(diting_wifi_scan_result, ssid_length) == 0x60 &&
              __builtin_offsetof(diting_wifi_scan_result, age_ms) == 0x84 &&
              __builtin_offsetof(diting_wifi_scan_result, elements) == DITING_WIFI_SCAN_ELEMENTS &&
              __builtin_offsetof(diting_wifi_scan_result, timestamp) == DITING_WIFI_SCAN_TIMESTAMP,
              "scan result layout");
static_assert(sizeof(diting_wifi_assoc_params) == 0x7c &&
              __builtin_offsetof(diting_wifi_assoc_params, key_length) == 0x30 &&
              __builtin_offsetof(diting_wifi_assoc_params, key_cipher) == 0x34 &&
              __builtin_offsetof(diting_wifi_assoc_params, key) == 0x3c,
              "assoc params layout");
static_assert(sizeof(diting_wifi_assoc_req_legacy) == 592 &&
              __builtin_offsetof(diting_wifi_assoc_req_legacy, network) == 0x0c,
              "legacy assoc layout");
static_assert(sizeof(diting_wifi_assoc_req27) == 656 &&
              __builtin_offsetof(diting_wifi_assoc_req27, network) == 0x10,
              "ios27 assoc layout");
static_assert(sizeof(diting_wifi_assoc_req24) == 900 &&
              __builtin_offsetof(diting_wifi_assoc_req24, network) == 0x10,
              "ipados18 assoc layout");
static_assert(sizeof(diting_wifi_assoc_event27) == 540 &&
              __builtin_offsetof(diting_wifi_assoc_event27, result) == 0x214 &&
              __builtin_offsetof(diting_wifi_assoc_event27, has_result) == 0x218,
              "ios27 assoc event layout");
#endif

static inline uint32_t diting_wifi_u32(const uint8_t *at) {
    uint32_t v;
    memcpy(&v, at, 4);
    return v;
}

static inline void diting_wifi_caps_fill(uint8_t *caps, size_t cap_bytes) {
    static const uint8_t has[] = {
        2, 3, 6, 9, 10, 14, 17, 19,
        DITING_WIFI_CAP_NAMED_SCAN,
        DITING_WIFI_CAP_SCAN_DONE_EVENT
    };
    memset(caps, 0, cap_bytes);
    for (size_t i = 0; i < sizeof(has); ++i) {
        const size_t byte = has[i] / 8u;
        if (byte < cap_bytes) caps[byte] |= (uint8_t)(1u << (has[i] % 8u));
    }
}

static inline int diting_wifi_scan_multiple_parse(const uint8_t *req, size_t size,
                                                  struct diting_wifi_scan_multiple *out) {
    memset(out, 0, sizeof(*out));
    if (!req || size < DITING_WIFI_SCAN_TYPE_MULTI + 4u) return -1;
    const uint32_t count = diting_wifi_u32(req + DITING_WIFI_SCAN_SSID_COUNT);
    if (count > DITING_WIFI_SCAN_SSID_MAX) return -1;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *entry = req + DITING_WIFI_SCAN_SSIDS + i * DITING_WIFI_SCAN_SSID_ENTRY;
        const uint32_t length = diting_wifi_u32(entry + 4u);
        if (length > 32u) return -1;
        if (!length) {
            out->wildcard = 1;
            continue;
        }
        out->length[out->named] = (uint8_t)length;
        memcpy(out->ssid[out->named], entry + 8u, length);
        ++out->named;
    }
    out->scan_type = diting_wifi_u32(req + DITING_WIFI_SCAN_TYPE_MULTI);
    return 0;
}

static inline void diting_wifi_scan_multiple_target(const struct diting_wifi_scan_multiple *s,
                                                    const uint8_t **ssid, uint32_t *length) {
    if (s->named == 1 && !s->wildcard) {
        *ssid = s->ssid[0];
        *length = s->length[0];
        return;
    }
    *ssid = NULL;
    *length = 0;
}

/*
 * Because kCardCaps advertises DITING_WIFI_CAP_SCAN_DONE_EVENT (bit 46),
 * iOS 27 IO80211/CoreWiFi waits asynchronously for kEvScanDone after EVERY
 * kScanReq/kScanReqMultiple that returns 0 — including scan_type == 3
 * (cached results). Returning 0 without posting kEvScanDone deadlocks wifid's
 * CoreWiFi scan worker indefinitely.
 */
static inline enum diting_wifi_scan_action diting_wifi_plan_scan(
        uint32_t scan_type, int is_auth_or_joined, int bss_count, uint32_t *delay_ms_out) {
    enum diting_wifi_scan_action action = DITING_WIFI_SCAN_ACTION_AIR;
    uint32_t delay_ms = 0u;
    if (scan_type == DITING_WIFI_SCAN_TYPE_CACHED && bss_count > 0) {
        action = DITING_WIFI_SCAN_ACTION_CACHED_EVENT;
        delay_ms = DITING_WIFI_CACHED_SCAN_DELAY_MS;
    } else if (is_auth_or_joined) {
        action = DITING_WIFI_SCAN_ACTION_CACHED_EVENT;
        delay_ms = bss_count > 0 ? DITING_WIFI_JOINED_SCAN_DELAY_MS
                                 : DITING_WIFI_AUTH_EMPTY_SCAN_DELAY_MS;
    }
    if (delay_ms_out) *delay_ms_out = delay_ms;
    return action;
}

static inline uint64_t diting_wifi_scan_timestamp(uint64_t now_ns, uint32_t age_ms) {
    const uint64_t age_ns = (uint64_t)age_ms * 1000000ULL;
    return now_ns > age_ns ? now_ns - age_ns : 0ULL;
}

static inline uint16_t diting_wifi_scan_elements_room(uint32_t request_bytes) {
    if (request_bytes <= DITING_WIFI_SCAN_ELEMENTS) return 0;
    const uint32_t room = request_bytes - DITING_WIFI_SCAN_ELEMENTS;
    return (uint16_t)(room < DITING_WIFI_SCAN_ELEMENTS_BYTES ? room : DITING_WIFI_SCAN_ELEMENTS_BYTES);
}

static inline void diting_wifi_scan_result_stamp(struct diting_wifi_scan_result *r,
                                                 const uint8_t *elements, size_t length,
                                                 uint32_t age_ms, uint64_t now_ns,
                                                 uint32_t request_bytes) {
    const uint16_t room = diting_wifi_scan_elements_room(request_bytes);
    r->age_ms = age_ms;
    r->timestamp = diting_wifi_scan_timestamp(now_ns, age_ms);
    r->elements_length = (uint16_t)(length < room ? length : room);
    if (r->elements_length && elements) memcpy(r->elements, elements, r->elements_length);
}

static inline int diting_wifi_is_2g_or_5g_freq(uint32_t mhz) {
    if (mhz == 2484u) return 1;
    if (mhz >= 2412u && mhz <= 2472u && ((mhz - 2407u) % 5u) == 0u) return 1;
    if (mhz >= 5180u && mhz <= 5885u && ((mhz - 5000u) % 5u) == 0u) return 1;
    return 0;
}

static inline int diting_wifi_fill_channel(struct diting_wifi_channel *c, uint32_t mhz) {
    if (!c || !diting_wifi_is_2g_or_5g_freq(mhz)) return -1;
    c->version = 1u;
    if (mhz >= 5180u) {
        c->channel = (mhz - 5000u) / 5u;
        c->flags = 0x2u | 0x10u;
    } else {
        c->channel = mhz == 2484u ? 14u : (mhz - 2407u) / 5u;
        c->flags = 0x2u | 0x8u;
    }
    return 0;
}

static inline int diting_wifi_decode_assoc(const void *raw, size_t len,
                                           struct diting_wifi_assoc_params *out,
                                           uint32_t *auth_lower_out,
                                           uint32_t *auth_upper_out,
                                           int *is_modern_out) {
    if (!raw || !out) return -1;
    memset(out, 0, sizeof(*out));
    uint32_t version = 0, al = 0, au = 0;
    int modern = 0;
    if (len == sizeof(struct diting_wifi_assoc_req_legacy)) {
        struct diting_wifi_assoc_req_legacy req;
        memcpy(&req, raw, sizeof(req));
        version = req.version;
        al = req.auth_lower;
        au = req.auth_upper;
        *out = req.network;
        modern = 0;
    } else if (len == sizeof(struct diting_wifi_assoc_req27)) {
        struct diting_wifi_assoc_req27 req;
        memcpy(&req, raw, sizeof(req));
        version = req.version;
        al = req.auth_lower;
        au = req.auth_upper;
        *out = req.network;
        modern = 1;
    } else if (len == sizeof(struct diting_wifi_assoc_req24)) {
        struct diting_wifi_assoc_req24 req;
        memcpy(&req, raw, sizeof(req));
        version = req.version;
        al = req.auth_lower;
        au = req.auth_upper;
        *out = req.network;
        modern = 1;
    } else {
        return -1;
    }
    if (version != 1u || !out->ssid_length || out->ssid_length > 32u || out->key_length > 64u) {
        memset(out, 0, sizeof(*out));
        return -1;
    }
    if (auth_lower_out) *auth_lower_out = al;
    if (auth_upper_out) *auth_upper_out = au;
    if (is_modern_out) *is_modern_out = modern;
    return 0;
}

#endif
