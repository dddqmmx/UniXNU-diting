/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Adapted from UniXNU/Ath12kWifi tests/sta_sim.c at
 * f6bf561f92cc42761bdd417fce1d53fe1db9477c. This is a host-only AP fixture;
 * production code and the crypto implementation are compiled unchanged.
 */
/* The station code against a fake driver (records the operations it is given) and a fake access point that answers like one:
 * scan results, open authentication, association, the WPA2 four-way handshake with a wrapped group key, then data in both directions. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <net/mac80211.h>
#include "lx_os.h"
#include "lx_plat.h"
#include "nw_abi.h"
#include "nw_crypto.h"
#include "nw_mac80211.h"
#include "nw_sta.h"

struct nw_wifi_stats nw_wifi_stats; unsigned int ath12k_debug_mask;
static u64 g_ms; static int fails; static unsigned checks;
static int g_query_scan_alloc, g_query_cancelled_scan;
#define CHECK(c, ...) do { checks++; if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } else if (getenv("V")) { printf("ok:   " __VA_ARGS__); printf("\n"); } } while (0)
static u64 p_now(void) { return g_ms; }
static void p_sleep(unsigned ms) { g_ms += ms; }
static void p_poll(void) {}
static void p_log(int l, const char *s) { if (getenv("V") || l <= 3) printf("[%d %llu] %s\n", l, (unsigned long long)g_ms, s); }
static void p_halt(void) { abort(); }
static void *p_alloc(size_t n) {
  if (g_query_scan_alloc) {
    g_query_scan_alloc = 0;
    CHECK(nw_wifi_scan(NULL, 0) == -EBUSY, "scan remains reserved while its request allocation reenters the ABI");
  }
  return malloc(n);
}
static void p_free(void *p) { free(p); }
static void *p_dma_alloc(size_t n, uint64_t *b) { void *p = calloc(1, n + 64); *b = (uint64_t)p; return p; }
static void p_dma_free(size_t n, void *p, uint64_t b) { (void)n; (void)b; free(p); }
static uint64_t p_dma_map(void *p, size_t n, int d) { (void)n; (void)d; return (uint64_t)p; }
static void p_dma_sync(uint64_t b, size_t n, int d) { (void)b; (void)n; (void)d; }
static int p_fw(const char *n, const u8 **d, size_t *s) { (void)n; (void)d; (void)s; return -1; }
static const struct lx_platform plat = { p_now, p_sleep, p_poll, p_log, p_halt, p_alloc, p_free, p_dma_alloc, p_dma_free, p_dma_map, p_dma_sync, p_fw };

/* ---- the access point ---- */
static u8 AP[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
static const char *SSID = "TestNet", *PASS = "correct horse";
static u8 g_pmk[32], g_ptk[48], g_anonce[32], g_snonce[32], g_sta_mac[6]; static u64 g_replay = 100;
static int g_stage, g_open, g_right, g_hold_m3, g_hold_scan, g_drop_auth, g_mixed_sae, g_required_mfp;
static int g_corrupt_m3;
static unsigned n_m2, n_m4, n_data, n_cancel, n_lost, n_group_reply, n_disable;
static int g_query_on_assign, g_fail_key;
static u8 last_m3[200]; static unsigned last_m3_len;
static u8 last_group[200]; static unsigned last_group_len;
void DitingWifiRandom(void *p, size_t n) { static unsigned counter; u8 *b=p; for(size_t i=0;i<n;i++)b[i]=(u8)(++counter); }
void nw_wifi_htt_stats(int type) { (void)type; }
void nw_wifi_rx_state(char *b,int n) { if(n>0)b[0]=0; }
void nw_wifi_ring_state(char *b,int n) { if(n>0)b[0]=0; }
void nw_wifi_tx_reasons(char *b,int n) { if(n>0)b[0]=0; }
static struct ieee80211_supported_band band;
static struct ieee80211_channel chans[3]; static struct ieee80211_rate rates[12];

static void inject(struct sk_buff *skb, int freq) { struct ieee80211_rx_status *st = IEEE80211_SKB_RXCB(skb); memset(st, 0, sizeof *st); st->freq = freq; st->signal = -47; nw_sta_rx(skb); kfree_skb(skb); }
static void beacon(void) {
  struct sk_buff *skb = alloc_skb(300, 0); struct ieee80211_mgmt *m = skb_put_zero(skb, 36);
  m->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_BEACON); memset(m->da, 0xff, 6); memcpy(m->sa, AP, 6); memcpy(m->bssid, AP, 6);
  m->u.beacon.beacon_int = cpu_to_le16(100); m->u.beacon.capab_info = cpu_to_le16(0x0411);
  u8 *e = skb_put(skb, 2 + strlen(SSID)); e[0] = 0; e[1] = strlen(SSID); memcpy(e + 2, SSID, strlen(SSID));
  e = skb_put(skb, 10); e[0] = 1; e[1] = 8; static const u8 r[8] = { 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 }; memcpy(e + 2, r, 8);
  e = skb_put(skb, 6); e[0] = 5; e[1] = 4; e[2] = 0; e[3] = 2; e[4] = 0; e[5] = 0;     /* TIM with DTIM period 2 */
  if (!g_open) { static const u8 rsn[] = { 48, 20, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 2, 0, 0 }; skb_put_data(skb, rsn, sizeof rsn); }
  if (g_mixed_sae) {
    static const u8 rsn_mixed[] = {48,24,1,0,0,0x0f,0xac,4,1,0,0,0x0f,0xac,4,2,0,0,0x0f,0xac,2,0,0x0f,0xac,8,0x80,0};
    /* Replace the previous RSN element with WPA2/WPA3 transition mode. */
    if (!g_open) { skb_trim(skb, skb->len-22); skb_put_data(skb,rsn_mixed,sizeof rsn_mixed); }
  }
  if (g_required_mfp && !g_open) skb->data[skb->len-2]|=0x40;
  inject(skb, 2437); }
