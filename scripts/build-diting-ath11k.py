#!/usr/bin/env python3
"""Build Linux 7.0.14 ath11k PCI/MHI using UniXNU's shared Wi-Fi ABI.

Sources are local public Linux and UniXNU Ath12kWifi checkouts.
The driver is not replaced by success stubs. Missing core symbols fail the
link recipe; power/DMA/MSI remain the consuming Diting board's responsibility.
"""
import argparse
import sys
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
import diting_paths


def adapt_nw_mac80211(mac):
    # Match Linux cfg80211 wiphy_new_nm() MIB defaults: leaving rts_threshold=0
    # makes ath11k_mac_op_add_interface() send WMI_VDEV_PARAM_RTS_THRESHOLD=0,
    # forcing RTS/CTS before every frame and collapsing TX rates.
    anchor = '  mutex_init(&w->mtx); INIT_LIST_HEAD(&w->wdev_list);'
    if mac.count(anchor) != 1:
        raise RuntimeError('nw_mac80211.c wiphy init anchor changed')
    mac = mac.replace(
        anchor,
        anchor + '\n  w->retry_short = 7; w->retry_long = 4;\n  w->frag_threshold = (u32)-1; w->rts_threshold = (u32)-1;',
        1,
    )
    mac += r"""
/* Linux ath11k still uses these mac80211 APIs; the shared station has one
 * hardware and an explicit active-vif list. Honour the supplied vif address. */
struct ieee80211_sta *ieee80211_find_sta_by_ifaddr(struct ieee80211_hw *hw, const u8 *addr, const u8 *local) {
    if (hw != g_hw || !g_sta || !ether_addr_equal(g_sta->addr, addr)) return 0;
    for (int i = 0; i < g_nvif; ++i) if (!local || ether_addr_equal(g_vif[i]->addr, local)) return g_sta;
    return 0;
}
void ieee80211_queue_work(struct ieee80211_hw *hw, struct work_struct *work) { if (hw == g_hw) lx_queue_work(work); }
"""
    return mac


