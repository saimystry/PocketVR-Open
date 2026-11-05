#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Opt-in SteamVR/Android check on an unlocked, adb-authorized phone and trusted local PC.

Installs nothing. Requires current debug/test APKs and a running PocketVR driver.
Automatically confirms the test phone's code on this PC and saves real pairing pins.
"""
import argparse
import importlib.util
import ipaddress
import json
from pathlib import Path
import queue
import shutil
import shlex
import subprocess
import threading
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', help='Private IPv4 address of this PC as seen by the phone')
    parser.add_argument('--usb-adb', action='store_true', help='Use existing adb reverse TCP mappings and loopback')
    parser.add_argument('--auto', action='store_true', help='Exercise phone automatic selection; --server supplies the manual fallback address')
    parser.add_argument('--expect-route', default='', help='Assert the selected phone route label, e.g. USB tethering')
    parser.add_argument('--adb', default=shutil.which('adb'))
    parser.add_argument('--output', required=True, type=Path, help='Local JSON measurement artifact')
    args = parser.parse_args()
    if not args.server and not args.usb_adb: parser.error('--server is required for Wi-Fi')
    address = ipaddress.IPv4Address(args.server or '127.0.0.1')
    if not address.is_private or (address.is_loopback != args.usb_adb) or not args.adb:
        parser.error('Supply a private LAN address and adb on PATH or --adb.')
    if args.usb_adb:
        mappings = subprocess.run([args.adb, 'reverse', '--list'], check=True,
                                  capture_output=True, text=True).stdout
        pairs = {tuple(line.split()[-2:]) for line in mappings.splitlines() if line.strip()}
        if not {('tcp:9943', 'tcp:9943'), ('tcp:9944', 'tcp:9944')}.issubset(pairs):
            raise RuntimeError('USB mappings are missing. Run setup_usb.ps1 after the adb connection is stable.')
    root = Path(__file__).resolve().parents[1]
    spec = importlib.util.spec_from_file_location('tracker_rpc', root / 'pc/tools/examples/tracker_bridge.py')
    bridge = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(bridge)
    if bridge.call({'method': 'network.status'})['session'] is not None:
        raise RuntimeError('Disconnect the existing phone session before running this check.')
    process = None
    lines = queue.Queue()
    outcome = None
    result_code = None
    try:
        process = subprocess.Popen([args.adb, 'shell', 'am', 'instrument', '-w', '-r', '-e',
            'live_server', str(address), '-e', 'live_usb', str(args.usb_adb).lower(),
            '-e', 'live_auto', str(args.auto).lower(),
            '-e', 'live_expected_route', shlex.quote(args.expect_route),
            'com.pocketvr.test/com.pocketvr.test.NativeInstrumentation'],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8')
        def read():
            for line in process.stdout:
                lines.put(line.rstrip())
            lines.put(None)
        reader = threading.Thread(target=read, daemon=True)
        reader.start()
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            try:
                line = lines.get(timeout=0.2)
            except queue.Empty:
                continue
            if line is None:
                break
            if 'INSTRUMENTATION_RESULT: live_session=' in line:
                raw = json.loads(line.split('live_session=', 1)[1])
                # Export measurements only, excluding identities, discovery and pairing details.
                outcome = {key: raw[key] for key in ('clock_ready', 'connected', 'decoder', 'renderer',
                    'sensors', 'quality', 'phone_stats', 'poses_sent', 'stream', 'selected_route', 'selected_ip', 'connections', 'headset_transitions_without_reconnect')}
            elif 'INSTRUMENTATION_CODE:' in line:
                result_code = int(line.split(':', 1)[1])
            elif 'stream=' in line:
                print(line, flush=True)
        if process.poll() is None:
            process.wait(timeout=2)
        if outcome is None or result_code != -1:
            raise RuntimeError('Live check failed or timed out; inspect instrumentation and driver logs.')
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(outcome, indent=2), encoding='utf-8')
        print(f'Live phone/SteamVR check passed; measurements saved to {args.output}')
    finally:
        if process and process.poll() is None:
            process.kill()
            subprocess.run([args.adb, 'shell', 'am', 'force-stop', 'com.pocketvr'], check=False,
                           timeout=10, stdout=subprocess.DEVNULL)


if __name__ == '__main__':
    main()
