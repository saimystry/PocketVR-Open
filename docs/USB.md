# USB cable streaming

## Automatic transport selection

Leave **Choose connection automatically** enabled on the phone and tap Connect.
The preference is USB tethering (UDP), adb reverse (TCP), then the saved Wi-Fi/LAN
address (UDP). Enable USB tethering yourself in Android settings; the app's
**Open USB tethering settings** button opens that page. PocketVR does not enable
USB debugging or tethering silently. The PC launcher maintains adb mappings.

The app identifies active rndis/usb/ncm interfaces and matches discovered PC
addresses against their actual IPv4 prefix. Discovery queries each local subnet;
a USB-only unicast discovery sweep is capped at 254 hosts per five seconds on
small private tether subnets. Once found, only known hosts are queried directly.
It does not sweep Wi-Fi/VPN subnets. Discovery hints never establish trust:
a saved PC identity is checked by the normal signed handshake across addresses.
First use still requires the matching pairing code. Ambiguous first-use PCs must
be selected explicitly. Forget this PC removes its saved address aliases.

A working stream keeps its route. Failed routes receive a 30-second cooldown;
a connection attempt gets up to 20 seconds before another route is tried.
Pairing confirmation is not interrupted by the selector. Surface recreation
reuses the selected endpoint. PC listeners refresh after an interface change
stays stable for three samples, so enabling tethering after startup is supported;
this refresh reconnects an existing session rather than migrating it seamlessly.

The status line shows the selected connection and IP. Turn automatic selection
off for manual Wi-Fi or **USB (adb reverse)**; selecting a discovered PC or
scanning its connection QR also selects manual mode. The desktop's reserved
connection.mode setting is unchanged; selection runs in the phone app.
Windows firewall rules must permit the cable adapter's network profile. PocketVR
does not silently change network trust categories or broaden firewall rules.

## Manual adb USB


The desktop **USB setup** button now opens a native wizard that discovers adb,
lists phones/authorization states and sets up or removes the two verified reverse
rules. It works before SteamVR starts. See [USB_WIZARD.md](USB_WIZARD.md). The
PowerShell helper below remains available for scripted setup.

Connect an adb-authorized Android phone using a data cable. Start SteamVR with
the PocketVR driver, then run from PowerShell with Android platform-tools on PATH:

```powershell
. D:\Tools\Enter-PocketVRTools.ps1
& scripts/setup_usb.ps1
```

Enable **USB (adb reverse)** in the phone app and connect. The address becomes
`127.0.0.1`; the default control port is 9943. Control and encrypted media/poses
use separate TCP reverse mappings (9943 and 9944). Pairing and confirmation still
apply. Wi-Fi is not required. The helper supports `-Serial`, `-ControlPort`,
`-MediaPort`, `-AdbPath`, `-WhatIf` and `-Remove`. It preserves unrelated mappings
and refuses conflicting destinations. Rerun it after unplug/replug if mappings
disappear. USB debugging authorization must be accepted on the phone by its owner.

The PC exposes TCP media only on loopback. The phone explicitly negotiates TCP;
manual mode does not fall back to Wi-Fi. Automatic mode can select a different route after failure. `network.status.session.transport` reports
`tcp`, and `media_peer_verified` requires an authenticated initial media ping.
The legacy `udp_peer_verified` remains false for TCP. Media socket closure ends
the session, and the phone reconnects using the normal authenticated flow.

TCP media keeps the existing encrypted packet format and checked record framing,
disables redundant FEC, enables TCP_NODELAY, bounds the socket send buffer and
compressed frame queue, and closes a record stream after a send deadline rather
than emitting a corrupted partial record. Media writes allow 10 ms to start a record, then skip it with the stream intact; a record already partly written gets 100 ms to finish. This covers transient
adb scheduling; whole-frame age remains bounded. Initial media binding allows
five seconds, including phone decoder/reassembly initialization.

This feature supplies explicit adb cable mode, a setup script and a desktop USB
wizard. USB tethering detection and selection are described above. Automatic platform-tools
installation remains separate work. Physical unplug/replug, extended gameplay and thermal
acceptance must not be inferred from short automated checks.

## Current home evidence (2026-11-03)

Actual Pixel automatic selection chose USB tethering at 192.168.16.129 over the
simultaneously available adb/Wi-Fi paths. The fullscreen UDP check passed with
1,236 decoded frames, 1,215 latched frames and 8,000 tracking poses. It asserts the
route label, UDP transport and continuous counters; it is not a 30-minute or
physical unplug/replug acceptance result. All 144 Windows and Pixel native/
renderer/security/sensor/transport checks pass. Run the opt-in check with:

```powershell
python scripts/test_live_session.py --server <WiFi-PC-IP> --auto --expect-route "USB tethering" --adb D:\Tools\android-sdk\platform-tools\adb.exe --output D:\PocketVR-Latest\build\live-tether.json
```

Start a VR test scene first, leave the phone still for calibration and disconnect
any existing phone session. The script confirms this test phone's matching code
on the local PC and stores normal pairing pins.
