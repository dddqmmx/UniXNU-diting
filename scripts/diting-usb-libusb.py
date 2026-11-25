#!/usr/bin/env python3
"""Talk directly to Diting's USB CDC console via libusb-1.0 (no cdc_acm module required)."""
import argparse
import ctypes
import sys
import time
from pathlib import Path

LIB = ctypes.CDLL("/usr/lib/libusb-1.0.so.0")

LIB.libusb_init.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
LIB.libusb_init.restype = ctypes.c_int
LIB.libusb_exit.argtypes = [ctypes.c_void_p]
LIB.libusb_open_device_with_vid_pid.argtypes = [ctypes.c_void_p, ctypes.c_uint16, ctypes.c_uint16]
LIB.libusb_open_device_with_vid_pid.restype = ctypes.c_void_p
LIB.libusb_close.argtypes = [ctypes.c_void_p]
LIB.libusb_set_auto_detach_kernel_driver.argtypes = [ctypes.c_void_p, ctypes.c_int]
LIB.libusb_set_auto_detach_kernel_driver.restype = ctypes.c_int
LIB.libusb_claim_interface.argtypes = [ctypes.c_void_p, ctypes.c_int]
LIB.libusb_claim_interface.restype = ctypes.c_int
LIB.libusb_release_interface.argtypes = [ctypes.c_void_p, ctypes.c_int]
LIB.libusb_release_interface.restype = ctypes.c_int
LIB.libusb_control_transfer.argtypes = [
    ctypes.c_void_p, ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint16, ctypes.c_uint16,
    ctypes.c_void_p, ctypes.c_uint16, ctypes.c_uint
]
LIB.libusb_control_transfer.restype = ctypes.c_int
LIB.libusb_bulk_transfer.argtypes = [
    ctypes.c_void_p, ctypes.c_uint8, ctypes.c_void_p, ctypes.c_int,
    ctypes.POINTER(ctypes.c_int), ctypes.c_uint
]
LIB.libusb_bulk_transfer.restype = ctypes.c_int

VID_PIDS = [(0x05AC, 0x1881), (0xFFFF, 0x0128), (0x2717, 0xFF08), (0x05AC, 0x12A8)]


def open_diting(ctx):
    for vid, pid in VID_PIDS:
        h = LIB.libusb_open_device_with_vid_pid(ctx, vid, pid)
        if h:
            return h, (vid, pid)
    return None, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("action", choices=["capture", "cmd", "fastboot", "status", "gpu", "display"])
    ap.add_argument("extra", nargs="*")
    ap.add_argument("--seconds", type=float, default=6.0)
    ap.add_argument("--timeout", type=float, default=8.0)
    ap.add_argument("--output", type=Path)
    args = ap.parse_args()

    ctx = ctypes.c_void_p()
    rc = LIB.libusb_init(ctypes.byref(ctx))
    if rc != 0:
        raise SystemExit(f"libusb_init failed: {rc}")
    h = None
    try:
        h, vp = open_diting(ctx)
        if not h:
            raise SystemExit("Diting USB console not found (checked 05ac:1881, ffff:0128, 2717:ff08, 05ac:12a8)")
        print(f"[usb] opened {vp[0]:04x}:{vp[1]:04x}", file=sys.stderr)
        LIB.libusb_set_auto_detach_kernel_driver(h, 1)
        for iface in (0, 1):
            rc = LIB.libusb_claim_interface(h, iface)
            if rc != 0:
                raise SystemExit(f"claim_interface({iface}) failed: {rc}")
        # Toggle DTR low then high via CDC SET_CONTROL_LINE_STATE (0x21, 0x22)
        LIB.libusb_control_transfer(h, 0x21, 0x22, 0, 0, None, 0, 1000)
        time.sleep(0.05)
        rc = LIB.libusb_control_transfer(h, 0x21, 0x22, 1, 0, None, 0, 1000)
        if rc < 0:
            raise SystemExit(f"SET_CONTROL_LINE_STATE(DTR=1) failed: {rc}")
        time.sleep(0.05)

        buf = (ctypes.c_ubyte * 16384)()
        xfer = ctypes.c_int()

        def read_chunk(ms=250):
            r = LIB.libusb_bulk_transfer(h, 0x81, buf, len(buf), ctypes.byref(xfer), ms)
            if r == 0 and xfer.value > 0:
                return bytes(buf[:xfer.value])
            if r in (-7, 0):  # LIBUSB_ERROR_TIMEOUT or zero
                return b""
            raise RuntimeError(f"bulk IN error {r}")

        def write_all(data: bytes):
            arr = (ctypes.c_ubyte * len(data)).from_buffer_copy(data)
            r = LIB.libusb_bulk_transfer(h, 0x01, arr, len(data), ctypes.byref(xfer), 2000)
            if r != 0:
                raise RuntimeError(f"bulk OUT error {r}")

        received = bytearray()
        if args.action == "capture":
            deadline = time.monotonic() + args.seconds
            err = None
            try:
                while time.monotonic() < deadline:
                    chunk = read_chunk(200)
                    if chunk:
                        received.extend(chunk)
            except Exception as exc:
                err = exc
            if args.output:
                args.output.parent.mkdir(parents=True, exist_ok=True)
                args.output.write_bytes(received)
            sys.stdout.write(received.decode("utf-8", errors="replace"))
            sys.stdout.flush()
            if err is not None:
                sys.stderr.write(f"[usb] capture stopped early ({len(received)} bytes saved): {err}\n")
                return 2
            return 0

        if args.action == "fastboot":
            cmd = "reboot fastboot"
        elif args.action == "cmd":
            cmd = " ".join(args.extra)
        else:
            cmd = " ".join([args.action] + args.extra)

        begin = f"DitingUSB BEGIN {cmd}\n".encode("ascii")
        end = f"DitingUSB END {cmd}\n".encode("ascii")
        write_all(b"\x15\n" + (cmd + "\n").encode("ascii"))
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            chunk = read_chunk(200)
            if chunk:
                received.extend(chunk)
                first = received.find(begin)
                last = received.find(end, first + len(begin)) if first >= 0 else -1
                if last >= 0:
                    if args.output:
                        args.output.parent.mkdir(parents=True, exist_ok=True)
                        args.output.write_bytes(received)
                    sys.stdout.write(received[first:last + len(end)].decode("utf-8", errors="replace"))
                    return 0
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_bytes(received)
        sys.stderr.write(f"[usb] timeout waiting for END {cmd}; captured {len(received)} bytes\n")
        sys.stdout.write(received.decode("utf-8", errors="replace"))
        return 1
    finally:
        if h:
            for iface in (1, 0):
                LIB.libusb_release_interface(h, iface)
            LIB.libusb_close(h)
        LIB.libusb_exit(ctx)


if __name__ == "__main__":
    raise SystemExit(main())
