# Phone thermal and encoder-performance protection

`power.thermal_guard` is now implemented and defaults on. The phone samples the
public Android thermal status and negotiates authenticated `phone.thermal`
reports. Initial capabilities carry a current status; subsequent reports have
monotonic sequence, bounded sample age and three-second receive freshness.
Malformed/replayed/out-of-range reports are ignored without advancing state.
Older peers that do not negotiate the capability do not send these messages.

Policy: Android moderate status caps video input at 45 fps; severe at 30 fps;
critical/emergency/shutdown status pauses video. Tracking/control remain active.
The renderer blacks video and displays a stereo cooling warning even with normal
stats off. Missing/stale hot reports cannot invent cooling. Recovery needs ten
seconds of suitable fresh headroom per step, resuming at 30 fps after a pause.

The encoder worker also measures a bounded rolling encode p95. After twenty
samples, sustained cost above 90 percent of the current cadence budget reduces
60 -> 45 -> 30 input fps. Recovery needs ten seconds below 80 percent of the next
cadence budget. Stale/invalid timing cannot invent headroom. Disabling the guard
restores the configured cap. This measures encoder/conversion/queue processing;
it is not a GPU-temperature or full application GPU-utilization measurement.

The pre-encode cadence gate uses phase accumulation so 45-on-60 stays about 45,
not 30. Policy skips have separate counters. Pausing returns texture leases
without accumulating a queue, and resume requests a fresh IDR. The phone display
and tracking stay at their normal cadence. This is an effective input cap, not
automatic resolution changes or a renegotiation of the encoder/decoder format.
Full resolution/FPS quality renegotiation and other pacing work remain separate.

Status: `network.status.session.adaptation.guard` exposes cap, skip count, encode
p95/sample count, thermal support/level/freshness/rejections and reason. The phone
setup screen shows the effective cap/reason. No protection settings in Android
or Windows are disabled or replaced; the operating systems keep their safeguards.

Verification: 161 Windows checks/audit pass. Pure tests cover status levels,
pause/cooldown, stale reports, processing overload, invalid samples, manual off,
cadence and replay. Real authenticated UDP and TCP fixtures pause encoded video,
continue fresh poses, release repeated texture leases and resume with IDR.
Actual Pixel native/GPU/viewer/security/sensor suites pass, including a rendered
cooling message with stats off; the saved two-eye output was visually inspected.
Real tether run: one connection, 1,537 decoded frames, 8,689 poses; fresh OS status
0, encode p95 13.558 ms, cap 60, no rejected thermal messages. Runtime off/on also
applied during a live session. No deliberate heating, GPU TDR or 30-minute thermal
acceptance was performed. Vendor thermal reporting and physical tuning remain
acceptance work; do not claim temperatures or physical motion-to-photon latency.