static struct sk_buff *mgmt_to_sta(u16 stype, const void *body, unsigned n) {
  struct sk_buff *skb = alloc_skb(200 + n, 0); struct ieee80211_mgmt *m = skb_put_zero(skb, 24);
  m->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT | stype); memcpy(m->da, g_sta_mac, 6); memcpy(m->sa, AP, 6); memcpy(m->bssid, AP, 6); skb_put_data(skb, body, n); return skb; }
static void ap_eapol(const u8 *k, unsigned n) {
  struct sk_buff *skb = alloc_skb(400, 0); struct ieee80211_hdr *h = skb_put_zero(skb, 26);
  h->frame_control = cpu_to_le16(IEEE80211_FTYPE_DATA | IEEE80211_STYPE_QOS_DATA | IEEE80211_FCTL_FROMDS); memcpy(h->addr1, g_sta_mac, 6); memcpy(h->addr2, AP, 6); memcpy(h->addr3, AP, 6);
  static const u8 snap[8] = { 0xaa, 0xaa, 3, 0, 0, 0, 0x88, 0x8e }; skb_put_data(skb, snap, 8); skb_put_data(skb, k, n); inject(skb, 2437); }
static void be16(u8 *p, unsigned v) { p[0] = v >> 8; p[1] = v; }
static void aes_wrap(const u8 kek[16], const u8 *plain, unsigned n, u8 *out) {   /* RFC 3394 5.1 */
  uint32_t rk[44]; nw_aes128_expand(kek, rk); u8 a[8], b[16], r[256]; unsigned blocks = n / 8; memset(a, 0xa6, 8); memcpy(r, plain, n);
  for (int j = 0; j < 6; j++) for (unsigned i = 1; i <= blocks; i++) { memcpy(b, a, 8); memcpy(b + 8, r + (i - 1) * 8, 8); nw_aes128_encrypt(rk, b, b);
    uint64_t t = (uint64_t)blocks * j + i; memcpy(a, b, 8); for (int k = 0; k < 8; k++) a[k] ^= (u8)(t >> (56 - 8 * k)); memcpy(r + (i - 1) * 8, b + 8, 8); }
  memcpy(out, a, 8); memcpy(out + 8, r, n); }
static void send_msg1(void) {
  u8 f[200]; memset(f, 0, sizeof f); f[0] = 2; f[1] = 3; f[4] = 2; be16(f + 5, 0x008a); be16(f + 7, 16); f[16] = g_replay >> 8 & 0; f[16 - 1 + 0] = 0;
  for (int i = 0; i < 8; i++) f[9 + i] = g_replay >> (56 - 8 * i); memcpy(f + 17, g_anonce, 32); unsigned total = 99; be16(f + 2, total - 4); ap_eapol(f, total); }
static void derive(void) {
  u8 d[76]; const u8 *lo = memcmp(AP, g_sta_mac, 6) < 0 ? AP : g_sta_mac, *hi = lo == AP ? g_sta_mac : AP; memcpy(d, lo, 6); memcpy(d + 6, hi, 6);
  const u8 *nl = memcmp(g_anonce, g_snonce, 32) < 0 ? g_anonce : g_snonce, *nh = nl == g_anonce ? g_snonce : g_anonce; memcpy(d + 12, nl, 32); memcpy(d + 44, nh, 32);
  nw_prf(g_pmk, 32, "Pairwise key expansion", d, 76, g_ptk, 384); }