def adapt_nw_sta(sta):
    # WPA station nonces use Darwin's entropy source, not a timer/counter.
    begin = sta.index('static void make_nonce(u8 *out) {')
    end = sta.index('static void derive_ptk(void)', begin)
    sta = sta[:begin] + 'extern void DitingWifiRandom(void *, size_t);\nstatic void make_nonce(u8 *out) { DitingWifiRandom(out, 32); }\n' + sta[end:]
    # Linux ath11k is a non-MLO driver: ath11k_ops provides .bss_info_changed
    # and leaves .link_info_changed/.vif_cfg_changed NULL. Match mac80211's
    # drv_link_info_changed()/drv_vif_cfg_changed() fallback instead of
    # branching through a NULL PAC-signed function pointer during connect.
    ops_decl = 'static struct ieee80211_hw *g_hw; static const struct ieee80211_ops *g_ops; static struct ieee80211_vif *g_vif;'
    if sta.count(ops_decl) != 1:
        raise RuntimeError('nw_sta.c g_ops declaration changed')
    if 'g_ops->link_info_changed(' not in sta or 'g_ops->vif_cfg_changed(' not in sta:
        raise RuntimeError('nw_sta.c link_info_changed/vif_cfg_changed calls missing')
    sta = sta.replace('g_ops->link_info_changed(', 'drv_link_info_changed(')
    sta = sta.replace('g_ops->vif_cfg_changed(', 'drv_vif_cfg_changed(')
    helpers = ops_decl + """
static void drv_link_info_changed(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
                                  struct ieee80211_bss_conf *info, u64 changed) {
  if (g_ops->link_info_changed) g_ops->link_info_changed(hw, vif, info, changed);
  else if (g_ops->bss_info_changed) g_ops->bss_info_changed(hw, vif, info, changed);
}
static void drv_vif_cfg_changed(struct ieee80211_hw *hw, struct ieee80211_vif *vif, u64 changed) {
  if (g_ops->vif_cfg_changed) g_ops->vif_cfg_changed(hw, vif, changed);
  else if (g_ops->bss_info_changed) g_ops->bss_info_changed(hw, vif, &vif->bss_conf, changed);
}
static void diting_conf_wmm(const u8 *ies, unsigned ie_len) {
  if (!g_ops->conf_tx) return;
  /* IEEE 802.11 / WMM defaults indexed by mac80211 AC (VO=0, VI=1, BE=2, BK=3). */
  struct ieee80211_tx_queue_params q[4];
  memset(q, 0, sizeof q);
  q[IEEE80211_AC_VO].aifs = 2; q[IEEE80211_AC_VO].cw_min = 3;  q[IEEE80211_AC_VO].cw_max = 7;    q[IEEE80211_AC_VO].txop = 47;
  q[IEEE80211_AC_VI].aifs = 2; q[IEEE80211_AC_VI].cw_min = 7;  q[IEEE80211_AC_VI].cw_max = 15;   q[IEEE80211_AC_VI].txop = 94;
  q[IEEE80211_AC_BE].aifs = 3; q[IEEE80211_AC_BE].cw_min = 15; q[IEEE80211_AC_BE].cw_max = 1023; q[IEEE80211_AC_BE].txop = 0;
  q[IEEE80211_AC_BK].aifs = 7; q[IEEE80211_AC_BK].cw_min = 15; q[IEEE80211_AC_BK].cw_max = 1023; q[IEEE80211_AC_BK].txop = 0;
  /* Parse AP WMM Parameter Element (OUI 00:50:f2, type 2, subtype 1, len >= 24). */
  const struct element *we = cfg80211_find_vendor_elem(0x0050f2, 2, ies, ie_len);
  if (we && we->datalen >= 24 && we->data[4] == 1) {
    static const u16 aci_to_ac[4] = { IEEE80211_AC_BE, IEEE80211_AC_BK, IEEE80211_AC_VI, IEEE80211_AC_VO };
    for (int i = 0; i < 4; ++i) {
      const u8 *r = we->data + 8 + i * 4;
      u16 ac = aci_to_ac[(r[0] >> 5) & 3];
      u8 aifs = r[0] & 0x0f;
      u8 ecw_min = r[1] & 0x0f;
      u8 ecw_max = (r[1] >> 4) & 0x0f;
      if (aifs < 2) aifs = 2;
      if (ecw_min > 15) ecw_min = 15;
      if (ecw_max > 15) ecw_max = 15;
      q[ac].aifs = aifs;
      q[ac].acm = !!(r[0] & 0x10);
      q[ac].cw_min = (u16)((1u << ecw_min) - 1u);
      q[ac].cw_max = (u16)((1u << ecw_max) - 1u);
      q[ac].txop = (u16)(r[2] | ((u16)r[3] << 8));
    }
  }
  for (u16 ac = 0; ac < 4; ++ac)
    g_ops->conf_tx(g_hw, g_vif, 0, ac, &q[ac]);
}"""
    sta = sta.replace(ops_decl, helpers, 1)
    # Advertise WMM Info Element in Association Request (required by 802.11n/ac
    # APs for HT/VHT MCS & QoS data frames when sta->wme = true).
    assoc_tail = '  if (g_vht_ie_len) skb_put_data(skb, g_vht_ie, g_vht_ie_len);\n  tx_frame(skb); }'
    assoc_wmm = (
        '  if (g_vht_ie_len) skb_put_data(skb, g_vht_ie, g_vht_ie_len);\n'
        '  if (g_ht_on || cfg80211_find_vendor_elem(0x0050f2, 2, C.ap.ie, C.ap.ie_len)) {\n'
        '    static const u8 wmm_ie[] = { WLAN_EID_VENDOR_SPECIFIC, 7, 0x00, 0x50, 0xf2, 0x02, 0x00, 0x01, 0x00 };\n'
        '    skb_put_data(skb, wmm_ie, sizeof wmm_ie);\n'
        '  }\n'
        '  tx_frame(skb); }'
    )
    if assoc_tail in sta:
        sta = sta.replace(assoc_tail, assoc_wmm, 1)
    # Program WMM EDCA parameters and explicitly disable firmware STA power save
    # right after association so QCA6490 never uses zeroed WMM params or PS sleep.
    assoc_cfg = '    drv_vif_cfg_changed(g_hw, g_vif, BSS_CHANGED_ASSOC | BSS_CHANGED_IDLE | BSS_CHANGED_SSID);'
    assoc_cfg_new = (
        '    diting_conf_wmm(C.ap.ie, C.ap.ie_len);\n'
        '    g_vif->cfg.ps = false;\n'
        '    drv_vif_cfg_changed(g_hw, g_vif, BSS_CHANGED_ASSOC | BSS_CHANGED_IDLE | BSS_CHANGED_SSID | BSS_CHANGED_PS);'
    )
    if assoc_cfg in sta:
        sta = sta.replace(assoc_cfg, assoc_cfg_new, 1)
    # Match IEEE 802.11n/ac RX A-MPDU buffer limit (64 subframes).
    sta = sta.replace('sta->max_rx_aggregation_subframes = 8;', 'sta->max_rx_aggregation_subframes = 64;', 1)
    # Propagate AP ERP protection, preamble, short-slot, DTIM period and listen interval.
    erp_old = '    g_vif->bss_conf.assoc_capability = C.ap.caps; g_vif->bss_conf.use_short_slot = true;'
    erp_new = (
        '    g_hw->conf.listen_interval = 10;\n'
        '    g_vif->bss_conf.assoc_capability = C.ap.caps;\n'
        '    {\n'
        '      const u8 *erp = find_ie(C.ap.ie, C.ap.ie_len, WLAN_EID_ERP_INFO);\n'
        '      g_vif->bss_conf.use_cts_prot = !!(erp && erp[1] >= 1 && (erp[2] & WLAN_ERP_USE_PROTECTION));\n'
        '      g_vif->bss_conf.use_short_preamble = !(erp && erp[1] >= 1 && (erp[2] & WLAN_ERP_BARKER_PREAMBLE)) && !!(C.ap.caps & WLAN_CAPABILITY_SHORT_PREAMBLE);\n'
        '      g_vif->bss_conf.use_short_slot = !!(C.ap.caps & WLAN_CAPABILITY_SHORT_SLOT_TIME);\n'
        '    }'
    )
    if erp_old in sta:
        sta = sta.replace(erp_old, erp_new, 1)
    link_old = 'drv_link_info_changed(g_hw, g_vif, &g_vif->bss_conf, BSS_CHANGED_BSSID | BSS_CHANGED_BASIC_RATES | BSS_CHANGED_BEACON_INT | BSS_CHANGED_ERP_SLOT);'
    link_new = 'drv_link_info_changed(g_hw, g_vif, &g_vif->bss_conf, BSS_CHANGED_BSSID | BSS_CHANGED_BASIC_RATES | BSS_CHANGED_BEACON_INT | BSS_CHANGED_BEACON_INFO | BSS_CHANGED_ERP_SLOT | BSS_CHANGED_ERP_CTS_PROT | BSS_CHANGED_ERP_PREAMBLE);'
    if link_old in sta:
        sta = sta.replace(link_old, link_new, 1)
    # Handle 802.11 Block Ack Action frames (ADDBA_REQ / DELBA) so ath11k REO
    # opens a real RX reorder window (ba_win_sz >= 64) instead of staying at 1.
    mgmt_sig = 'static void on_mgmt(struct sk_buff *skb) {'
    addba_helper = """static void diting_handle_action(struct sk_buff *skb) {
  if (!C.sta || C.state < NW_STA_KEYS || skb->len < 25) return;
  const struct ieee80211_mgmt *m = (const void *)skb->data;
  if (m->u.action.category != WLAN_CATEGORY_BACK) return;
  const u8 code = m->u.action.u.addba_req.action_code;
  if (code == WLAN_ACTION_ADDBA_REQ && skb->len >= 24 + 9) {
    u8 token = m->u.action.u.addba_req.dialog_token;
    u16 capab = le16_to_cpu(m->u.action.u.addba_req.capab);
    u16 timeout = le16_to_cpu(m->u.action.u.addba_req.timeout);
    u16 ssn = le16_to_cpu(m->u.action.u.addba_req.start_seq_num) >> 4;
    u16 amsdu = capab & 1u;
    u16 policy = (capab >> 1) & 1u;
    u16 tid = (capab >> 2) & 0xfu;
    u16 buf_size = (capab >> 6) & 0x3ffu;
    if (!buf_size || buf_size > 64) buf_size = 64;
    u16 status = WLAN_STATUS_REQUEST_DECLINED;
    if (policy == 1 && tid < IEEE80211_NUM_TIDS && g_ops->ampdu_action) {
      struct ieee80211_ampdu_params ap;
      memset(&ap, 0, sizeof ap);
      ap.action = IEEE80211_AMPDU_RX_START;
      ap.sta = C.sta;
      ap.tid = tid;
      ap.ssn = ssn;
      ap.buf_size = buf_size;
      ap.amsdu = !!amsdu;
      ap.timeout = timeout;
      int rc = g_ops->ampdu_action(g_hw, g_vif, &ap);
      if (!rc) status = WLAN_STATUS_SUCCESS;
      lx_log(6, "wifi: ADDBA RX tid=%u buf=%u ssn=%u amsdu=%u -> %d", tid, buf_size, ssn, amsdu, rc);
    }
    struct sk_buff *resp = new_frame(16);
    if (!resp) return;
    mgmt_header(resp, IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_ACTION);
    u8 *p = skb_put_zero(resp, 9);
    u16 rcap = (u16)(amsdu | (1u << 1) | ((tid & 0xfu) << 2) | ((buf_size & 0x3ffu) << 6));
    p[0] = WLAN_CATEGORY_BACK;
    p[1] = WLAN_ACTION_ADDBA_RESP;
    p[2] = token;
    p[3] = (u8)(status & 0xff); p[4] = (u8)(status >> 8);
    p[5] = (u8)(rcap & 0xff); p[6] = (u8)(rcap >> 8);
    p[7] = 0; p[8] = 0;
    tx_frame(resp);
  } else if (code == WLAN_ACTION_DELBA && skb->len >= 24 + 6) {
    u16 params = le16_to_cpu(m->u.action.u.delba.params);
    u16 initiator = (params >> 11) & 1u;
    u16 tid = (params >> 12) & 0xfu;
    if (initiator && tid < IEEE80211_NUM_TIDS && g_ops->ampdu_action) {
      struct ieee80211_ampdu_params ap;
      memset(&ap, 0, sizeof ap);
      ap.action = IEEE80211_AMPDU_RX_STOP;
      ap.sta = C.sta;
      ap.tid = tid;
      g_ops->ampdu_action(g_hw, g_vif, &ap);
      lx_log(6, "wifi: DELBA RX tid=%u", tid);
    }
  }
}
"""
    if mgmt_sig in sta:
        sta = sta.replace(mgmt_sig, addba_helper + mgmt_sig, 1)
    deauth_check = '  if ((ieee80211_is_deauth(m->frame_control) || ieee80211_is_disassoc(m->frame_control)) && C.state >= NW_STA_AUTH) {'
    if deauth_check in sta:
        sta = sta.replace(
            deauth_check,
            '  if (ieee80211_is_action(m->frame_control)) { diting_handle_action(skb); return; }\n' + deauth_check,
            1,
        )
    # Drop self-echoed broadcast/multicast frames (SA == own station MAC) per
    # IEEE 802.11 / IFF_SIMPLEX so XNU never mistakes our own ARP/DHCP for a peer.
    rx_8023_old = '      if (skb->len >= 14 && !(skb->data[12] == 0x88 && skb->data[13] == 0x8e) && C.state >= NW_STA_KEYS && g_ev.rx) g_ev.rx(skb->data, skb->len);'
    rx_8023_new = '      if (skb->len >= 14 && memcmp(skb->data + 6, g_vif->addr, 6) && !(skb->data[12] == 0x88 && skb->data[13] == 0x8e) && C.state >= NW_STA_KEYS && g_ev.rx) g_ev.rx(skb->data, skb->len);'
    if rx_8023_old in sta:
        sta = sta.replace(rx_8023_old, rx_8023_new, 1)
    to_eth_old = '  if (eth[12] == 0x88 && eth[13] == 0x8e) return;'
    to_eth_new = '  if (!memcmp(eth + 6, g_vif->addr, 6)) return;\n  if (eth[12] == 0x88 && eth[13] == 0x8e) return;'
    if to_eth_old in sta:
        sta = sta.replace(to_eth_old, to_eth_new, 1)
    # Prefer 5 GHz over congested 2.4 GHz when signal is good (>= -75 dBm) and
    # no explicit BSSID was requested.
    bss_pick_old = '    if (g_req.has_bssid && memcmp(b->bssid, g_req.bssid, 6)) continue; if (!best || b->signal > best->signal) best = b; }'
    bss_pick_new = (
        '    if (g_req.has_bssid && memcmp(b->bssid, g_req.bssid, 6)) continue;\n'
        '    int score = (int)b->signal + ((b->freq >= 4900 && b->signal >= -75) ? 12 : 0);\n'
        '    int best_score = best ? ((int)best->signal + ((best->freq >= 4900 && best->signal >= -75) ? 12 : 0)) : -999;\n'
        '    if (!best || score > best_score) best = b; }'
    )
    if bss_pick_old in sta:
        sta = sta.replace(bss_pick_old, bss_pick_new, 1)
    # Keep Assoc Req VHT capabilities aligned with peer_caps(): never advertise
    # SU/MU beamforming or 160 MHz to the AP when firmware peer_caps strips them.
    vht_ie_old = '    cap.vht_cap_info = cpu_to_le32(sb->vht_cap.cap);'
    vht_ie_new = (
        '    u32 vcap = sb->vht_cap.cap;\n'
        '    if (g_width != NL80211_CHAN_WIDTH_160) vcap &= ~IEEE80211_VHT_CAP_SHORT_GI_160;\n'
        '    if (g_width != NL80211_CHAN_WIDTH_160 && g_width != NL80211_CHAN_WIDTH_80P80)\n'
        '      vcap &= ~(IEEE80211_VHT_CAP_SUPP_CHAN_WIDTH_MASK | IEEE80211_VHT_CAP_EXT_NSS_BW_MASK);\n'
        '    vcap &= ~(IEEE80211_VHT_CAP_SU_BEAMFORMER_CAPABLE | IEEE80211_VHT_CAP_SU_BEAMFORMEE_CAPABLE |\n'
        '              IEEE80211_VHT_CAP_MU_BEAMFORMER_CAPABLE | IEEE80211_VHT_CAP_MU_BEAMFORMEE_CAPABLE |\n'
        '              IEEE80211_VHT_CAP_BEAMFORMEE_STS_MASK | IEEE80211_VHT_CAP_SOUNDING_DIMENSIONS_MASK);\n'
        '    cap.vht_cap_info = cpu_to_le32(vcap);'
    )
    if vht_ie_old in sta:
        sta = sta.replace(vht_ie_old, vht_ie_new, 1)
    # Match chanctx RX chain count to the radio's actual HT spatial streams (2x2 on QCA6490).
    chains_old = '  ctx->min_def = ctx->def; ctx->ap = ctx->def; ctx->rx_chains_static = 1; ctx->rx_chains_dynamic = 1;'
    chains_new = (
        '  u8 chains = 1;\n'
        '  struct ieee80211_supported_band *csb = g_hw->wiphy->bands[ch->band];\n'
        '  if (csb) { for (int ci = 0; ci < 4; ci++) if (csb->ht_cap.mcs.rx_mask[ci]) chains = (u8)(ci + 1); }\n'
        '  ctx->min_def = ctx->def; ctx->ap = ctx->def; ctx->rx_chains_static = chains; ctx->rx_chains_dynamic = chains;'
    )
    if chains_old in sta:
        sta = sta.replace(chains_old, chains_new, 1)
    return sta


