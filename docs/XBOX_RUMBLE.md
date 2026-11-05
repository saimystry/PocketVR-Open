# PC Xbox rumble forwarding

The input helper's **Xbox rumble** checkbox defaults on. Rumble is allowed only
for an active, neutrally armed XInput lease. Background use still needs the
background-controller opt-in. Generic joystick fallback has no Xbox rumble.
Inactive input does not continuously write to a pad or own a native game's rumble.

SteamVR left virtual-hand pulses map to the left/low-frequency motor; right-hand
pulses map to the right/high-frequency motor. Amplitude maps to 16-bit strength.
XInput cannot reproduce arbitrary frequency hints. Each hand's latest command
replaces its motor deadline; zero duration/amplitude cancels it. Pulses cap at 500 ms.

The driver routes haptics to the active input owner (desktop gamepad or phone),
never both. A bounded mailbox supplies desktop pulses in input.desktop replies
only for a valid session/lease and explicit haptics request. Events older than
100 ms or from the future are discarded; disabled readers drain pending events.
Phone haptics retain their route only while phone input owns the controllers.

An independent five-millisecond actuator worker owns XInputSetState. It expires
pulses despite blocked RPC, stops after 250 ms without a renewed lease, and zeros
the old device on pause/device changes/teardown. Device changes need a new
ownership update before accepting pulses. Input generations reject replies that
cross focus/pause/checkbox changes. Independent hand tracking is not added.

Verification: all 154 Windows checks and dependency audit pass. Five rumble
checks cover deadlines/amplitude, invalid/stale/future events, bounded mailbox,
owner routing, pause/device changes, watchdog, no replay and teardown zero.
Actual XInput slot 0 accepted a bounded 150 ms start and zero/stop command.
Physical strength and SteamVR title-generated routing still need real-use tests.
