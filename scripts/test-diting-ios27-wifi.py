#!/usr/bin/env python3
"""Automated live hardware Wi-Fi test runner for Diting iOS 27 over USB (05ac:1881)."""
import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
USB = ROOT / "scripts/diting-usb-libusb.py"
CAPTURE = ROOT / "scripts/diting-screen-capture.py"


def usb_cmd(command: str, timeout: int = 8) -> str:
    res = subprocess.run(
        [sys.executable, str(USB), "cmd", *command.split()],
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    if res.returncode != 0:
        raise RuntimeError(f"USB cmd '{command}' failed ({res.returncode}): {res.stderr.strip()}")
    return res.stdout


def capture_screen(out_path: Path, timeout: int = 12) -> bytes:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    res = subprocess.run(
        [sys.executable, str(CAPTURE), str(out_path)],
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    if res.returncode != 0 or not out_path.is_file():
        raise RuntimeError(f"Screen capture to {out_path} failed ({res.returncode}): {res.stderr.strip()}")
    return out_path.read_bytes()


def parse_kv(line: str) -> dict:
    return dict(re.findall(r"([A-Za-z0-9_]+)=([^\s]+)", line))


def parse_int_prefix(val: str, default: int = 0) -> int:
    m = re.match(r"(-?\d+)", val or "")
    return int(m.group(1)) if m else default


def find_line(text: str, prefix: str) -> str:
    return next((line for line in text.splitlines() if line.startswith(prefix)), "")


def main() -> int:
    ap = argparse.ArgumentParser(description="Automated Diting iOS 27 Wi-Fi hardware test via USB")
    ap.add_argument("--join", help="Optional SSID to join during live test")
    ap.add_argument("--psk", default="", help="Optional WPA2 passphrase for --join")
    ap.add_argument("--scan-wait", type=int, default=12, help="Max seconds to wait for fresh scan completion")
    ap.add_argument("--join-wait", type=int, default=25, help="Max seconds to wait for association + DHCP")
    ap.add_argument("--skip-power", action="store_true", help="Skip wifi power 0/1 toggle cycle")
    ap.add_argument("--ui-probe", action="store_true",
                    help="Unlock via HID, open Settings -> Wi-Fi, verify live scan & UI power switch with screenshots")
    ap.add_argument("--capture-dir", type=Path, default=Path("/tmp/diting-wifi-ui-probe"),
                    help="Directory for --ui-probe before/after PNG screenshots")
    args = ap.parse_args()

    checks = []

    def record(name: str, ok: bool, detail: str):
        checks.append((name, ok, detail))
        print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}")

    # 1. Status baseline
    st_text = usb_cmd("wifi status")
    print(st_text.strip())
    st = parse_kv(find_line(st_text, "WIFI_STATUS "))
    rd = parse_kv(find_line(st_text, "WIFI_RADIO "))

    ready = st.get("ready") == "1" and st.get("power") == "1" and parse_int_prefix(st.get("channels")) > 0
    record("1.interface_ready", ready,
           f"state={st.get('state')} power={st.get('power')} mac={st.get('mac')} ch={st.get('channels')}")

    # 2. Kernel Apple80211 + 540-byte KEV protocol self-test (also verifies 2.4/5 GHz channel filter)
    self_text = usb_cmd("wifi selftest")
    print(self_text.strip())
    self_line = find_line(self_text, "WIFI_SELFTEST ")
    sf = parse_kv(self_line)
    m_ch = re.search(r"ch=0\(n=(\d+)\)", self_line)
    filtered_ch = int(m_ch.group(1)) if m_ch else 0
    record("2.apple80211_selftest", sf.get("status") == "PASS" and 0 < filtered_ch < 80,
           f"{self_line} (filtered_2g5g_ch={filtered_ch})")

    # 3. Trigger a fresh on-air scan and verify scan counter increments & APs populate
    scans_before = parse_int_prefix(rd.get("scans"))
    scan_text = usb_cmd("wifi scan")
    queued = "queued=1" in scan_text
    scans_after = scans_before
    bss_count = parse_int_prefix(rd.get("bss"))
    deadline = time.monotonic() + args.scan_wait
    while time.monotonic() < deadline:
        time.sleep(1.5)
        cur = usb_cmd("wifi status")
        cur_rd = parse_kv(find_line(cur, "WIFI_RADIO "))
        scans_after = parse_int_prefix(cur_rd.get("scans"))
        bss_count = parse_int_prefix(cur_rd.get("bss"))
        if scans_after > scans_before:
            break
    record("3.fresh_air_scan", queued and scans_after > scans_before and bss_count > 0,
           f"queued={queued} scans={scans_before}->{scans_after} bss={bss_count}")

    bss_text = usb_cmd("wifi bss")
    bss_lines = [l for l in bss_text.splitlines() if l.startswith("WIFI_BSS ")]
    sample_ssids = [parse_kv(l).get("ssid", "?") for l in bss_lines[:5]]
    record("4.bss_list_readable", len(bss_lines) > 0, f"returned={len(bss_lines)} sample={sample_ssids}")

    # 5. Live IOKit quiet, software AES, IOPlatformUUID, and wifid/configd protocol binding
    st2 = usb_cmd("wifi status")
    io2 = parse_kv(find_line(st2, "WIFI_IOCTL "))
    err2 = find_line(st2, "WIFI_ERRORS") or "none"
    diag_text = usb_cmd("wifi diag")
    print(diag_text.strip())
    iokit_line = find_line(diag_text, "WIFI_IOKIT ")
    aes_line = find_line(diag_text, "WIFI_AES ")
    plat_line = find_line(diag_text, "WIFI_PLATFORM ")
    clients_line = find_line(diag_text, "WIFI_CLIENTS ")
    iokit_kv = parse_kv(iokit_line)
    aes_kv = parse_kv(aes_line)
    plat_kv = parse_kv(plat_line)

    iokit_quiet = iokit_kv.get("busy") == "0"
    aes_ready = aes_kv.get("state") == "1" and aes_kv.get("kat") == "1" and aes_kv.get("reg") == "1"
    plat_ok = (not plat_line) or (plat_kv.get("uuid", "none") != "none")
    record("5.iokit_aes_platform_ready", iokit_quiet and aes_ready and plat_ok,
           f"{iokit_line} | {aes_line} | {plat_line or 'WIFI_PLATFORM=n/a'}")

    wifid_scans = parse_int_prefix(io2.get("scan_req")) + parse_int_prefix(io2.get("scan_multi"))
    wifid_results = parse_int_prefix(io2.get("scan_res"))
    wifid_caps = parse_int_prefix(io2.get("caps"))
    wifid_uc_refused = parse_int_prefix(io2.get("uc_refused"))
    ev_err = parse_int_prefix(io2.get("ev_err"))
    clients_procs = parse_kv(clients_line).get("procs", "")
    record("6.wifid_protocol_activity",
           wifid_caps > 0 and wifid_uc_refused == 0 and ev_err == 0 and "wifid" in clients_procs and "configd" in clients_procs,
           f"procs={clients_procs} total={io2.get('total')} uc={io2.get('uc')} caps={wifid_caps} scan_reqs={wifid_scans} scan_res={wifid_results} ev_ok={io2.get('ev_ok')} ev_err={ev_err} uc_refused={wifid_uc_refused}")
    print(f"[INFO] errors={err2}")

    # 7. USB-driven power toggle off -> on cycle + event delivery verification
    if not args.skip_power:
        ev_before = parse_int_prefix(io2.get("ev_ok"))
        p0 = usb_cmd("wifi power 0")
        time.sleep(0.5)
        st_mid = parse_kv(find_line(usb_cmd("wifi status"), "WIFI_STATUS "))
        p1 = usb_cmd("wifi power 1")
        time.sleep(0.8)
        st_on_raw = usb_cmd("wifi status")
        st_on = parse_kv(find_line(st_on_raw, "WIFI_STATUS "))
        io_on = parse_kv(find_line(st_on_raw, "WIFI_IOCTL "))
        ev_after = parse_int_prefix(io_on.get("ev_ok"))
        # Note: live wifid immediately reacts to kEvPowerChanged and may restore
        # power=1 before the subsequent status read, so verify power=0 on p0's reply.
        p0_ok = "rc=0" in p0 and "power=0" in p0
        p1_ok = "rc=0" in p1 and st_on.get("power") == "1"
        ok_pow = (p0_ok and p1_ok and
                  ev_after >= ev_before + 2 and parse_int_prefix(io_on.get("ev_err")) == 0)
        record("7.power_toggle_cycle", ok_pow,
               f"p0_ok={p0_ok} mid={st_mid.get('power')} on={st_on.get('power')} ev_ok={ev_before}->{ev_after}")

    # 8. Automated HID UI Settings -> Wi-Fi live scan & toggle switch verification with screenshots
    if args.ui_probe:
        cap_dir = args.capture_dir
        png_before = capture_screen(cap_dir / "01-before.png")
        io_pre_ui = parse_kv(find_line(usb_cmd("wifi status"), "WIFI_IOCTL "))
        scan_reqs_base = parse_int_prefix(io_pre_ui.get("scan_req")) + parse_int_prefix(io_pre_ui.get("scan_multi"))
        scan_res_base = parse_int_prefix(io_pre_ui.get("scan_res"))
        # Swipe up from bottom edge: unlocks Lock Screen or returns to Home Screen.
        print(usb_cmd("hid swipe 500 965 500 350 300").strip())
        time.sleep(2.5)
        # Tap Settings icon on Home Screen and allow cold launch time.
        print(usb_cmd("hid tap 150 725").strip())
        time.sleep(6.0)
        # Tap Wi-Fi row in Settings (no-op if Settings restored directly to Wi-Fi subpage).
        print(usb_cmd("hid tap 400 412").strip())
        ui_scan_reqs = scan_reqs_base
        ui_scan_res = scan_res_base
        st_list_raw = ""
        retried_tap = False
        deadline = time.monotonic() + 12.0
        while time.monotonic() < deadline:
            time.sleep(1.5)
            st_list_raw = usb_cmd("wifi status")
            st_list = parse_kv(find_line(st_list_raw, "WIFI_STATUS "))
            io_list = parse_kv(find_line(st_list_raw, "WIFI_IOCTL "))
            ui_scan_reqs = parse_int_prefix(io_list.get("scan_req")) + parse_int_prefix(io_list.get("scan_multi"))
            ui_scan_res = parse_int_prefix(io_list.get("scan_res"))
            if st_list.get("power") == "0":
                # If Settings restored to Wi-Fi page with switch OFF, turn it ON first.
                print(usb_cmd("hid tap 830 418").strip())
                continue
            if ui_scan_reqs > 0 and ui_scan_res > 0 and (ui_scan_reqs > scan_reqs_base or ui_scan_res > scan_res_base):
                break
            if not retried_tap and time.monotonic() > deadline - 7.0:
                retried_tap = True
                print(usb_cmd("hid tap 400 412").strip())
        io_list = parse_kv(find_line(st_list_raw, "WIFI_IOCTL "))
        power_set_base = parse_int_prefix(io_list.get("power_set"))
        png_list = capture_screen(cap_dir / "02-wifi-list.png")
        record("8.ui_settings_wifi_scan",
               ui_scan_reqs > 0 and ui_scan_res > 0 and parse_int_prefix(io_list.get("uc_refused")) == 0,
               f"scan_reqs={scan_reqs_base}->{ui_scan_reqs} scan_res={scan_res_base}->{ui_scan_res} shot={cap_dir / '02-wifi-list.png'}")

        # Toggle Wi-Fi switch OFF via Settings UI
        print(usb_cmd("hid tap 830 418").strip())
        time.sleep(2.5)
        st_ui_off_raw = usb_cmd("wifi status")
        st_ui_off = parse_kv(find_line(st_ui_off_raw, "WIFI_STATUS "))
        io_ui_off = parse_kv(find_line(st_ui_off_raw, "WIFI_IOCTL "))
        png_off = capture_screen(cap_dir / "03-wifi-off.png")
        off_power = st_ui_off.get("power")
        off_pset = parse_int_prefix(io_ui_off.get("power_set"))

        # Toggle Wi-Fi switch ON via Settings UI
        print(usb_cmd("hid tap 830 418").strip())
        time.sleep(4.5)
        st_ui_on_raw = usb_cmd("wifi status")
        st_ui_on = parse_kv(find_line(st_ui_on_raw, "WIFI_STATUS "))
        io_ui_on = parse_kv(find_line(st_ui_on_raw, "WIFI_IOCTL "))
        png_on = capture_screen(cap_dir / "04-wifi-on.png")
        on_power = st_ui_on.get("power")
        on_pset = parse_int_prefix(io_ui_on.get("power_set"))
        on_scan_res = parse_int_prefix(io_ui_on.get("scan_res"))

        ui_toggle_ok = (
            off_power == "0" and on_power == "1" and
            off_pset >= power_set_base + 1 and
            on_pset >= power_set_base + 2 and
            on_pset >= 2 and
            parse_int_prefix(io_ui_on.get("uc_refused")) == 0 and
            parse_int_prefix(io_ui_on.get("ev_err")) == 0 and
            png_off != png_on
        )
        record("9.ui_settings_wifi_switch", ui_toggle_ok,
               f"power={off_power}->{on_power} power_set={power_set_base}->{off_pset}->{on_pset} scan_res={ui_scan_res}->{on_scan_res} shots={cap_dir}")

    # 10. Optional real-AP association + DHCP IPv4 check
    if args.join:
        cmd = f"wifi join {args.join}" + (f" {args.psk}" if args.psk else "")
        j_out = usb_cmd(cmd)
        print(j_out.strip())
        up = False
        ipv4 = "none"
        last_state = "?"
        deadline = time.monotonic() + args.join_wait
        while time.monotonic() < deadline:
            time.sleep(1.5)
            cur = usb_cmd("wifi status")
            cur_st = parse_kv(find_line(cur, "WIFI_STATUS "))
            last_state = cur_st.get("state", "?")
            ipv4 = cur_st.get("ipv4", "none")
            if last_state.startswith("UP"):
                up = True
                if ipv4 != "none":
                    break
        record("10.ap_join_and_dhcp", up and ipv4 != "none", f"ssid={args.join} state={last_state} ipv4={ipv4}")

    failed = [c for c in checks if not c[1]]
    print(f"\nSUMMARY: {len(checks) - len(failed)}/{len(checks)} PASS")
    return 0 if not failed else 1


if __name__ == "__main__":
    raise SystemExit(main())
