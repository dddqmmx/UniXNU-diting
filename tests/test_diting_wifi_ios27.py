#!/usr/bin/env python3
"""Host unit tests for Diting iOS 27 (24B156) Apple80211 Wi-Fi protocol helpers."""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

HARNESS = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "DitingWifiProtocol27.h"

static void test_caps(void) {
    uint8_t caps[23];
    diting_wifi_caps_fill(caps, sizeof(caps));
    const unsigned required[] = {2, 3, 6, 9, 10, 14, 17, 19, 22, 46};
    for (unsigned b : required) {
        assert((caps[b / 8] & (1u << (b % 8))) != 0);
    }
    for (size_t i = 6; i < sizeof(caps); ++i) {
        assert(caps[i] == 0);
    }
}

static void test_scan_multiple(void) {
    uint8_t req[DITING_WIFI_SCAN_MULTIPLE_BYTES];
    memset(req, 0, sizeof(req));
    uint32_t count = 2;
    memcpy(req + DITING_WIFI_SCAN_SSID_COUNT, &count, 4);
    // Entry 0: named SSID "TestAP"
    uint32_t ver = 1, len = 6;
    memcpy(req + DITING_WIFI_SCAN_SSIDS + 0, &ver, 4);
    memcpy(req + DITING_WIFI_SCAN_SSIDS + 4, &len, 4);
    memcpy(req + DITING_WIFI_SCAN_SSIDS + 8, "TestAP", 6);
    // Entry 1: wildcard (length 0)
    memcpy(req + DITING_WIFI_SCAN_SSIDS + 40 + 0, &ver, 4);
    uint32_t scan_type = DITING_WIFI_SCAN_TYPE_CACHED;
    memcpy(req + DITING_WIFI_SCAN_TYPE_MULTI, &scan_type, 4);

    struct diting_wifi_scan_multiple parsed;
    assert(diting_wifi_scan_multiple_parse(req, sizeof(req), &parsed) == 0);
    assert(parsed.named == 1);
    assert(parsed.wildcard == 1);
    assert(parsed.scan_type == DITING_WIFI_SCAN_TYPE_CACHED);
    const uint8_t *target = nullptr;
    uint32_t tlen = 99;
    diting_wifi_scan_multiple_target(&parsed, &target, &tlen);
    assert(target == nullptr && tlen == 0);

    // Single named SSID without wildcard
    count = 1;
    memcpy(req + DITING_WIFI_SCAN_SSID_COUNT, &count, 4);
    assert(diting_wifi_scan_multiple_parse(req, sizeof(req), &parsed) == 0);
    assert(parsed.named == 1 && parsed.wildcard == 0);
    diting_wifi_scan_multiple_target(&parsed, &target, &tlen);
    assert(target != nullptr && tlen == 6 && memcmp(target, "TestAP", 6) == 0);

    // Reject too many entries
    count = DITING_WIFI_SCAN_SSID_MAX + 1;
    memcpy(req + DITING_WIFI_SCAN_SSID_COUNT, &count, 4);
    assert(diting_wifi_scan_multiple_parse(req, sizeof(req), &parsed) == -1);
}

static void test_scan_result_timestamp(void) {
    struct diting_wifi_scan_result res;
    memset(&res, 0, sizeof(res));
    uint8_t ies[300];
    memset(ies, 0x5a, sizeof(ies));
    const uint64_t now_ns = 90000000000ULL;
    diting_wifi_scan_result_stamp(&res, ies, sizeof(ies), 2500u, now_ns, DITING_WIFI_SCAN_RESULT_BYTES);
    assert(res.age_ms == 2500u);
    assert(res.timestamp == now_ns - 2500000000ULL);
    assert(res.elements_length == 300u);
    assert(res.elements[299] == 0x5a);

    // Short buffer truncates IE room cleanly
    memset(&res, 0, sizeof(res));
    diting_wifi_scan_result_stamp(&res, ies, sizeof(ies), 100u, now_ns, DITING_WIFI_SCAN_ELEMENTS + 16u);
    assert(res.elements_length == 16u);
    assert(res.elements[15] == 0x5a && res.elements[16] == 0);
}

