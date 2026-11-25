#!/usr/bin/env python3
"""Capture a live RGB332 screenshot PNG from Diting over USB CDC (05ac:1881)."""
import argparse
import ctypes
import re
import struct
import sys
import time
import zlib
from pathlib import Path

LIB = ctypes.CDLL("/usr/lib/libusb-1.0.so.0")
LIB.libusb_init.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
LIB.libusb_open_device_with_vid_pid.argtypes = [ctypes.c_void_p, ctypes.c_uint16, ctypes.c_uint16]
LIB.libusb_open_device_with_vid_pid.restype = ctypes.c_void_p
LIB.libusb_close.argtypes = [ctypes.c_void_p]
LIB.libusb_exit.argtypes = [ctypes.c_void_p]
LIB.libusb_set_auto_detach_kernel_driver.argtypes = [ctypes.c_void_p, ctypes.c_int]
LIB.libusb_claim_interface.argtypes = [ctypes.c_void_p, ctypes.c_int]
LIB.libusb_release_interface.argtypes = [ctypes.c_void_p, ctypes.c_int]
LIB.libusb_control_transfer.argtypes = [
    ctypes.c_void_p, ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint16, ctypes.c_uint16,
    ctypes.c_void_p, ctypes.c_uint16, ctypes.c_uint
]
LIB.libusb_bulk_transfer.argtypes = [
    ctypes.c_void_p, ctypes.c_uint8, ctypes.c_void_p, ctypes.c_int,
    ctypes.POINTER(ctypes.c_int), ctypes.c_uint
]


def write_png(path: Path, width: int, height: int, rgb_rows: list[bytes]) -> None:
    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + row for row in rgb_rows)
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 6))
        + chunk(b"IEND", b"")
    )
    path.write_bytes(png)


def decode_rgb332(hex_row: str) -> bytes:
    data = bytes.fromhex(hex_row)
    out = bytearray(len(data) * 3)
    for i, c in enumerate(data):
        r = (c & 0xE0) | ((c & 0xE0) >> 3) | ((c & 0xC0) >> 6)
        g = ((c & 0x1C) << 3) | (c & 0x1C) | ((c & 0x18) >> 3)
        b = ((c & 0x03) << 6) | ((c & 0x03) << 4) | ((c & 0x03) << 2) | (c & 0x03)
        out[i * 3] = r
        out[i * 3 + 1] = g
        out[i * 3 + 2] = b
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("output", type=Path, help="destination PNG path")
    ap.add_argument("--step", type=int, default=4, help="pixel stride (2..8, default 4 -> 305x678)")
    ap.add_argument("--y0", type=int, default=0, help="start Y in panel pixels")
    ap.add_argument("--y1", type=int, default=2712, help="end Y in panel pixels")
    args = ap.parse_args()

    ctx = ctypes.c_void_p()
    if LIB.libusb_init(ctypes.byref(ctx)) != 0:
        raise SystemExit("libusb_init failed")
    h = LIB.libusb_open_device_with_vid_pid(ctx, 0x05AC, 0x1881)
    if not h:
        LIB.libusb_exit(ctx)
        raise SystemExit("05ac:1881 not found")
    try:
        LIB.libusb_set_auto_detach_kernel_driver(h, 1)
        for iface in (0, 1):
            LIB.libusb_claim_interface(h, iface)
        LIB.libusb_control_transfer(h, 0x21, 0x22, 0, 0, None, 0, 1000)
        time.sleep(0.02)
        LIB.libusb_control_transfer(h, 0x21, 0x22, 1, 0, None, 0, 1000)
        time.sleep(0.02)

        buf = (ctypes.c_ubyte * 16384)()
        xfer = ctypes.c_int()

        def cmd(c: str) -> str:
            payload = (c + "\n").encode()
            arr = (ctypes.c_ubyte * len(payload)).from_buffer_copy(payload)
            LIB.libusb_bulk_transfer(h, 0x01, arr, len(payload), ctypes.byref(xfer), 1000)
            end_marker = f"DitingUSB END {c}".encode()
            out = bytearray()
            t0 = time.monotonic()
            while time.monotonic() - t0 < 2.0:
                r = LIB.libusb_bulk_transfer(h, 0x81, buf, len(buf), ctypes.byref(xfer), 100)
                if r == 0 and xfer.value > 0:
                    out.extend(buf[: xfer.value])
                    if end_marker in out:
                        break
            return out.decode("latin1", errors="replace")

        step = max(2, min(8, args.step))
        rows_per_cmd = max(1, min(6, 3600 // (2 * ((1220 + step - 1) // step) + 12)))
        rgb_rows: list[bytes] = []
        width = 0
        y = args.y0
        while y < args.y1:
            txt = cmd(f"display pixels band {y} {rows_per_cmd} {step}")
            m = re.search(r"BAND w=(\d+) h=(\d+) y0=\d+ step=\d+ cols=(\d+)", txt)
            if not m:
                raise RuntimeError(f"unexpected response at y={y}: {txt[:200]!r}")
            panel_h = int(m.group(2))
            cols = int(m.group(3))
            width = cols
            got = 0
            for line in txt.splitlines():
                rm = re.match(r"^R(\d{4})\s+([0-9a-f]+)$", line)
                if rm:
                    ry = int(rm.group(1))
                    if ry >= args.y1:
                        continue
                    rgb_rows.append(decode_rgb332(rm.group(2)))
                    got += 1
            if got == 0:
                break
            y += got * step
            if y >= panel_h:
                break

        if not rgb_rows or not width:
            raise RuntimeError("no rows captured")
        write_png(args.output, width, len(rgb_rows), rgb_rows)
        print(f"wrote {args.output} ({width}x{len(rgb_rows)}, step={step})")
        return 0
    finally:
        for iface in (0, 1):
            LIB.libusb_release_interface(h, iface)
        LIB.libusb_close(h)
        LIB.libusb_exit(ctx)


if __name__ == "__main__":
    raise SystemExit(main())
