# Headset-view transitions without reconnecting

The phone retains the same attached SurfaceView and window for preview and
headset mode. Enter/exit changes layout, setup visibility and immersive flags;
it no longer reparents the view or calls setContentView again. The EGL renderer
already queries the resized surface dimensions. On the tested Pixel this keeps
the decoder, transport, pose counters and authenticated session alive through
enter, exit and re-enter, including orientation changes.

Native status exposes a cumulative `connections` counter per client lifetime,
incremented at authenticated ready. The opt-in live test records it and video/
pose counters before entering; every subsequent sample must retain the same
connection and monotonically advancing counters. The test leaves headset view,
returns, and requires continued decoded and latched frame delivery, no renderer
error and no new calibration. A connected-but-frozen renderer cannot pass.

Evidence (2026-11-03/04): Android app/test builds and notices/alignment audits
pass. Pixel native, renderer, viewer, sensor, security and handle checks pass.
Strict automatic tether test: **one connection**, all three transitions completed,
**1,442 decoded / 1,429 latched frames / 8,645 poses**. The saved screenshot was
inspected: both diagnostic arrows are upright with matching stereo orientation.
The surrounding Windows build remains 154 passing checks.

This removes the avoidable UI-induced reconnect. Actual surface loss, app pause,
device/codec errors and server restarts retain their existing stop/reconnect
lifecycle. Codec-only recovery improvements and long app-switch acceptance remain
separate work. Other Android devices can recreate surfaces differently and need
their own acceptance; this is not a guarantee of universal surface preservation.
