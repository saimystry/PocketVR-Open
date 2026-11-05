# Validation and acceptance plan

Current evidence is in `spec.md`. The steps below describe required validation; listing a step here does not mean it has passed. Installer implementation and host Release checks exist; live installation and full application/hardware acceptance remain unfinished.

## Host checks before milestones

Run `scripts/build.ps1 -Configuration Debug` (and Release before packaging), the exact installed-dependency/SDK license audit, Android debug/test APK builds and `scripts/audit_android.py`. Check 16 KB APK alignment. Run Android native instrumentation on ARM64 when a device is available. Keep build-only and execution results distinct.

Security/regression checks cover malformed lengths/fields, replay, corrupt tags, no replay-window advance on failed authentication, identity pin mismatch, mutually approved first pairing, one missing pin, closed/expired pairing windows, forgetting a connected phone, and no stale-session media acceptance. Exercise cancellation/reconnect repeatedly. Never log codes, keys or credentials.

Verify whole-frame FEC recovery/drop deadlines and bounded memory under random/burst loss and reordering. With an encoder stall, pending raw frames must be replaced by the newest usable frame; texture leases must return on reset. Verify left/right composition, bounds, overlay ordering, linear-light alpha, render-pose identity and decode pixel quality with synthetic reference images. Inspect rendered test output.

## Required hardware matrix: RX 590 + Pixel 4a

All entries are pending target hardware and completed feature paths.

| Test | Procedure | Expected outcome / evidence |
|---|---|---|
| Install and pair | Install APK/Windows package, open explicit pairing, type phone code on PC, verify matching PC code on phone | Both pins persist; restarting either side reconnects with fresh session keys; wrong code/identity fails |
| Wi-Fi | Same private clean 5 GHz LAN; discover, manual IP and QR | Same host selected, correct ports, stable video/tracking, no unintended Internet traffic |
| USB tethering | Enable Android tethering, select detected RNDIS interface | UDP path selected; unplug/replug safely returns to waiting/reconnect |
| USB adb | Authorize adb, select helper, verify reverse rules | TCP media uses bounded frame queue and small independent pose/input messages; disconnect cannot build latency backlog |
| Optics | Display calibration grid; tune QR/manual IPD/FOV/lens profile | Correct eye order, stereo convergence and matching driver projections; no unexpected clipping; inspect with actual viewer |
| Rotation | Slow yaw, pitch, roll and rapid turns; recenter through each input route | Correct axes/signs, no slow-turn bias suppression, bounded prediction, accurate frame/timewarp association |
| Fallback/reprojection | Temporarily reduce video to 30 fps while rotating head | Physical display/timewarp stays at 60 Hz; old frames reproject with newest pose |
| 30-minute gameplay | SteamVR Home and a simple available OpenVR title, then a normal game | No disconnect, thermal shutdown or persistent judder; record FPS, late count, battery and thermal timeline |
| Network loss | Impairment proxy at 1%, 3%, 8% random and burst loss; bandwidth cliff | Bounded drop/recovery, IDR rate limiting, no freeze/backlog; adaptation reasons logged |
| Recovery | Pull USB, disable/re-enable Wi-Fi, switch app/screen, restart SteamVR | Resources released on pause; bounded reconnect; no vrserver crash or leaked texture/decoder leases |
| Controllers | Each mapping preset, two controllers, second phone, gamepad/touch/haptics | Input profiles bind; intended actions/haptic amplitude route to the correct device |
| Presets | Fast/Balanced/Quality, H.264/HEVC, relevant resolutions | Record actual selected encoder/decoder, bitrate accuracy, timings and visual quality |
| Live format adaptation | Matching builds, adaptive video on; sustained processing/network pressure followed by clean headroom, on tether UDP and adb TCP | Advertised resolution then FPS steps; same authenticated connection, live tracking, fresh IDR and correct both-eye image; preferences retained; slow recovery; no stale-format flashes. Reject/time out a change in a controlled fixture: restore previous format and stop automatic retries. Measure actual Pixel/AMF switch duration separately |
| Uninstall | Uninstall after shutdown | Driver/overlay registration and only owned firewall/install files removed; document settings retention behavior |

Record per-frame CSV stage p50/p95/p99 and sustained bitrate/FPS. Targets from the requirements: rotational latency below 25 ms; video p50 below 70 ms USB and 90 ms clean Wi-Fi; encode p95 below 8 ms at 2160x1080; Pixel decode p95 below 10 ms; sustained 60 Hz reprojection and zero steady-state dropped vsyncs. Targets are measurement goals, not guarantees. Report failures and bottlenecks honestly.
