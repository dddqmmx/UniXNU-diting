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
            # WMM Information Element in Association Request
            self.assertIn("0x00, 0x50, 0xf2, 0x02, 0x00, 0x01, 0x00", real_out)
            # WMM EDCA parameter programming via conf_tx()
            self.assertIn("diting_conf_wmm(C.ap.ie, C.ap.ie_len);", real_out)
            self.assertIn("g_ops->conf_tx(g_hw, g_vif, 0, ac, &q[ac]);", real_out)
            # Explicit STA power-save disable via BSS_CHANGED_PS
            self.assertIn("g_vif->cfg.ps = false;", real_out)
            self.assertIn("BSS_CHANGED_PS", real_out)
            # RX Block Ack (ADDBA_REQ / ADDBA_RESP / DELBA) & 64-subframe window
            self.assertIn("sta->max_rx_aggregation_subframes = 64;", real_out)
            self.assertIn("IEEE80211_AMPDU_RX_START", real_out)
            self.assertIn("IEEE80211_AMPDU_RX_STOP", real_out)
            self.assertIn("WLAN_ACTION_ADDBA_RESP", real_out)
            self.assertIn("diting_handle_action(skb);", real_out)
            # Self-echo multicast/broadcast RX filter & ERP/DTIM params & 5 GHz preference
            self.assertIn("memcmp(skb->data + 6, g_vif->addr, 6)", real_out)
            self.assertIn("g_hw->conf.listen_interval = 10;", real_out)
            self.assertIn("BSS_CHANGED_ERP_CTS_PROT", real_out)
            self.assertIn("b->freq >= 4900 && b->signal >= -75", real_out)
            # Assoc Req VHT cap parity & dynamic 2x2 RX chains
            self.assertIn("cap.vht_cap_info = cpu_to_le32(vcap);", real_out)
            self.assertIn("ctx->rx_chains_static = chains; ctx->rx_chains_dynamic = chains;", real_out)

        real_mac = ROOT / "refer/ipados18/Ath12kWifi/src/nw_mac80211.c"
        if real_mac.is_file():
            mac_out = mod.adapt_nw_mac80211(real_mac.read_text())
            self.assertIn("w->rts_threshold = (u32)-1;", mac_out)
            self.assertIn("w->frag_threshold = (u32)-1;", mac_out)
            self.assertIn("w->retry_short = 7;", mac_out)
            self.assertIn("w->retry_long = 4;", mac_out)

        real_hw = ROOT / "refer/ipados18/linux-7.0.14/drivers/net/wireless/ath/ath11k/hw.c"
        if real_hw.is_file():
            hw_out = mod.adapt_hw_c(real_hw.read_text())
            self.assertIn("Single-worker strict-FIFO RX: route all 16 hash buckets to SW1.", hw_out)
            self.assertIn("HAL_HASH_ROUTING_RING_SW1 << 28;", hw_out)

    def test_diting_bdf_regdb_msi_and_slab_invariants(self):
        ath_script = (ROOT / "scripts/build-diting-ath11k.py").read_text()
        self.assertIn("bd_l12gf.elf", ath_script)
        self.assertIn("bd_l12.elf", ath_script)
        self.assertIn("bdwlang.elf", ath_script)
        self.assertIn("diting_fallback", ath_script)

        plat = (ROOT / "drivers/DitingWifi/DitingWifiPlatform.cpp").read_text()
        self.assertIn("PcieMsi0Intid=32+141", plat)
        self.assertIn("armcpu_spi_attach(PcieMsi0Intid", plat)
        self.assertIn("regdb_xiaomi.bin", plat)
        self.assertIn("SlabSlotBytes=2560u, SlabSlots=12288u", plat)
        self.assertIn("waitWorkerUs(", plat)

        net = (ROOT / "drivers/DitingWifi/DitingWifiNet.cpp").read_text()
        self.assertIn("bpf_tap_in(gIf, DLT_EN10MB, m, const_cast<uint8_t *>(eth), 14);", net)
        self.assertIn("bpf_tap_out(gIf, DLT_EN10MB, m, nullptr, 0);", net)

    def test_dma_slab_allocator_behavior(self):
        slab_harness = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static constexpr uint64_t Iova = 0xa0000000ULL;
static constexpr size_t ArenaBytes = 64u * 1024u * 1024u;
static constexpr uint32_t SlabSlotBytes = 2560u, SlabSlots = 12288u;
static constexpr uint32_t SlabBytes = SlabSlotBytes * SlabSlots;
static constexpr uint32_t GeneralBytes = uint32_t(ArenaBytes) - SlabBytes;
static_assert((GeneralBytes & 4095u) == 0 && (SlabSlotBytes & 63u) == 0 && SlabBytes < ArenaBytes, "geom");

struct Block { uint32_t at, bytes; bool used; };
static Block blocks[16384];
static unsigned blockCount;
static uint16_t slabFree[SlabSlots];
static unsigned slabTop;
static uint8_t *arenaVa;