static int check_mic(const u8 *k, unsigned n) { u8 c[400], h[20]; memcpy(c, k, n); memset(c + 81, 0, 16); nw_hmac_sha1(g_ptk, 16, c, n, h); return !memcmp(h, k + 81, 16); }
static const u8 GTK[16] = { 0xa0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
static const u8 GTK2[16] = { 0xb0, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1 };
static const u8 *expected_gtk = GTK; static unsigned expected_gtk_index = 1;
static void send_msg3(void) {
  u8 kd[40], wrapped[48], f[200]; memset(kd, 0, sizeof kd); kd[0] = 0xdd; kd[1] = 22; kd[2] = 0x00; kd[3] = 0x0f; kd[4] = 0xac; kd[5] = 1; kd[6] = 1; kd[7] = 0; memcpy(kd + 8, GTK, 16); kd[24] = 0xdd; /* padding */
  aes_wrap(g_ptk + 16, kd, 32, wrapped);
  memset(f, 0, sizeof f); f[0] = 2; f[1] = 3; f[4] = 2; be16(f + 5, 0x13ca); be16(f + 7, 16); g_replay++; for (int i = 0; i < 8; i++) f[9 + i] = g_replay >> (56 - 8 * i);
  memcpy(f + 17, g_anonce, 32); be16(f + 97, 40); memcpy(f + 99, wrapped, 40); unsigned total = 99 + 40; be16(f + 2, total - 4);
  u8 h[20]; nw_hmac_sha1(g_ptk, 16, f, total, h); memcpy(f + 81, h, 16); memcpy(last_m3,f,total);last_m3_len=total;
  if(g_corrupt_m3) { /* correct PTK MIC, invalid AES-wrapped key data */
    f[120]^=1;memset(f+81,0,16);nw_hmac_sha1(g_ptk,16,f,total,h);memcpy(f+81,h,16);
  }
  ap_eapol(f, total); }
static void send_group(const u8 *key, unsigned index) {
  u8 kd[32] = {0}, wrapped[40], f[139] = {0}, mic[20];
  kd[0]=0xdd;kd[1]=22;kd[3]=0x0f;kd[4]=0xac;kd[5]=1;kd[6]=index;
  memcpy(kd+8,key,16);kd[24]=0xdd;aes_wrap(g_ptk+16,kd,sizeof kd,wrapped);
  f[0]=2;f[1]=3;f[4]=2;be16(f+2,sizeof f-4);be16(f+5,0x1382);be16(f+7,16);
  ++g_replay;for(int i=0;i<8;i++)f[9+i]=g_replay>>(56-8*i);
  be16(f+97,sizeof wrapped);memcpy(f+99,wrapped,sizeof wrapped);
  nw_hmac_sha1(g_ptk,16,f,sizeof f,mic);memcpy(f+81,mic,16);
  memcpy(last_group,f,sizeof f);last_group_len=sizeof f;ap_eapol(f,sizeof f);
}
static void inject_eth(const u8 *eth, unsigned n, u32 flags) {
  struct sk_buff *skb=alloc_skb(n,0);skb_put_data(skb,eth,n);
  struct ieee80211_rx_status *rs=IEEE80211_SKB_RXCB(skb);memset(rs,0,sizeof *rs);rs->flag=flags|RX_FLAG_8023;rs->freq=2437;
  nw_sta_rx(skb);kfree_skb(skb);
}

/* ---- the fake driver ---- */
static int n_tx, n_setkey, n_state; static int g_states[8]; static struct ieee80211_key_conf *g_pk, *g_gk; static struct sk_buff *g_last_data;
static u8 g_auth_seen, g_assoc_seen, g_msg2_seen, g_msg4_seen; static int g_scan_req;
static int op_start(struct ieee80211_hw *hw) { (void)hw; return 0; }
static int op_add_if(struct ieee80211_hw *hw, struct ieee80211_vif *v) { (void)hw; (void)v; return 0; }
static int op_hw_scan(struct ieee80211_hw *hw, struct ieee80211_vif *v, struct ieee80211_scan_request *r) { (void)hw; (void)v; g_scan_req = r->req.n_channels; return 0; }
static int op_add_ctx(struct ieee80211_hw *hw, struct ieee80211_chanctx_conf *c) { (void)hw; CHECK(c->def.chan->center_freq == 2437, "chanctx on the AP's channel"); return 0; }
static void op_cancel(struct ieee80211_hw *hw, struct ieee80211_vif *v) { (void)hw;(void)v;n_cancel++;if(!g_hold_scan)nw_sta_scan_done(true); }
static void op_rm_ctx(struct ieee80211_hw *hw, struct ieee80211_chanctx_conf *c) { (void)hw; (void)c; }
static int op_assign(struct ieee80211_hw *hw, struct ieee80211_vif *v, struct ieee80211_bss_conf *b, struct ieee80211_chanctx_conf *c) {
  (void)hw;(void)v;(void)b;(void)c;
  if(g_query_on_assign) {
    CHECK(nw_wifi_scan(NULL,0)==-EBUSY,"scan refused during channel assignment before AUTH");
    CHECK(nw_wifi_connect((const u8 *)SSID,strlen(SSID),0,PASS,strlen(PASS))==-EBUSY,"new request cannot replace a worker-owned join");
  }
  return 0;
}
static void op_unassign(struct ieee80211_hw *hw, struct ieee80211_vif *v, struct ieee80211_bss_conf *b, struct ieee80211_chanctx_conf *c) { (void)hw; (void)v; (void)b; (void)c; }
static int op_state(struct ieee80211_hw *hw, struct ieee80211_vif *v, struct ieee80211_sta *s, enum ieee80211_sta_state o, enum ieee80211_sta_state n) { (void)hw; (void)v; (void)s; (void)o; if (n_state < 8) g_states[n_state++] = n; return 0; }
static void op_link(struct ieee80211_hw *hw, struct ieee80211_vif *v, struct ieee80211_bss_conf *b, u64 ch) { (void)hw; (void)v; (void)b; (void)ch; }
static void op_cfg(struct ieee80211_hw *hw, struct ieee80211_vif *v, u64 ch) { (void)hw; (void)v; (void)ch; }
static int op_setkey(struct ieee80211_hw *hw, enum set_key_cmd cmd, struct ieee80211_vif *v, struct ieee80211_sta *s, struct ieee80211_key_conf *k) {
  (void)hw; (void)v; if (cmd != SET_KEY) {n_disable++;return 0;} if(g_fail_key)return -EIO;
  n_setkey++; if (k->flags & IEEE80211_KEY_FLAG_PAIRWISE) { CHECK(s != 0, "pairwise key has a station"); g_pk = k; CHECK(!memcmp(k->key, g_ptk + 32, 16), "TK equals the AP's"); }
  else { g_gk = k; CHECK(!memcmp(k->key, expected_gtk, 16) && k->keyidx == expected_gtk_index, "GTK and its index arrive intact"); } return 0; }
static void op_tx(struct ieee80211_hw *hw, struct ieee80211_tx_control *c, struct sk_buff *skb) {
  (void)hw; (void)c; n_tx++; struct ieee80211_hdr *h = (void *)skb->data; u16 fc = le16_to_cpu(h->frame_control);
  if (IEEE80211_SKB_CB(skb)->flags & IEEE80211_TX_CTL_HW_80211_ENCAP) {
    n_data++; if(g_last_data)kfree_skb(g_last_data);g_last_data=skb;return;
  }
  if (ieee80211_is_auth(h->frame_control)) { g_auth_seen = 1;
    if(g_drop_auth){kfree_skb(skb);return;} u8 b[6] = { 0, 0, 2, 0, 0, 0 }; inject(mgmt_to_sta(IEEE80211_STYPE_AUTH, b, 6), 2437); }
  else if (ieee80211_is_assoc_req(h->frame_control)) { g_assoc_seen = 1;
    const u8 *ie = skb->data + 24 + 4; unsigned left = skb->len - 28; int ssid_ok = 0, rsn_ok = 0;
    while (left >= 2 && left >= 2u + ie[1]) { if (ie[0] == 0 && ie[1] == strlen(SSID) && !memcmp(ie + 2, SSID, ie[1])) ssid_ok = 1; if (ie[0] == 48 && ie[1] == 20) rsn_ok = 1; left -= 2 + ie[1]; ie += 2 + ie[1]; }
    CHECK(ssid_ok, "association request names the network"); CHECK(rsn_ok == !g_open, "association request carries the RSN element when needed");
    u8 b[6] = { 0x11, 0x04, 0, 0, 0x01, 0xc0 }; inject(mgmt_to_sta(IEEE80211_STYPE_ASSOC_RESP, b, 6), 2437);
    if (!g_open) { g_stage = 1; send_msg1(); } }
  else if (ieee80211_is_data(h->frame_control) && skb->len > 26 + 8 && skb->data[26 + 6] == 0x88 && skb->data[26 + 7] == 0x8e) {
    const u8 *k = skb->data + 26 + 8; unsigned n = skb->len - 26 - 8; unsigned info = k[5] << 8 | k[6];
    if (info == 0x010a) { n_m2++;g_msg2_seen = 1; memcpy(g_snonce, k + 17, 32); derive(); int ok = check_mic(k, n); if (g_right) CHECK(ok, "message 2 MIC verifies under the PTK"); else CHECK(!ok, "message 2 MIC differs under a wrong passphrase"); if(!g_hold_m3)send_msg3(); }
    else if (info == 0x030a) { n_m4++;g_msg4_seen = 1; CHECK(check_mic(k, n), "message 4 MIC verifies"); }
    else if (info == 0x0302) {
      n_group_reply++;CHECK(check_mic(k,n),"group reply MIC verifies");
      CHECK(ieee80211_has_protected(h->frame_control) && IEEE80211_SKB_CB(skb)->control.hw_key==g_pk,"group reply uses the installed pairwise encryption key");
    } }
  else if (ieee80211_is_data(h->frame_control)) { if (g_last_data) kfree_skb(g_last_data); g_last_data = skb; (void)fc; return; }
  kfree_skb(skb); }
static const struct ieee80211_ops ops = { .start = op_start, .add_interface = op_add_if, .hw_scan = op_hw_scan, .cancel_hw_scan = op_cancel, .add_chanctx = op_add_ctx, .remove_chanctx = op_rm_ctx,
  .assign_vif_chanctx = op_assign, .unassign_vif_chanctx = op_unassign, .sta_state = op_state, .link_info_changed = op_link, .vif_cfg_changed = op_cfg, .set_key = op_setkey, .tx = op_tx };

static int g_scan_done, g_conn_done = -1, g_rx_n; static u8 g_rx_eth[200]; static size_t g_rx_len;
static void ev_scan(void) {
  g_scan_done++;
  if (g_query_cancelled_scan) {
    g_query_cancelled_scan = 0;
    CHECK(nw_wifi_scan(NULL, 0) == -EBUSY, "cancelled-scan callback cannot overwrite a dequeued join");
    CHECK(nw_wifi_connect((const u8 *)SSID, strlen(SSID), 0, PASS, strlen(PASS)) == -EBUSY,
          "join remains reserved before cancelled-scan completion returns");
  }
}
static void ev_conn(int r) { g_conn_done = r; }
static void ev_lost(int r) { (void)r;n_lost++; }
static void ev_rx(const u8 *e, size_t n) { g_rx_n++; g_rx_len = n; memcpy(g_rx_eth, e, n); }
static void spin(int ms) { for (int i = 0; i < ms; i++) { nw_sta_run(); lx_pump(); g_ms++; } }
static void crypto_vectors(void) {
  u8 key[32], out[32];
  static const u8 pmk[32]={0xf4,0x2c,0x6f,0xc5,0x2d,0xf0,0xeb,0xef,0x9e,0xbb,0x4b,0x90,0xb3,0x8a,0x5f,0x90,0x2e,0x83,0xfe,0x1b,0x13,0x5a,0x70,0xe2,0x3a,0xed,0x76,0x2e,0x97,0x10,0xa1,0x2e};
  nw_pbkdf2_sha1("password",8,(const u8 *)"IEEE",4,4096,out,32);
  CHECK(!memcmp(out,pmk,32),"WPA2 password/IEEE known PMK vector");
  static const u8 hmac[20]={0xb6,0x17,0x31,0x86,0x55,0x05,0x72,0x64,0xe2,0x8b,0xc0,0xb6,0xfb,0x37,0x8c,0x8e,0xf1,0x46,0xbe,0x00};
  memset(key,0x0b,20);nw_hmac_sha1(key,20,(const u8 *)"Hi There",8,out);
  CHECK(!memcmp(out,hmac,20),"RFC 2202 HMAC-SHA1 vector");
  static const u8 wrapped[24]={0x1f,0xa6,0x8b,0x0a,0x81,0x12,0xb4,0x47,0xae,0xf3,0x4b,0xd8,0xfb,0x5a,0x7b,0x82,0x9d,0x3e,0x86,0x23,0x71,0xd2,0xcf,0xe5};
  static const u8 plain[16]={0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
  for(unsigned i=0;i<16;i++)key[i]=i;
  CHECK(!nw_aes_unwrap(key,wrapped,sizeof wrapped,out) && !memcmp(out,plain,16),"RFC 3394 AES key unwrap vector");
}

int main(void) {
  crypto_vectors();
  lx_plat = &plat; lx_root_init();
  for (int i = 0; i < 12; i++) { static const int br[12] = { 10, 20, 55, 110, 60, 90, 120, 180, 240, 360, 480, 540 }; rates[i].bitrate = br[i]; }
  chans[0].center_freq = 2412; chans[1].center_freq = 2437; chans[2].center_freq = 2462; for (int i = 0; i < 3; i++) chans[i].band = NL80211_BAND_2GHZ;
  band.channels = chans; band.n_channels = 3; band.bitrates = rates; band.n_bitrates = 12; band.band = NL80211_BAND_2GHZ;
  struct ieee80211_hw *hw = ieee80211_alloc_hw_nm(64, &ops, "t"); hw->wiphy->bands[NL80211_BAND_2GHZ] = &band; hw->sta_data_size = 64; hw->vif_data_size = 64; hw->chanctx_data_size = 16;
  memcpy(hw->wiphy->perm_addr, "\x02\xaa\xbb\xcc\xdd\xee", 6); memcpy(g_sta_mac, hw->wiphy->perm_addr, 6);
  struct nw_wifi_events ev = { ev_scan, ev_conn, ev_lost, ev_rx }; nw_wifi_set_events(&ev);
  nw_pbkdf2_sha1(PASS, strlen(PASS), (const u8 *)SSID, strlen(SSID), 4096, g_pmk, 32); for (int i = 0; i < 32; i++) g_anonce[i] = 0x40 + i;
  nw_sta_ready(hw);
  spin(5); CHECK(g_scan_req == 3, "first scan covers all three channels (%d)", g_scan_req);
  beacon(); nw_sta_scan_done(false); CHECK(g_scan_done == 1 && nw_wifi_bss_count() == 1, "scan finished with one network");
  struct nw_bss_info bi; CHECK(!nw_wifi_bss_get(0, &bi) && bi.ssid_len == strlen(SSID) && bi.freq_mhz == 2437 && bi.signal_dbm == -47, "network details");
  /* wrong passphrase: handshake must fail with a key error, not hang */
  nw_wifi_connect((const u8 *)SSID, strlen(SSID), 0, "wrong password", 14); spin(9000);
  CHECK(g_conn_done == NW_RES_BAD_KEY, "wrong passphrase gives a key error (%d)", g_conn_done);
  /* the right one */
  g_right = 1; g_query_on_assign=1; g_conn_done = -1; g_stage = 0; n_state = 0; n_setkey = 0; g_msg2_seen = g_msg4_seen = 0;
  nw_wifi_connect((const u8 *)SSID, strlen(SSID), 0, PASS, strlen(PASS)); spin(3000);
  CHECK(g_auth_seen && g_assoc_seen && g_msg2_seen && g_msg4_seen, "auth, assoc, message 2 and message 4 all went out");
  CHECK(g_conn_done == NW_RES_OK, "connected (%d)", g_conn_done); CHECK(n_setkey == 2, "two keys installed (%d)", n_setkey);
  CHECK(n_state >= 4 && g_states[0] == IEEE80211_STA_NONE && g_states[n_state - 1] == IEEE80211_STA_AUTHORIZED, "station state ends authorized");
  struct nw_wifi_status st; nw_wifi_status_get(&st); CHECK(st.state == NW_STA_UP && st.freq_mhz == 2437, "status says up");
  g_query_on_assign=0;
  /* data out */
  u8 eth[60]; memset(eth, 0, sizeof eth); memcpy(eth, AP, 6); memcpy(eth + 6, g_sta_mac, 6); eth[12] = 0x08; eth[13] = 0x00; memcpy(eth + 14, "payload-bytes", 13);
  nw_wifi_tx(eth, 27); spin(5);
  CHECK(g_last_data != 0, "a data frame reached the driver");
  if (g_last_data) { struct ieee80211_hdr *h = (void *)g_last_data->data; struct ieee80211_tx_info *info = IEEE80211_SKB_CB(g_last_data);
    (void)h;
    CHECK(info->flags & IEEE80211_TX_CTL_HW_80211_ENCAP, "Ethernet MSDU requests hardware encapsulation");
    CHECK(info->control.hw_key == g_pk && info->control.vif != 0, "frame names the pairwise key and the interface");
    CHECK(g_last_data->len==27 && !memcmp(g_last_data->data,eth,27), "Ethernet header and payload arrive intact at hardware"); }
  /* data in */
  { struct sk_buff *skb = alloc_skb(200, 0); struct ieee80211_hdr *h = skb_put_zero(skb, 26); h->frame_control = cpu_to_le16(IEEE80211_FTYPE_DATA | IEEE80211_STYPE_QOS_DATA | IEEE80211_FCTL_FROMDS | IEEE80211_FCTL_PROTECTED);
    memcpy(h->addr1, g_sta_mac, 6); memcpy(h->addr2, AP, 6); memcpy(h->addr3, "\x02\x77\x66\x55\x44\x33", 6); u8 snap[8] = { 0xaa, 0xaa, 3, 0, 0, 0, 0x08, 0x06 }; skb_put_data(skb, snap, 8); skb_put_data(skb, "ARPDATA", 7);
    struct ieee80211_rx_status *rs = IEEE80211_SKB_RXCB(skb); memset(rs, 0, sizeof *rs); rs->freq = 2437; rs->flag = RX_FLAG_DECRYPTED | RX_FLAG_IV_STRIPPED; nw_sta_rx(skb); kfree_skb(skb); }
  CHECK(g_rx_n == 1 && g_rx_len == 14 + 7 && g_rx_eth[12] == 0x08 && g_rx_eth[13] == 0x06 && !memcmp(g_rx_eth, g_sta_mac, 6) && !memcmp(g_rx_eth + 6, "\x02\x77\x66\x55\x44\x33", 6), "frame from the AP becomes an Ethernet frame");
  unsigned rx_before=g_rx_n;u8 reply[60]={0};memcpy(reply,g_sta_mac,6);memcpy(reply+6,AP,6);reply[12]=8;
  inject_eth(reply,sizeof reply,RX_FLAG_DECRYPTED);
  CHECK(g_rx_n==rx_before+1 && g_rx_len==sizeof reply && !memcmp(g_rx_eth,reply,sizeof reply),"hardware-decapped data preserves Ethernet bytes");
  inject_eth(reply,sizeof reply,0);inject_eth(reply,sizeof reply,RX_FLAG_DECRYPTED|RX_FLAG_FAILED_FCS_CRC);
  reply[5]^=1;inject_eth(reply,sizeof reply,RX_FLAG_DECRYPTED);reply[5]^=1;
  CHECK(g_rx_n==rx_before+1,"undecrypted, corrupt and other-destination data are dropped");
  const u8 short_reason[2]={0};for(unsigned n=24;n<26;n++)inject(mgmt_to_sta(IEEE80211_STYPE_DEAUTH,short_reason,n-24),2437);
  nw_wifi_status_get(&st);CHECK(st.state==NW_STA_UP,"truncated deauthentication cannot end the link");
  /* AP group-key rotation and retransmissions must preserve hardware PNs. */
  expected_gtk=GTK2;expected_gtk_index=2;unsigned keys_before=n_setkey;
  send_group(GTK2,2);spin(5);
  CHECK(n_setkey==keys_before+1 && n_group_reply==1,"rotated GTK is installed and acknowledged");
  ap_eapol(last_group,last_group_len);spin(5);
  CHECK(n_setkey==keys_before+1 && n_group_reply==2,"lost group reply is resent without key reinstallation");
  send_group(GTK2,2);spin(5);
  CHECK(n_setkey==keys_before+1 && n_group_reply==3,"new-counter retry of the same GTK retains receive PN");
  /* Backpressure is bounded, and disconnect rejects newly submitted data. */
  for(unsigned i=0;i<128;i++)CHECK(!nw_wifi_tx(eth,27),"bounded TX queue accepts entry %u",i);
  CHECK(nw_wifi_tx(eth,27)==-ENOBUFS,"129th queued packet reports backpressure");
  unsigned disabled_before=n_disable;
  nw_wifi_disconnect();CHECK(nw_wifi_tx(eth,27)==-ENETDOWN,"disconnect rejects data immediately");spin(5);
  CHECK(n_disable==disabled_before+3,"disconnect removes pairwise and both group-key indexes");
  expected_gtk=GTK;expected_gtk_index=1;
  /* open network */
  nw_wifi_disconnect(); spin(5); g_open = 1; beacon(); g_conn_done = -1; n_state = 0;
  nw_wifi_connect((const u8 *)SSID, strlen(SSID), 0, 0, 0); spin(3000);
  CHECK(g_conn_done == NW_RES_OK, "open network connects (%d)", g_conn_done);
  /* Native state snapshots and requests must survive an AP change. */
  CHECK(nw_wifi_connect((const u8 *)SSID,0,0,0,0)<0,"empty SSID rejected");
  CHECK(nw_wifi_connect((const u8 *)SSID,strlen(SSID),0,"gggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggg",64)<0,"nonhex raw PMK rejected");
  CHECK(nw_wifi_scan(NULL,33)<0,"oversized directed scan rejected");
  unsigned old_data=n_data, old_lost=n_lost;
  CHECK(nw_wifi_tx(eth,27)==0,"old-link frame can be queued");
  SSID="SecondNet";AP[5]++;g_open=0;g_mixed_sae=1;beacon();
  nw_pbkdf2_sha1(PASS,strlen(PASS),(const u8 *)SSID,strlen(SSID),4096,g_pmk,32);
  g_conn_done=-1;n_setkey=0;
  CHECK(!nw_wifi_connect((const u8 *)SSID,strlen(SSID),0,PASS,strlen(PASS)),"new join accepted while connected");
  CHECK(nw_wifi_scan(NULL,0)<0,"a scan cannot replace a queued join");
  spin(1000);nw_wifi_status_get(&st);
  CHECK(g_conn_done==NW_RES_OK && st.state==NW_STA_UP && st.ssid_len==strlen(SSID) && !memcmp(st.ssid,SSID,st.ssid_len),"switch to WPA2/WPA3-compatible AP completes");
  CHECK(n_lost==old_lost+1 && n_data==old_data,"switch retires old link and drops its queued data");
  CHECK(n_setkey==2,"new network installs its own PTK/GTK");
  unsigned before_m4=n_m4;
  ap_eapol(last_m3,last_m3_len);spin(5);
  CHECK(n_m4==before_m4+1 && n_setkey==2,"repeated M3 gets M4 without reinstalling keys");
  unsigned before_m2=n_m2;send_msg1();spin(5);
  CHECK(n_m2==before_m2,"late M1 cannot reset an authorized link");

  nw_wifi_disconnect();spin(5);g_hold_m3=1;g_conn_done=-1;
  nw_wifi_connect((const u8 *)SSID,strlen(SSID),0,PASS,strlen(PASS));spin(5);
  u8 first_nonce[32];memcpy(first_nonce,g_snonce,32);send_msg1();spin(5);
  CHECK(!memcmp(first_nonce,g_snonce,32),"M1 retransmission reuses the same SNonce");
  before_m4=n_m4;g_corrupt_m3=1;send_msg3();spin(5);g_corrupt_m3=0;
  CHECK(n_m4==before_m4 && g_conn_done==-1,"invalid wrapped key data does not authorize or consume its replay counter");
  ap_eapol(last_m3,last_m3_len);spin(5);ap_eapol(last_m3,last_m3_len);spin(400);
  CHECK(n_m4==before_m4+2 && g_conn_done==NW_RES_OK,"valid same-counter M3 and lost M4 retries complete authorization");
  u8 malformed_m3[200];memcpy(malformed_m3,last_m3,last_m3_len);malformed_m3[3]--;
  before_m4=n_m4;ap_eapol(malformed_m3,last_m3_len);spin(5);
  CHECK(n_m4==before_m4,"inconsistent EAPOL/key-data lengths are rejected");
  g_hold_m3=0;

  /* Turning the radio off while a scan cancellation is pending must cancel
   * the join too. A late hardware scan completion cannot revive it. */
  nw_wifi_disconnect();spin(5);g_hold_scan=1;g_conn_done=-1;
  CHECK(!nw_wifi_scan(NULL,0),"scan accepted while idle");spin(1);
  CHECK(!nw_wifi_connect((const u8 *)SSID,strlen(SSID),0,PASS,strlen(PASS)),"join can wait for scan cancellation");spin(1);
  nw_wifi_disconnect();spin(5);nw_sta_scan_done(true);spin(20);nw_wifi_status_get(&st);
  CHECK(g_conn_done==NW_RES_FAILED && st.state==NW_STA_IDLE,"disconnect cancels waiting join despite late scan completion");
  CHECK(nw_wifi_tx(eth,27)<0,"data rejected while disconnected");
  g_hold_scan=0;

  g_drop_auth=1;g_conn_done=-1;
  nw_wifi_connect((const u8 *)SSID,strlen(SSID),0,PASS,strlen(PASS));spin(5);
  nw_wifi_disconnect();spin(9000);nw_wifi_status_get(&st);
  CHECK(g_conn_done==NW_RES_FAILED && st.state==NW_STA_IDLE,"disconnect during authentication finishes the request");
  g_drop_auth=0;

  g_required_mfp=1;beacon();g_conn_done=-1;
  nw_wifi_connect((const u8 *)SSID,strlen(SSID),0,PASS,strlen(PASS));spin(5);
  CHECK(g_conn_done==NW_RES_UNSUPPORTED,"mandatory PMF refused instead of an insecure join");
  g_required_mfp=0;
  beacon();g_fail_key=1;g_conn_done=-1;
  nw_wifi_connect((const u8 *)SSID,strlen(SSID),0,PASS,strlen(PASS));spin(500);nw_wifi_status_get(&st);
  CHECK(g_conn_done==NW_RES_FAILED && st.state==NW_STA_IDLE,"hardware key failure cannot authorize the link");g_fail_key=0;
  /* A native client may supply the 64 hexadecimal digits of an already
   * derived PMK. It must produce the same verified four-way handshake. */
  char pmk_hex[65];static const char digits[]="0123456789abcdef";
  for(unsigned i=0;i<32;i++){pmk_hex[2*i]=digits[g_pmk[i]>>4];pmk_hex[2*i+1]=digits[g_pmk[i]&15];}pmk_hex[64]=0;
  g_conn_done=-1;nw_wifi_connect((const u8 *)SSID,strlen(SSID),0,pmk_hex,64);spin(500);
  CHECK(g_conn_done==NW_RES_OK,"raw PMK connects with a verified MIC");
  nw_sta_link_lost();beacon();beacon();spin(5200);nw_wifi_status_get(&st);
  CHECK(st.state==NW_STA_UP,"real AP traffic keeps a link after a beacon-miss notification");
  nw_sta_link_lost();AP[5]++;beacon();beacon();AP[5]--;spin(5200);nw_wifi_status_get(&st);
  CHECK(st.state==NW_STA_IDLE,"nearby AP beacons cannot keep a lost link alive");
  nw_wifi_disconnect();spin(5);
  unsigned completed_scans = g_scan_done;
  CHECK(!nw_wifi_scan(NULL, 0), "pending scan accepted before an immediate join");
  g_query_cancelled_scan = 1; g_conn_done = -1;
  CHECK(!nw_wifi_connect((const u8 *)SSID, strlen(SSID), 0, PASS, strlen(PASS)), "join supersedes a scan not yet submitted to hardware");
  spin(500); nw_wifi_status_get(&st);
  CHECK(g_scan_done == completed_scans + 1 && !g_query_cancelled_scan && g_conn_done == NW_RES_OK && st.state == NW_STA_UP,
        "superseded scan completes exactly once and the original join succeeds");
  nw_wifi_disconnect(); spin(5);
  completed_scans = g_scan_done;
  CHECK(!nw_wifi_scan(NULL, 0), "pending scan accepted before radio-off");
  nw_wifi_disconnect(); spin(5);
  CHECK(g_scan_done == completed_scans + 1, "radio-off completes a scan cancelled before hardware submission");
  g_query_scan_alloc = 1;
  CHECK(!nw_wifi_scan(NULL, 0), "new scan accepted after pending cancellation"); spin(5);
  CHECK(!g_query_scan_alloc, "hardware scan allocation exercised the reentrant request");
  nw_sta_scan_done(false); spin(5);
  if(g_last_data){kfree_skb(g_last_data);g_last_data=0;}
  printf("%s: %u checks, %d failures\n", fails ? "FAILED" : "PASS",checks,fails); return fails ? 1 : 0; }