static void test_assoc_layouts(void) {
    struct diting_wifi_assoc_params p;
    uint32_t al = 0, au = 0;
    int modern = -1;

    // 1. iOS 27 656-byte layout (WPA2-Personal 0x10)
    struct diting_wifi_assoc_req27 r27;
    memset(&r27, 0, sizeof(r27));
    r27.version = 1; r27.auth_lower = 1; r27.auth_upper = 0x10;
    r27.network.ssid_length = 4; memcpy(r27.network.ssid, "Home", 4);
    r27.network.key_length = 8; r27.network.key_cipher = 10; memcpy(r27.network.key, "12345678", 8);
    assert(diting_wifi_decode_assoc(&r27, sizeof(r27), &p, &al, &au, &modern) == 0);
    assert(al == 1 && au == 0x10 && modern == 1 && p.ssid_length == 4 && p.key_length == 8 && p.key_cipher == 10);

    // 2. iPadOS 18 900-byte layout (WPA2-Personal 0x8)
    struct diting_wifi_assoc_req24 r24;
    memset(&r24, 0, sizeof(r24));
    r24.version = 1; r24.auth_lower = 1; r24.auth_upper = 0x8;
    r24.network.ssid_length = 5; memcpy(r24.network.ssid, "Lab5G", 5);
    assert(diting_wifi_decode_assoc(&r24, sizeof(r24), &p, &al, &au, &modern) == 0);
    assert(al == 1 && au == 0x8 && modern == 1 && p.ssid_length == 5 && p.key_length == 0);

    // 3. Legacy 592-byte layout
    struct diting_wifi_assoc_req_legacy r16;
    memset(&r16, 0, sizeof(r16));
    r16.version = 1; r16.auth_lower = 1; r16.auth_upper = 0x8;
    r16.network.ssid_length = 3; memcpy(r16.network.ssid, "Old", 3);
    assert(diting_wifi_decode_assoc(&r16, sizeof(r16), &p, &al, &au, &modern) == 0);
    assert(al == 1 && au == 0x8 && modern == 0 && p.ssid_length == 3);

    // 4. Reject unknown size or bad version
    assert(diting_wifi_decode_assoc(&r27, 400, &p, &al, &au, &modern) == -1);
    r27.version = 2;
    assert(diting_wifi_decode_assoc(&r27, sizeof(r27), &p, &al, &au, &modern) == -1);
}

static void test_channel_filter(void) {
    struct diting_wifi_channel c;
    // Standard 2.4 GHz (ch 1..14)
    assert(diting_wifi_fill_channel(&c, 2412) == 0 && c.channel == 1 && c.flags == (0x2u | 0x8u));
    assert(diting_wifi_fill_channel(&c, 2437) == 0 && c.channel == 6 && c.flags == (0x2u | 0x8u));
    assert(diting_wifi_fill_channel(&c, 2472) == 0 && c.channel == 13 && c.flags == (0x2u | 0x8u));
    assert(diting_wifi_fill_channel(&c, 2484) == 0 && c.channel == 14 && c.flags == (0x2u | 0x8u));
    // Standard 5 GHz (ch 36..177)
    assert(diting_wifi_fill_channel(&c, 5180) == 0 && c.channel == 36 && c.flags == (0x2u | 0x10u));
    assert(diting_wifi_fill_channel(&c, 5745) == 0 && c.channel == 149 && c.flags == (0x2u | 0x10u));
    assert(diting_wifi_fill_channel(&c, 5885) == 0 && c.channel == 177 && c.flags == (0x2u | 0x10u));
    // Reject 6 GHz and out-of-band / non-grid frequencies
    const uint32_t rejected[] = {0, 2407, 2413, 2477, 2490, 5175, 5182, 5890, 5955, 6115, 7115};
    for (uint32_t freq : rejected) {
        assert(!diting_wifi_is_2g_or_5g_freq(freq));
        assert(diting_wifi_fill_channel(&c, freq) == -1);
    }
}