static void dmaInit(void) {
    blocks[0] = {0, GeneralBytes, false};
    blockCount = 1;
    for (unsigned i = 0; i < SlabSlots; ++i) slabFree[i] = uint16_t(SlabSlots - 1u - i);
    slabTop = SlabSlots;
}
static void *dmaAlloc(size_t n, uint64_t *bus) {
    if (!bus || !n || n > ArenaBytes) return nullptr;
    n = (n + 63u) & ~size_t(63u);
    if (n >= 4096u) n = (n + 4095u) & ~size_t(4095u);
    if (n <= SlabSlotBytes && slabTop) {
        const uint32_t slot = slabFree[--slabTop];
        const uint32_t off = GeneralBytes + slot * SlabSlotBytes;
        *bus = Iova + 4096u + off;
        void *p = arenaVa + off;
        memset(p, 0, n);
        return p;
    }
    uint32_t align = 64u;
    if (n >= 4096u) { align = 4096u; while (align < n) align <<= 1; }
    for (unsigned i = 0; i < blockCount; ++i) {
        auto b = blocks[i];
        if (b.used) continue;
        uint64_t addr = Iova + 4096u + b.at;
        uint32_t pad = uint32_t((-addr) & (align - 1u));
        if (n > b.bytes || pad > b.bytes - n) continue;
        unsigned extra = (pad ? 1u : 0u) + (b.bytes > pad + n ? 1u : 0u);
        if (blockCount + extra > 16384u) return nullptr;
        memmove(blocks + i + extra, blocks + i, (blockCount - i) * sizeof(Block));
        blockCount += extra;
        unsigned at = i;
        if (pad) blocks[at++] = {b.at, pad, false};
        blocks[at] = {b.at + pad, uint32_t(n), true};
        if (b.bytes > pad + n) blocks[at + 1] = {uint32_t(b.at + pad + n), uint32_t(b.bytes - pad - n), false};
        *bus = Iova + 4096u + b.at + pad;
        void *p = arenaVa + b.at + pad;
        memset(p, 0, n);
        return p;
    }
    return nullptr;
}
static void dmaFree(void *p) {
    if (!p) return;
    uintptr_t off = reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(arenaVa);
    if (off >= GeneralBytes && off < ArenaBytes) {
        const uint32_t rel = uint32_t(off - GeneralBytes);
        if ((rel % SlabSlotBytes) == 0 && slabTop < SlabSlots) slabFree[slabTop++] = uint16_t(rel / SlabSlotBytes);
        return;
    }
    for (unsigned i = 0; i < blockCount; ++i) {
        if (blocks[i].at == off && blocks[i].used) {
            blocks[i].used = false;
            if (i + 1 < blockCount && !blocks[i + 1].used) {
                blocks[i].bytes += blocks[i + 1].bytes;
                memmove(blocks + i + 1, blocks + i + 2, (blockCount - i - 2) * sizeof(Block));
                --blockCount;
            }
            if (i && !blocks[i - 1].used) {
                blocks[i - 1].bytes += blocks[i].bytes;
                memmove(blocks + i, blocks + i + 1, (blockCount - i - 1) * sizeof(Block));
                --blockCount;
            }
            return;
        }
    }
}
int main(void) {
    arenaVa = static_cast<uint8_t *>(calloc(1, ArenaBytes));
    assert(arenaVa != nullptr);
    dmaInit();
    // Allocate 8500 ath11k RX buffers (2304 B): blocks[] must remain untouched (count == 1)!
    void *ptrs[SlabSlots + 16];
    uint64_t bus = 0;
    for (unsigned i = 0; i < 8500; ++i) {
        ptrs[i] = dmaAlloc(2304, &bus);
        assert(ptrs[i] != nullptr);
        assert((bus & 511u) == 0);
    }
    assert(blockCount == 1);
    assert(slabTop == SlabSlots - 8500);
    // Exhaust the slab and verify seamless fallback to general pool.
    for (unsigned i = 8500; i < SlabSlots + 8; ++i) {
        ptrs[i] = dmaAlloc(2304, &bus);
        assert(ptrs[i] != nullptr);
        assert((bus & 63u) == 0);
    }
    assert(slabTop == 0);
    assert(blockCount > 1);
    // Large ring allocation goes straight to general pool with power-of-two alignment.
    void *ring = dmaAlloc(65536, &bus);
    assert(ring != nullptr && (bus & 65535u) == 0);
    // Free everything and verify full slab recovery and complete general coalescing.
    for (unsigned i = 0; i < SlabSlots + 8; ++i) dmaFree(ptrs[i]);
    dmaFree(ring);
    assert(slabTop == SlabSlots);
    assert(blockCount == 1 && !blocks[0].used && blocks[0].at == 0 && blocks[0].bytes == GeneralBytes);
    free(arenaVa);
    puts("WIFI_DMA_SLAB_HOST_TESTS_PASS");
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "test_dma_slab.cpp"
            exe = Path(tmp) / "test_dma_slab"
            src.write_text(slab_harness)
            subprocess.run(
                ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)],
                check=True,
            )
            res = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
            self.assertIn("WIFI_DMA_SLAB_HOST_TESTS_PASS", res.stdout)


if __name__ == "__main__":
    unittest.main()