def adapt_hw_c(hw: str) -> str:
    """Route all WCN6855 REO hash buckets to SW1 (reo_dst[0]) for strict FIFO RX.

    Our single worker polls REO destination rings sequentially (SW1..SW4).
    With the default 4-ring hash spread, consecutive MPDUs of the same TCP
    stream land on different REO rings and arrive ~65% out-of-order at XNU TCP,
    triggering SACK/DUP-ACK storms and collapsing sender cwnd.
    """
    needle = (
        "static void ath11k_hw_wcn6855_reo_setup(struct ath11k_base *ab)\n"
        "{\n"
        "\tu32 reo_base = HAL_SEQ_WCSS_UMAC_REO_REG;\n"
        "\tu32 val;\n"
        "\t/* Each hash entry uses four bits to map to a particular ring. */\n"
        "\tu32 ring_hash_map = HAL_HASH_ROUTING_RING_SW1 << 0 |\n"
        "\t\tHAL_HASH_ROUTING_RING_SW2 << 4 |\n"
        "\t\tHAL_HASH_ROUTING_RING_SW3 << 8 |\n"
        "\t\tHAL_HASH_ROUTING_RING_SW4 << 12 |\n"
        "\t\tHAL_HASH_ROUTING_RING_SW1 << 16 |\n"
        "\t\tHAL_HASH_ROUTING_RING_SW2 << 20 |\n"
        "\t\tHAL_HASH_ROUTING_RING_SW3 << 24 |\n"
        "\t\tHAL_HASH_ROUTING_RING_SW4 << 28;"
    )
    replacement = (
        "static void ath11k_hw_wcn6855_reo_setup(struct ath11k_base *ab)\n"
        "{\n"
        "\tu32 reo_base = HAL_SEQ_WCSS_UMAC_REO_REG;\n"
        "\tu32 val;\n"
        "\t/* Single-worker strict-FIFO RX: route all 16 hash buckets to SW1. */\n"
        "\tu32 ring_hash_map = HAL_HASH_ROUTING_RING_SW1 << 0 |\n"
        "\t\tHAL_HASH_ROUTING_RING_SW1 << 4 |\n"
        "\t\tHAL_HASH_ROUTING_RING_SW1 << 8 |\n"
        "\t\tHAL_HASH_ROUTING_RING_SW1 << 12 |\n"
        "\t\tHAL_HASH_ROUTING_RING_SW1 << 16 |\n"
        "\t\tHAL_HASH_ROUTING_RING_SW1 << 20 |\n"
        "\t\tHAL_HASH_ROUTING_RING_SW1 << 24 |\n"
        "\t\tHAL_HASH_ROUTING_RING_SW1 << 28;"
    )
    if hw.count(needle) != 1:
        raise RuntimeError('ath11k WCN6855 REO setup ABI differs')
    return hw.replace(needle, replacement, 1)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--linux', type=Path, required=True)
    p.add_argument('--shared', type=Path, default=diting_paths.sheng_root()/'drivers/Ath12kWifi')
    p.add_argument('--output-dir', type=Path, required=True)
    p.add_argument('--jobs', type=int, default=4)
    p.add_argument('--target', default='arm64-apple-ios18.0')
    p.add_argument('--clang', default='')
    p.add_argument('--ld', default='')
    p.add_argument('--libtool', default='')
    a = p.parse_args(); linux = a.linux.resolve(); shared = a.shared.resolve(); out = a.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if not (linux / 'drivers/net/wireless/ath/ath11k/pci.c').is_file(): raise RuntimeError('Linux ath11k PCI sources required')
    for file in ('include/nw_abi.h', 'shim/src/lx_rt.c', 'src/nw_sta.c'):
        if not (shared / file).is_file(): raise RuntimeError('UniXNU Ath12kWifi shared ABI required: ' + file)
    source = linux / 'drivers/net/wireless/ath/ath11k'
    gen = out / 'ath11k'; gen.mkdir(exist_ok=True)
    for f in source.glob('*'):
        if f.suffix not in ('.c', '.h') or f.name == 'trace.h': continue
        target = gen / f.name
        if target.exists() or target.is_symlink(): target.unlink()
        target.symlink_to(f)
    traces = sorted({v for f in source.glob('*.[ch]') for v in re.findall(r'\btrace_ath11k_[a-z0-9_]+', f.read_text())})
    (gen / 'trace.h').write_text('\n'.join('#define ' + n + '(...) ' + ('0' if n.endswith('_enabled') else '((void)0)') for n in traces) + '\n')
    spectral = out / 'spectral_common.h'
    if spectral.exists() or spectral.is_symlink(): spectral.unlink()
    spectral.symlink_to(source.parent / 'spectral_common.h')
    # A kernel variable-like jiffies macro cannot also name this statistics
    # member. Rename only the generated member/access, not time expressions.
    for name in ('core.h', 'dp_rx.c'):
        content = (source / name).read_text().replace('unsigned long jiffies;', 'unsigned long bp_jiffies;').replace('bp_stats->jiffies', 'bp_stats->bp_jiffies')
        (gen / name).unlink(); (gen / name).write_text(content)
    pc = (source / 'pci.c').read_text()
    for old, new in [('static int ath11k_pci_init(void)', 'int diting_ath11k_pci_init(void)'),
                     ('static void ath11k_pci_exit(void)', 'void diting_ath11k_pci_exit(void)')]:
        if pc.count(old) != 1: raise RuntimeError('Linux 7.0.14 PCI entry differs: ' + old)
        pc = pc.replace(old, new)
    (gen / 'pci.c').unlink(); (gen / 'pci.c').write_text(pc)
    # Xiaomi diting (HARDWARE_PROJECT_L12) CNSS2 BDF selection:
    # For board_id == 0xff, use L12 board data (bd_l12gf.elf when chip_id & 0x10,
    # else bd_l12.elf), falling back to generic bdwlang.elf/bdwlan.elf if absent.
    core = (gen / 'core.c').read_text()
    needle = '\tbd->fw = ath11k_core_firmware_request(ab, name);'
    replacement = r"""    char diting_bdf[48];
    const char *diting_fallback = NULL;
    if (!strcmp(name, ATH11K_DEFAULT_BOARD_FILE)) {
        u32 id = ab->qmi.target.board_id;
        int gf = !!(ab->qmi.target.chip_id & 0x10);
        const char *prefix = gf ? "bdwlang" : "bdwlan";
        if (id == 0xff) {
            snprintf(diting_bdf, sizeof diting_bdf, "%s", gf ? "bd_l12gf.elf" : "bd_l12.elf");
            diting_fallback = gf ? "bdwlang.elf" : "bdwlan.elf";
        } else if (id < 0xff) {
            snprintf(diting_bdf, sizeof diting_bdf, "%s.e%02x", prefix, id);
        } else {
            snprintf(diting_bdf, sizeof diting_bdf, "bdwlan%02x.e%02x", (id >> 8) & 255, id & 255);
        }
        name = diting_bdf;
        ath11k_info(ab, "Diting: actual QMI board firmware %s (chip_id=0x%x board_id=0x%x)\n",
                    name, ab->qmi.target.chip_id, id);
    }
""" + needle + r"""
    if (IS_ERR(bd->fw) && diting_fallback) {
        ath11k_warn(ab, "Diting: %s unavailable, falling back to %s\n", name, diting_fallback);
        bd->fw = ath11k_core_firmware_request(ab, diting_fallback);
    }"""
    if core.count(needle) != 1: raise RuntimeError('ath11k BDF request ABI differs')
    (gen / 'core.c').unlink(); (gen / 'core.c').write_text(core.replace(needle, replacement))
    hw_c = adapt_hw_c((source / 'hw.c').read_text())
    (gen / 'hw.c').unlink(); (gen / 'hw.c').write_text(hw_c)
    # Decode the vendor BDF PA flag and configure S3E only after the final
    # real response. The public Linux checkout is never changed.
    from diting_wifi_bdf import adapt_bdf
    qmi_source,qmi_header=adapt_bdf((source/'qmi.c').read_text(),(source/'qmi.h').read_text())
    for name,content in (('qmi.c',qmi_source),('qmi.h',qmi_header)):
        (gen/name).unlink();(gen/name).write_text(content)
    mac = adapt_nw_mac80211((shared / 'src/nw_mac80211.c').read_text())
    (out / 'nw_mac80211_diting.c').write_text(mac)

    pinned = out / 'pinned'; pinned.mkdir(exist_ok=True)
    inputs = {'linux/mhi.h': 'linux/mhi.h', 'linux/ieee80211.h': 'linux/ieee80211.h',
        'linux/soc/qcom/qmi.h': 'linux/soc/qcom/qmi.h', 'net/cfg80211.h': 'net/cfg80211.h',
        'net/regulatory.h': 'net/regulatory.h', 'net/codel.h': 'net/codel.h',
        'net/ieee80211_radiotap.h': 'net/ieee80211_radiotap.h',
        'linux/nl80211.h': 'uapi/linux/nl80211.h', 'linux/rfkill.h': 'uapi/linux/rfkill.h',
        'uapi/linux/rfkill.h': 'uapi/linux/rfkill.h', 'net/mac80211.real.h': 'net/mac80211.h'}
    inputs.update({str(f.relative_to(linux / 'include')): str(f.relative_to(linux / 'include'))
                   for f in (linux / 'include/linux').glob('ieee80211-*.h')})
    for target, name in inputs.items():
        src = linux / 'include' / name
        if not src.is_file(): raise RuntimeError('Missing Linux public header: ' + str(src))
        dest = pinned / target; dest.parent.mkdir(parents=True, exist_ok=True)
        if dest.exists() or dest.is_symlink(): dest.unlink()
        dest.symlink_to(src)
    mhi = out / 'mhi'; mhi.mkdir(exist_ok=True)
    for name in ('init.c', 'main.c', 'pm.c', 'boot.c', 'internal.h'):
        dest = mhi / name
        if dest.exists() or dest.is_symlink(): dest.unlink()
        dest.symlink_to(linux / 'drivers/bus/mhi/host' / name)
    (mhi / 'trace.h').write_text((shared / 'shim/mhi/trace.h').read_text())
    common = out / 'common.h'
    if common.exists() or common.is_symlink(): common.unlink()
    common.symlink_to(linux / 'drivers/bus/mhi/common.h')
    env = dict(os.environ, DEVELOPER_DIR=str(diting_paths.developer_dir()))
    clang = a.clang or subprocess.check_output(['xcrun', '-f', 'clang'], env=env, text=True).strip()
    resource = subprocess.check_output([clang, '-print-resource-dir'], text=True).strip() + '/include'
    overlay = ROOT / 'drivers/DitingWifi/ath11k/compat'
    arch = 'arm64e' if a.target.startswith('arm64e') else 'arm64'
    pac_flags = ['-fptrauth-function-pointer-type-discrimination'] if arch == 'arm64e' else []
    flags = ['-target', a.target, *pac_flags, '-mkernel', '-O2', '-g0', '-ffreestanding',
        '-fno-builtin', '-fno-common', '-fno-strict-aliasing', '-fwrapv', '-fno-stack-protector',
        '-fno-delete-null-pointer-checks', '-mgeneral-regs-only', '-std=gnu11',
        '-Wno-incompatible-pointer-types', '-Wno-error', '-Wno-unused-value',
        '-nostdinc', '-isystem', str(shared / 'shim/libc'), '-isystem', resource,
        '-I', str(overlay), '-include', 'DitingCompat.h', '-I', str(shared / 'shim/include'),
        '-I', str(shared / 'include'), '-I', str(pinned), '-I', str(shared / 'src'),
        '-I', str(ROOT/'drivers/DitingWifi'), '-I', str(gen), '-I', str(out), '-I', str(source.parent), '-DCONFIG_ATH11K_PCI=1',
        '-DLX_BOOT_SETTLE_MS=0']
    ath = 'core hal hal_tx hal_rx wmi mac reg htc qmi dp dp_tx dp_rx ce peer dbring hw pcic fw p2p mhi pci'.split()
    jobs = [(gen / (n + '.c'), out / ('ath11k_' + n + '.o'), []) for n in ath]
    jobs += [(mhi / (n + '.c'), out / ('mhi_' + n + '.o'), ['-I', str(mhi)]) for n in ('init', 'main', 'pm', 'boot')]
    sta = adapt_nw_sta((shared / 'src/nw_sta.c').read_text())
    (out / 'nw_sta_diting.c').write_text(sta)
    common_sources = ['src/nw_crypto.c', 'src/nw_mac80211.c', 'src/nw_net.c', 'src/nw_pci.c',
        'src/nw_qrtr.c', 'src/nw_qrtr_mhi.c', 'src/nw_sta.c', 'shim/src/lx_rt.c', 'shim/src/mhi_hook.c']
    jobs += [((out / 'nw_sta_diting.c') if n == 'src/nw_sta.c' else (out / 'nw_mac80211_diting.c') if n == 'src/nw_mac80211.c' else shared / n, out / ('shared_' + Path(n).stem + '.o'), []) for n in common_sources]
    jobs += [(linux / ('drivers/soc/qcom/' + n + '.c'), out / (n + '.o'), []) for n in ('qmi_interface', 'qmi_encdec')]
    jobs += [(ROOT / 'drivers/DitingWifi/ath11k/DitingAth11k.c', out / 'DitingAth11k.o', [])]
    commands = []
    def compile(job):
        src, obj, extra = job; cmd = [clang, *flags, *extra, '-c', str(src), '-o', str(obj)]
        commands.append(cmd)
        r = subprocess.run(cmd, capture_output=True, text=True)
        (out / (obj.stem + '.log')).write_text(r.stdout + r.stderr)
        return obj.name if r.returncode else None
    with ThreadPoolExecutor(a.jobs) as pool: failed = [r for r in pool.map(compile, jobs) if r]
    (out / 'compile-commands.json').write_text(json.dumps(commands, indent=2) + '\n')
    if failed: raise SystemExit('ath11k compile failures (retained .log): ' + ', '.join(failed))
    archive = out / 'libDitingAth11k.a'
    linked = out / 'ath11k-combined.o'
    ld_cmd = [a.ld] if a.ld else ['xcrun', 'ld']
    subprocess.run([*ld_cmd, '-r', '-arch', arch, *[str(o) for _, o, _ in jobs], '-o', str(linked)], env=env, check=True)
    nm = diting_paths.tool('llvm-nm', 'DITING_LLVM_NM', ('/opt/local/bin/llvm-nm-mp-18',))
    undefined = subprocess.check_output([nm, '-u', '-j', str(linked)], text=True).splitlines()
    (out / 'undefined-symbols.txt').write_text('\n'.join(undefined) + '\n')
    allowed = {'_diting_wifi_bdf_power_config','_DitingWifiRandom','_bzero','_memcmp','_memcpy','_memmove','_memset','_snprintf','_strchr','_strcmp','_strlen','_strncmp','_strncpy','_strtoul','_vsnprintf'}
    if set(undefined) - allowed: raise RuntimeError('unimplemented ath11k imports: ' + repr(set(undefined) - allowed))
    # Export only the documented board/station boundary. Linux shims have the
    # same C names as XNU helpers, so namespace every other defined symbol.
    exports = {'_DitingWifiRandom', '_nw_wifi_where'}
    defined = subprocess.check_output([nm, '--defined-only', '--extern-only', '-j', str(linked)], text=True).splitlines()
    renames = {n: '_diting11_' + n[1:] for n in defined if not n.startswith('_nw_wifi_') and n not in exports}
    mapping = out / 'symbol-map.txt'; mapping.write_text(''.join(k + ' ' + v + '\n' for k, v in renames.items()))
    namespaced = out / 'DitingAth11k-linked.o'
    subprocess.run([diting_paths.tool('llvm-objcopy', 'DITING_LLVM_OBJCOPY',
                                    ('/opt/local/bin/llvm-objcopy-mp-18',)),
                    '--redefine-syms=' + str(mapping), str(linked), str(namespaced)], check=True)
    archive.unlink(missing_ok=True)
    libtool_cmd = [a.libtool] if a.libtool else ['xcrun', 'libtool']
    subprocess.run([*libtool_cmd, '-static', '-o', str(archive), str(namespaced)], env=env, check=True)
    (out / 'build-report.json').write_text(json.dumps(dict(linux='v7.0.14', shared=str(shared),
        guest='22H374', archive=str(archive), real_pci_mhi=True, wifi_operational=False,
        missing_symbols_policy='no fabricated-success stubs', image_verification='skipped by request'), indent=2) + '\n')
    print(archive)


if __name__ == '__main__': main()