static void test_scan_plan(void) {
    uint32_t delay_ms = 999u;
    // Cached scan (type 3) with held BSSes must still deliver an async kEvScanDone event.
    assert(diting_wifi_plan_scan(DITING_WIFI_SCAN_TYPE_CACHED, 0, 12, &delay_ms) ==
           DITING_WIFI_SCAN_ACTION_CACHED_EVENT);
    assert(delay_ms == DITING_WIFI_CACHED_SCAN_DELAY_MS);
    assert(diting_wifi_plan_scan(DITING_WIFI_SCAN_TYPE_CACHED, 1, 12, &delay_ms) ==
           DITING_WIFI_SCAN_ACTION_CACHED_EVENT);
    assert(delay_ms == DITING_WIFI_CACHED_SCAN_DELAY_MS);
    // Cached scan with empty cache while idle falls back to an on-air scan.
    assert(diting_wifi_plan_scan(DITING_WIFI_SCAN_TYPE_CACHED, 0, 0, &delay_ms) ==
           DITING_WIFI_SCAN_ACTION_AIR);
    assert(delay_ms == 0u);
    // Active scan while idle runs on-air.
    assert(diting_wifi_plan_scan(1u, 0, 12, &delay_ms) == DITING_WIFI_SCAN_ACTION_AIR);
    assert(delay_ms == 0u);
    // Active scan while joining/joined answers asynchronously from cache without disrupting link.
    assert(diting_wifi_plan_scan(1u, 1, 12, &delay_ms) == DITING_WIFI_SCAN_ACTION_CACHED_EVENT);
    assert(delay_ms == DITING_WIFI_JOINED_SCAN_DELAY_MS);
    assert(diting_wifi_plan_scan(1u, 1, 0, &delay_ms) == DITING_WIFI_SCAN_ACTION_CACHED_EVENT);
    assert(delay_ms == DITING_WIFI_AUTH_EMPTY_SCAN_DELAY_MS);
}

int main(void) {
    test_caps();
    test_scan_multiple();
    test_scan_plan();
    test_scan_result_timestamp();
    test_assoc_layouts();
    test_channel_filter();
    puts("WIFI_PROTOCOL27_HOST_TESTS_PASS");
    return 0;
}
"""


class WifiProtocol27Tests(unittest.TestCase):
    def test_protocol_wire_layouts_and_helpers(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "test_wifi_proto.cpp"
            exe = Path(tmp) / "test_wifi_proto"
            src.write_text(HARNESS)
            subprocess.run(
                [
                    "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "drivers/DitingWifi"),
                    str(src), "-o", str(exe),
                ],
                check=True,
            )
            res = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
            self.assertIn("WIFI_PROTOCOL27_HOST_TESTS_PASS", res.stdout)

    def test_ath11k_nw_sta_bss_info_changed_fallback(self):
        import importlib.util

        spec = importlib.util.spec_from_file_location(
            "build_diting_ath11k", ROOT / "scripts/build-diting-ath11k.py"
        )
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)

        sample = (
            "static struct ieee80211_hw *g_hw; static const struct ieee80211_ops *g_ops; static struct ieee80211_vif *g_vif;\n"
            "static void make_nonce(u8 *out) { (void)out; }\n"
            "static void derive_ptk(void) {}\n"
            "static void f(void) {\n"
            "  g_ops->link_info_changed(g_hw, g_vif, &g_vif->bss_conf, 1);\n"
            "  g_ops->vif_cfg_changed(g_hw, g_vif, 2);\n"
            "}\n"
        )
        out = mod.adapt_nw_sta(sample)
        self.assertIn("DitingWifiRandom(out, 32);", out)
        self.assertIn("else if (g_ops->bss_info_changed) g_ops->bss_info_changed(hw, vif, info, changed);", out)
        self.assertIn("else if (g_ops->bss_info_changed) g_ops->bss_info_changed(hw, vif, &vif->bss_conf, changed);", out)
        self.assertIn("drv_link_info_changed(g_hw, g_vif, &g_vif->bss_conf, 1);", out)
        self.assertIn("drv_vif_cfg_changed(g_hw, g_vif, 2);", out)
        self.assertEqual(out.count("g_ops->link_info_changed("), 1)
        self.assertEqual(out.count("g_ops->vif_cfg_changed("), 1)

        real_sta = ROOT / "refer/ipados18/Ath12kWifi/src/nw_sta.c"
        if real_sta.is_file():
            real_out = mod.adapt_nw_sta(real_sta.read_text())
            self.assertEqual(real_out.count("g_ops->link_info_changed("), 1)
            self.assertEqual(real_out.count("g_ops->vif_cfg_changed("), 1)
            self.assertGreaterEqual(real_out.count("drv_link_info_changed("), 2)
            self.assertGreaterEqual(real_out.count("drv_vif_cfg_changed("), 2)


if __name__ == "__main__":
    unittest.main()
