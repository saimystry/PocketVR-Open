# Desktop setup and first live streaming check

Run the built `pc/control/pocketvr_control.exe`. This desktop utility provides SteamVR launch, connection addresses, live tracking/encoder/phone status, pairing-window controls, six-digit code submission, paired-phone revocation, recenter, mouse/controller input launching, an advanced schema-validated settings editor, log-folder access and a diagnostics ZIP export.

1. Build the Windows driver and apps. Register the package once:
   `pwsh -File scripts/register_driver.ps1 -DriverPath D:\PocketVR-builds\windows-debug\package\pocketvr`
   The helper locates the registered SteamVR runtime; `-SteamVRPath` overrides detection. `-WhatIf` validates without registering. `-Remove` unregisters this exact package, retaining files/settings and other drivers. Restart SteamVR after registration changes. Development Debug builds require the development toolchain/runtime; this is not a release installer.
2. In the desktop utility choose **Start SteamVR**. Wait for a PC address. The window retries if the driver is stopped and disables driver actions while unavailable.
3. Open pairing, then connect the phone using the PC's LAN address. Enter the phone's six-digit code on the desktop. Verify the matching PC-confirmed code on the phone and confirm there. Known paired devices reconnect without re-entering the code. Pairing is closed by default and expires after two minutes.
4. Rest the phone still for initial gyro calibration, then enter headset view. The headset view displays a hold-still instruction when calibration is required. A successfully measured bias is retained in fixed sensor coordinates across fullscreen surface recreation, display rotation and reconnects within the same app process. Explicit gyro calibration clears it; app restart starts fresh. Bias is not learned during slow intentional turns.

The settings editor uses the same schema and validates a candidate before asking the driver to save it. Enter JSON values, including quotes for strings, e.g. `"fast"`, `false`, or `60`. Edits are not overwritten by periodic polling. Bitrate/adaptive/display/tracking updates have live paths; transport mode, codec, FPS, slices and other unfinished reconfiguration paths must not be assumed to take effect mid-session. Full settings pages and all live-reconfiguration behavior remain future work.

Diagnostics ZIP export freezes status before opening the Save dialog and bundles driver/network/settings JSON, a manifest and filtered recent tails of known PocketVR logs. It excludes pairing state and identity stores, is bounded to 8 MiB, and works offline. Review addresses/settings/logs before sharing. See [DIAGNOSTICS_EXPORT.md](DIAGNOSTICS_EXPORT.md) for filters, limits and failed-save behavior. Logs are local to `%LOCALAPPDATA%\PocketVR\logs`.

Choose **USB setup** to find adb, select a connected phone, inspect authorization and set up/remove matching reverse rules. This helper works even while SteamVR is stopped. See [USB_WIZARD.md](USB_WIZARD.md). Choose **Benchmark** for a hardware-encoder preset comparison and local JSON report; see [ENCODER_BENCHMARK_UI.md](ENCODER_BENCHMARK_UI.md). QR and richer settings pages remain unfinished. A [USB setup script and phone cable mode](USB.md) is also available. Normal streaming still runs inside the SteamVR driver; closing the desktop utility does not stop it.

## Repeatable opt-in hardware diagnostic

Current debug/test APKs, an authorized adb connection, an unlocked phone on the same LAN and a running registered driver are required. This test automatically confirms the test phone's matching code via the current-user pipe and creates real pairing pins. It refuses to interrupt an existing phone session, closes the pairing window afterward and exports measurements without identities/discovery records:

`python scripts/test_live_session.py --server <PC-private-IPv4> --output D:\PocketVR-builds\live-session.json`

The test starts the normal phone Activity, connects, waits for calibrated tracking/video, enters fullscreen, verifies calibration reuse, runs for another 20 seconds and checks decoded/rendered frames and pose delivery. It saves an optional phone screenshot of the fullscreen content. It is deliberately excluded from normal CI/native instrumentation runs.

For original, unmistakable stereo content, run `pocketvr_stereo_scene.exe --seconds 90` before the check. The diagnostic submits matching red-over-green images with white crosshairs, an upward arrow and UP lettering in both eyes through the actual OpenVR scene API. It needs no controllers. This also distinguishes the submitted-scene path from SteamVR's system/startup background. Older captures used yellow/blue for the right eye; that colour difference was intentional.

## Observed hardware evidence (2026-11-02)

SteamVR 2.18.4 loaded PocketVR as its HMD on RX590 GME. Mutual pairing, actual 2160x1080 H.264 delivery, Qualcomm hardware decoding, phone IMU pose delivery, fullscreen reconnect and local diagnostics were exercised over Wi-Fi. The explicit stereo scene was visually verified in both eyes after lens correction. The fullscreen run reported 968 decoded/latched frames, about 414 Hz sensor updates, reused calibration, two missed-vsync intervals, sampled display FPS about 59.8 and decoder p50/p95 about 12.1/15.3 ms. This is a short diagnostic with startup/reconnect time, not sustained 60 fps, a measured end-to-end latency, optical acceptance or the required 30-minute gameplay test. Initial SteamVR system-background capture had a black left eye; that was not reproduced with the submitted diagnostic scene and remains a system-content acceptance item.

All 78 Windows CTest checks and license audit pass. Android APK builds, dependency/notices/alignment audit, actual Pixel shared/native/GPU/calibration checks and opt-in fullscreen stereo check pass. AMF initialization still falls back, and decode p95 exceeds the 10 ms target. Performance, controllers, USB and physical headset acceptance remain unfinished.

Connection QR now opens a selectable actual-LAN-listener code; phone scanning fills the address after review and preserves normal pairing. Cardboard viewer import is also available on the phone. See `QR_SETUP.md` for supported formats, measurements and verification limits.

**Dashboard** starts the new OpenVR overlay application. See `DASHBOARD.md` for registration/autolaunch, desktop preview, settings behavior and the pending home-PC acceptance checks.
