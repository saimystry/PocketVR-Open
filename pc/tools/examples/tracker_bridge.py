#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Feed an original synthetic tracker trace into the running local driver.

No third-party tracker SDK is required. This demonstrates the documented local
API. Device 0 (HMD) must not compete with phone tracking. Devices 1/2
(left/right controller) may run alongside the phone; no tracking SDK is included.
"""
import argparse
import ctypes
from ctypes import wintypes
import json
import math
import struct
import time

kernel = ctypes.WinDLL("kernel32", use_last_error=True)
kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
kernel.CreateFileW.restype = wintypes.HANDLE
kernel.WaitNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD]
kernel.ReadFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
kernel.WriteFile.argtypes = kernel.ReadFile.argtypes
kernel.CloseHandle.argtypes = [wintypes.HANDLE]
PIPE = r"\\.\pipe\PocketVR.v1"


def call(request):
    if not kernel.WaitNamedPipeW(PIPE, 1000):
        raise ctypes.WinError(ctypes.get_last_error())
    handle = kernel.CreateFileW(PIPE, 0xC0000000, 0, None, 3, 0, None)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        body = json.dumps(request, allow_nan=False).encode()
        if not 0 < len(body) <= 65536:
            raise ValueError("Request size")
        packet = struct.pack("<I", len(body)) + body
        data = ctypes.create_string_buffer(packet)
        count = wintypes.DWORD()
        if not kernel.WriteFile(handle, data, len(packet), ctypes.byref(count), None) or count.value != len(packet):
            raise ctypes.WinError(ctypes.get_last_error())

        def read(size):
            output = bytearray()
            while len(output) < size:
                buffer = ctypes.create_string_buffer(size - len(output))
                if not kernel.ReadFile(handle, buffer, len(buffer), ctypes.byref(count), None) or count.value == 0:
                    raise ctypes.WinError(ctypes.get_last_error())
                output.extend(buffer.raw[:count.value])
            return output

        size = struct.unpack("<I", read(4))[0]
        if not 0 < size <= 65536:
            raise ValueError("Invalid response size")
        response = json.loads(read(size))
        if not response.get("ok"):
            raise RuntimeError(response.get("error", "Driver rejected request"))
        return response
    finally:
        kernel.CloseHandle(handle)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=float, default=10)
    parser.add_argument("--rate", type=int, default=200)
    parser.add_argument("--recenter", action="store_true")
    parser.add_argument("--device", type=int, choices=(0, 1, 2), default=0, help="0 HMD, 1 left, 2 right controller")
    parser.add_argument("--space", choices=("tracking", "standing"), default="tracking")
    parser.add_argument("--release", action="store_true", help="Explicitly release bridge pose ownership")
    parser.add_argument("--status", action="store_true", help="Read bridge ownership/pose diagnostics")
    args = parser.parse_args()
    if not 1 <= args.rate <= 500 or not 0 < args.seconds <= 3600:
        parser.error("rate must be 1..500 and seconds 0..3600")
    if args.status:
        print(json.dumps(call({"method": "bridge.status"}), indent=2))
    elif args.release:
        print(call({"method": "bridge.release", "device": args.device}))
    elif args.recenter:
        print(call({"method": "recenter"}))
    else:
        start = time.monotonic()
        tick = start
        try:
            while time.monotonic() - start < args.seconds:
                elapsed = time.monotonic() - start
                angle = 0.4 * math.sin(elapsed)
                pose = {"method": "bridge.pose", "device": args.device,
                    "orientation": [0, math.sin(angle / 2), 0, math.cos(angle / 2)],
                    "angular_velocity": [0, 0.4 * math.cos(elapsed), 0],
                    "timestamp_pc_ns": time.monotonic_ns(), "space": args.space}
                if args.device:
                    pose["position"] = [(-0.2 if args.device == 1 else 0.2) + 0.05 * math.sin(elapsed), 1.3, -0.4]
                    pose["linear_velocity"] = [0.05 * math.cos(elapsed), 0, 0]
                call(pose)
                tick += 1 / args.rate
                time.sleep(max(0, tick - time.monotonic()))
        finally:
            if args.device: call({"method": "bridge.release", "device": args.device})
        print(call({"method": "status"}))
