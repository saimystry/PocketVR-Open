# PocketVR specification

The single source of truth for what PocketVR is, what it does today and what is left. Updated 2026-11-05 at commit `399665e`, plus the documentation cleanup that added this file and the session replacement fix that followed it.

`docs/DECISIONS.md` holds the reasoning behind choices. Feature documents written before 2026-11-05 can still describe pairing codes or the USB setup window; this file wins wherever they disagree.

## 1. Product

PocketVR is a free, clean-room SteamVR driver and Android app. It streams SteamVR games from a Windows PC to a phone in a Cardboard-style headset, with no time limits, watermarks, licensing checks or telemetry.

**Owner goal:** play War Thunder in VR, better than the existing fallback of ALVR v20.8.0 with PhoneVR v2.0.0-beta over USB, whose weaknesses are a blocky sky and latency. If PocketVR cannot beat it after tuning, record why and decide whether to continue.

**Principles, in order:**

1. Simple and to the point. A user runs one installer, taps Connect and plays.
2. Stable. A session must survive a real evening of play.
3. Lowest practical latency with useful image quality.

### Target hardware

PocketVR must work on any PC that can run SteamVR, on AMD, NVIDIA and Intel graphics, not only the owner's. The list below is what it is tested against, not a limit.

| Item | Detail |
|---|---|
| Game PC (owner's home) | Ryzen 5 5600, RX 590 GME 8 GB, 16 GB single-channel DDR4-3200, Windows, SteamVR |
| Main phone | Pixel 4a 4G, Snapdragon 730G, 2340x1080 OLED, fixed 60 Hz, Android 11 |
| Second phone | Galaxy S22, 2340x1080 |
| Controls | Logitech Extreme 3D Pro joystick, with planned DIY Arduino throttle and pedals. These are ordinary PC game controllers, so War Thunder needs no VR controllers. An Xbox 360 pad and mouse are also supported. |
| Headset | None measured yet. Shinecon SC-G13, G12 and G10 are candidates, not validated profiles. |
| Build PC | i5-2400 with Intel HD graphics and no DirectX 11. It builds everything but cannot run SteamVR. |

### Owner decisions

- **No pairing code.** While the PC app is open, the first phone is trusted and remembered; with the app closed it needs the same one click as any new phone (changed 2026-11-05). A phone on the USB cable is always accepted. Any other new phone needs one click on Allow in the PC app; it is refused after 60 seconds. The signed handshake, per-session keys and encrypted media stay.
- **USB is on by default with no setup window.** Android still requires USB debugging to be enabled once by hand.
- **Audio stays on the PC.** Phone audio streaming is out of scope.
- **No 6DoF / ARCore.**
- **60 Hz only.** 90/120 Hz is out of scope for now.
- **Second-phone controller mode is deferred.**
- **Installer:** one unsigned setup `.exe`, no certificate purchase. The owner should never handle adb manually.
- **Desktop UI:** WebView2. **Phone UI:** native Android views with no new dependencies.

## 2. Rules for contributors

- **Clean room:** never decompile or inspect iVRy or other proprietary VR implementations. iVRy features come from public product pages only. ALVR (MIT) and Cardboard (Apache-2.0) may be read as architecture references.
- **Licensing:** Apache-2.0. Shipped dependencies must be permissive (MIT, BSD, Apache-2.0, zlib); no GPL, AGPL or LGPL. Keep notices and list components in `THIRD_PARTY_NOTICES.md`. `scripts/license_audit.py` enforces this.
- **Secrets:** never commit credentials, signing keys, pairing identities, `local.properties`, APKs or build output.
- **Honesty:** label claims as built, unit-tested, loopback-tested, device-tested or human-verified. Never report a hardware or gameplay result that did not happen, and never invent measurements. Report numbers with build type, phone and date.
- **Phone data:** do not uninstall the owner's app without asking; that erases lens settings.
- **Keep this file current** and record architecture choices in `docs/DECISIONS.md`.

Code conventions and build commands are in `AGENTS.md` and `docs/DEVELOPMENT.md`.

## 3. Architecture

```
Phone                                              PC (Windows)
IMU -> sensor fusion -> pose ------ UDP/TCP ---->  SteamVR driver: HMD and two controllers
gamepad, touch, volume keys -------------------->  pose history, prediction, vsync timing
                                                   SteamVR compositor (direct mode)
                                                   D3D11 layer composite, side by side
                                                   Hardware encoder, H.264 or HEVC: AMF, NVENC or Media Foundation
MediaCodec <- reassembly + FEC <-- UDP/TCP ------  packetizer and encryption
  -> SurfaceTexture -> GLES3 late rotation, lens correction, vignette -> display
```

Design rules:

- One video stream with both eyes side by side: one encoder, one decoder.
- Lens distortion and late rotational reprojection run on the phone. The driver reports an undistorted display and tags each frame with its render pose.
- Video is never retransmitted. Loss is concealed with FEC and on-demand IDR frames.
- Timestamps are in the PC clock domain with a continuously estimated phone offset.
- Android `monotonic_ns()` is CLOCK_BOOTTIME, while Choreographer uses CLOCK_MONOTONIC. Never compare them directly.

### Code map

| Path | Purpose |
|---|---|
| `pc/common` | Protocol, crypto, FEC, tracking, timing, adaptation and reassembly, shared with Android |
| `pc/driver` | OpenVR provider, D3D11 compositor, encoders, network server |
| `pc/control` | Desktop app (`main.cpp`, `ui/`), input helper, windowless USB helper, benchmark |
| `pc/overlay` | ImGui panel inside the SteamVR dashboard |
| `pc/tools` | Loopback client, network impairment proxy, encoder benchmark, diagnostics, test scene |
| `pc/installer` | Inno Setup script and registration and firewall helpers |
| `android` | Kotlin UI and lifecycle, C++ real-time client |
| `proto`, `third_party`, `scripts`, `docs` | Wire definitions, dependency locks and licenses, tooling, feature notes |

### Protocol and network

- TCP control on port 9943 and media on port 9944 (UDP, or TCP over adb). Details are in `docs/PROTOCOL.md`.
- Handshake: signed identities, X25519, ChaCha20-Poly1305 with replay protection. Identities use DPAPI on the PC and Android Keystore on the phone.
- The driver listens only on private, link-local and loopback addresses.
- The newest accepted phone replaces an existing session, so a phone that lost its link is not refused. A new connection authenticates beside the live session and replaces it only after its handshake and capabilities are accepted; silent, malformed or unsupported connections leave the session running.
- Automatic route order on the phone: USB tethering (UDP), adb reverse (TCP), then Wi-Fi.
- Discovery uses mDNS and UDP, plus a QR code and manual IP as fallbacks.
- The phone app and PC driver must match. `hello.protocol` is 2, and the PC tells an older phone app to update.

## 4. What exists today

Status labels: **device** means run on the real Pixel; **tested** means automated tests on Windows; **live** means seen working in a short real SteamVR run before 2026-11-05; **unproven** means written but never run in a live session.

### Streaming and stability

| Feature | Status |
|---|---|
| SteamVR driver, direct mode, layer compositing | live |
| AMF encoder on the RX 590 using AMD's Polaris workaround; Media Foundation fallback | live |
| Direct NVENC encoder on NVIDIA, falling back to Media Foundation | compiled; never run on an NVIDIA card |
| Media Foundation hardware encoder for Intel and any other vendor | tested in software form; not run on those cards |
| H.264 and HEVC selection | live for H.264; HEVC on device is unmeasured. `codec=auto` currently means H.264. |
| Hardware decoder, stereo renderer, lens mesh, late reprojection | device, live |
| Wi-Fi UDP, USB tethering UDP, adb TCP, automatic selection | live for short runs; recovery matrix open |
| FEC, adaptive bitrate, thermal and encoder guard | tested, partly live |
| Resolution/FPS switching with rollback | tested, unproven on hardware |
| Stricter switching: needs a stage above one frame for 5 s or sustained frame loss, predicts cost before stepping up, backs off 1 then 2 minutes, then stops | tested, unproven |
| Encoder resets counted over 60 s rather than per session | tested, unproven |
| Decoder-only restart, up to 3 in 30 s | built, unproven |
| Frame pacing: phone reports arrival lead, PC corrects phase and drift at up to 0.1 ms per frame | tested against simulated drift, unproven |
| Late frames are decoded instead of dropped; only frames over 250 ms old wait for a keyframe, requested at most every 0.5 s | built, unproven on hardware |
| Wi-Fi smoothing: each frame's packets spread over 60% of the frame interval by default, off on adb and USB tethering | timing tested, unproven on hardware |
| Per-stage latency: PC and phone timings joined per frame, live in the PC app, optional `latency.csv`, `scripts/latency_analyzer.py` | tested with synthetic timings; no real numbers yet |
| Test picture injected after the compositor, through the real encoder | built, unproven on hardware |
| GPU-loss detection with one SteamVR restart request | tested; no real GPU reset induced |

### Apps and setup

| Feature | Status |
|---|---|
| Desktop app: Home, Phone, Settings, Help; every runtime setting under Advanced | built; Home screen seen on Windows; other pages and driver-connected states unproven |
| Phone app: status card, Connect, Enter VR, Settings | device: Home, Settings and failed-connection screens |
| Connection without a code | tested on the driver; not yet run phone-to-PC |
| Windowless USB helper started by the driver | built, unproven |
| Install phone app from the PC through bundled adb | built, unproven |
| Connection QR, Cardboard viewer QR, manual lens settings | device partly; camera scan unproven |
| Views: headset, fit to screen, no headset | built, unproven visually |
| Volume button toggle, restore defaults, restart SteamVR | built, unproven |
| SteamVR dashboard panel | tested with a fake driver; never run in live SteamVR |
| Support ZIP, logs, encoded frame dump | tested |
| Installer: per-user Inno Setup, driver and dashboard registration, firewall rules, WebView2 download if missing, bundled adb and phone app | run on the build PC on 2026-11-05: install, uninstall and reinstall work, including driver and firewall removal, with all 112 files matching the manifest. The bundled adb server used to lock the folder and break both; setup now stops PocketVR's own copy. Not yet run on the RX 590 PC. |
| Graphics check: desktop app says when a PC has no DirectX 11 or SteamVR runs without PocketVR, and the driver log records the real failure | seen failing as expected on the build PC's driver log; desktop message built |
| Eye size: picture scaled about each lens, default 135%, set from the phone preview, PC app or dashboard, kept in step both ways | device: scaling and offline preview seen on the Pixel; PC-to-phone sync unproven |
| Sliders for every ranged setting in the PC app; sliders for lens settings on the phone | device for the phone; PC interface exercised with simulated state, not a live driver |
| Manual stream resolution, falling back to full size if the phone cannot decode it | tested on the driver, unproven on hardware |
| Profiles: built-in War Thunder, Lowest delay and Best picture, plus saved ones. Built-in values are starting points, not tuned results. | tested on the driver |
| One-click Allow for a new phone; HEVC falls back to H.264 on a phone without it | tested with `trust_loopback` off |
| Clearer phone error messages and Share logs | built; a PC whose driver failed still cannot tell the phone why |
| Phone Picture quality page: resolution, antialiasing (supersampling), bitrate, frame rate, video format, edge darkening | built, needs a live PC |

### Input and tracking

| Feature | Status |
|---|---|
| 3DoF IMU tracking, calibration, recenter, slow auto-center | live |
| Gyro drift follows quiet periods during play; slow deliberate turns are not learned | tested with simulated sensors |
| Each phone pose goes to SteamVR as it arrives, not once per `RunFrame` | built, unproven |
| In-headset menu: Back + Start on an Xbox pad, or Ctrl+Alt+M, then D-pad or Ctrl+Alt+arrows. Drawn by the phone. | menu logic tested; never seen in a headset |
| Two virtual Vive-style controllers and default bindings | tested; live bindings unproven |
| Xbox/XInput and mouse helper, rumble | partly hardware-checked; game use unproven |
| Phone gamepad, touch, volume input, mapping editor | built, phone routing unproven |
| External tracker bridge | tested with real OpenVR; no physical tracker |

### Verification at this commit

- Windows Debug: 217 of 217 tests pass, license audit passes, after the latency, FEC and approval changes of 2026-11-05. Release not rerun. These changes have not run on a phone or an AMD card.
- Pixel 4a instrumentation: `native_failures=0`, with renderer, viewer, QR, secure store, sensor direction, transport and handle checks passing.
- Release package audit: 112 files. The phone app is signed with the shared development key.
- Not done: no live SteamVR session has run since these changes.

### Last measurements

These are old, short runs, not Release-tuned or sustained:

| Measure | Result | Target |
|---|---|---|
| AMF encode p95 at 2160x1080 | 12.4-13.6 ms | under 8 ms |
| Pixel decode p95 | about 14-23.5 ms across runs | under 10 ms |

## 5. iVRy feature comparison

From public product pages only.

| iVRy feature | PocketVR |
|---|---|
| Phone as SteamVR HMD, no server app needed | Yes |
| USB and Wi-Fi | Yes |
| Automatic quality; user quality options | Yes |
| Configurable vignette | Yes |
| Cardboard QR lens profile and FOV | Yes |
| Settings in the SteamVR dashboard | Yes, unproven live |
| H.264 and H.265 | Yes |
| Automatic resolution | Yes |
| Monoscopic view without a headset, fit to screen | Yes, unproven visually |
| Disable volume buttons | Yes |
| Restore defaults, restart SteamVR | Yes |
| Touchscreen click in VR menus | Partly: touch zones press controller buttons and controllers follow the head; unproven |
| Oculus titles through Revive | Documented |
| Motion controllers | Gamepads and tracker bridge; presets for cheap Bluetooth remotes are left |
| Audio streaming, 6DoF, 90/120 Hz | Out of scope by owner decision |
| Direct/virtual driver modes | Direct only; low value |
| iPhone, Vision Pro, PSVR, Quest, Pico | Out of scope |

## 6. Settings

`config/pocketvr.json` is the single source, mirrored by `settings.schema.json` and generated code. Reserved and not editable: `connection.mode` and `input.recenter_hotkey`.

| Group | Settings |
|---|---|
| Video | codec, preset, fps (30/45/60), stream resolution, render scale (shown as antialiasing), bitrate and limits, adaptive on/off, slices |
| Network | FEC adaptive and ratio, ports, media encryption |
| Display | IPD, FOV scale, eye size, vignette strength/size/softness/dynamic, chromatic correction, edge mode, stats overlay |
| Lens | profile, k1, k2, distortion tweak, screen and lens distances, vertical offset, FOV limits |
| Tracking | fusion mode, rate, yaw correction, prediction, eye height, reprojection |
| Pacing | target lead (default 4 ms) |
| Input | mapping preset and 16 button bindings |
| Power and debug | keep awake, thermal guard, log level, frame dump |

Phone-only preferences: view mode, volume buttons, performance overlay and the experimental faster display.

## 7. What is left, by priority

### P0: prove it on the RX 590 (owner runs; needs no new code unless something fails)

1. Run the installer on the RX 590 PC: install, first launch, firewall prompt, driver registration. Then test upgrade and uninstall. The build PC cannot do this step: its Intel HD graphics stop at feature level 10_1, so SteamVR refuses the driver with `VRInitError_Driver_Failed`.
2. Uninstall the old phone app once, because the signing key changed, then use **Install on phone**. Saved lens settings are lost.
3. First phone-to-PC connection with no code, on USB and on Wi-Fi.
4. **Black flash check:** 10 minutes on USB and 10 on Wi-Fi with no flash or reconnect. If it persists, collect the support ZIP and `adb logcat -s PocketVR`. The session end reason is now logged on both sides. Three suspects were fixed: format switching, encoder resets and decoder errors.
5. Confirm frame pacing converges. `session.adaptation.pacing` reports arrival lead and phase; compare judder with the old fixed timer.
6. Desktop app states with a live driver, which have never been seen.

### P1: make it good enough to replace ALVR

1. **Latency tuning.** Re-measure in Release. Tune AMF (preset, slices, rate control) and the decoder.
2. **Per-stage latency:** built. Turn on "Record delay to a file" and run `scripts/latency_analyzer.py` on `latency.csv` before tuning.
3. **Image quality (blocky sky):** the test picture, bits per frame and a War Thunder profile are built. AMF now asks for VBAQ, a QP limit of 42 and explicit BT.709 colour; the log line `AMF quality options accepted` shows which ones the RX 590 took. None is measured yet. Compare H.264 with HEVC on hardware and tune that profile. USB still starts at 60 Mbps; the original plan was 80 Mbps. Automatic FEC now starts at 10% on Wi-Fi and 0% on USB, and rises if frames are lost.
4. **Recovery matrix on hardware:** cable pull and replug, tethering enabled late, Wi-Fi loss, sleep and resume, app switch, SteamVR restart, multiple or unauthorized phones.
5. **Send pacing:** built. Measure the delay it adds against the loss it saves, and tune the 60% default.
6. **Measure the real headset** and save a lens profile.
7. **30-minute War Thunder session over USB,** then fill in the checklist in section 9.

### P2: comfort and correctness

1. Check gyro drift tracking on a real head: tremor may keep quiet windows from being accepted.
2. Magnetometer hard and soft-iron calibration.
3. Confirm the pose pump helps on hardware.
4. Replace the fixed `SecondsFromVsyncToPhotons` (0.011) with a measured value.
5. GPU overload guard: `gpu_ms` is checked but nothing supplies it, and `render_scale` never adapts.
6. Run on an NVIDIA card and on Intel Arc or newer to prove those encoders.
7. Encoder restarts run while `adaptive_mutex_` is held, which can stall status requests.
8. Remaining review items: TCP split reads, clocks, concurrency, lifecycle, CG-NAT and IPv6 policy, effective socket buffers.
9. gcc `-Werror` failures that MSVC and clang miss: `transport.cpp:134` and `common_tests.cpp:1030`. Formatting drift against `.clang-format`.

### P3: remaining features

1. Automatic H.264/HEVC benchmark and selection using phone decode data.
2. Controller remapping on the PC and in the dashboard, generic trigger calibration, presets for cheap Bluetooth VR remotes.
3. Tap-to-click gaze pointer for SteamVR menus, if touch zones prove insufficient.
4. Configurable recenter hotkey (fixed at Ctrl+Alt+R today) and desktop `connection.mode`, both reserved.
5. Presets for gamepads attached to the phone in the headset menu, which only reads PC pads today.
6. Optional explicit Cisco OpenH264 download.
7. Galaxy S22 pass: check Pixel assumptions in sensors and decoder limits. Stay at 60 Hz.
8. Big-core thread affinity on the phone, from the original design.

### P4: housekeeping and release

1. Update feature docs that still describe pairing or the USB wizard: `AUTHENTICATION`, `USB_WIZARD`, `DESKTOP_SETUP`, `QR_SETUP`, `DASHBOARD`, `USB`, `INSTALLER`.
2. Review the terms for bundling adb before any public sharing.
3. Installer code signing and a proper release key, if the project is ever published.
4. Run CI once; it is configured but manual-only and never executed remotely.
5. Decide whether the shipped benchmark and tools still belong in the installer.

## 8. Acceptance targets

Measure and report; these are goals, not guarantees.

- 60 fps sustained, with zero dropped vsyncs on the phone in steady state.
- Rotational latency under 25 ms on all transports.
- Video latency p50 under 70 ms on USB and under 90 ms on clean 5 GHz Wi-Fi.
- Encode p95 under 8 ms at 2160x1080, or the documented best on the RX 590.
- Decode p95 under 10 ms on the Pixel 4a.
- Graceful degradation at 1%, 3% and 8% loss under `netsim`: no freezes and no backlog.

**Playable v1:** a 30-minute War Thunder session over USB on the Pixel 4a with no disconnects and no blocking comfort issue, at least as good as the ALVR fallback in the owner's judgment.

**Release:** recovery matrix passes, installer upgrade and uninstall pass, Galaxy S22 pass done, feature docs current.

## 9. Owner checklist

Fill this in after each hardware milestone, for each phone.

| Check | Result | Notes |
|---|---|---|
| Sharp enough to spot a plane at distance | | |
| Sky banding or blockiness compared with ALVR | | |
| Head turns feel responsive | | |
| Judder or double images | | |
| No black flash in 20 minutes | | |
| Lens alignment and IPD comfortable | | |
| Nausea after 15 minutes | | |
| Phone heat after 30 minutes | | |
| Joystick flight controls work alongside VR | | |

## 10. Build and test

Tool setup is in `docs/DEVELOPMENT.md`. Release signing needs your own key, which never goes in this repository.

```powershell
pwsh -File scripts/build.ps1 -Configuration Debug        # build, CTest, license audit
pwsh -File scripts/build_android.ps1 -Configuration Release -SkipDependencyInstall -GradleExecutable <gradle.bat>
pwsh -File scripts/build_installer.ps1 -BuildDirectory <windows-release> -Iscc <ISCC.exe> -OutputDirectory <out> -PhoneApk <app-release.apk>
```

Notes learned the hard way:

- Android native libraries took too long to rebuild here because an MSYS2 mirror stalled. Reusing `android-vcpkg-installed` from an earlier build worked with `-SkipDependencyInstall`.
- The Android CLI uses slash package names such as `ndk/26.3.11579264`.
- For on-device tests without touching the installed app, build with `-PqrVerification=true -PnativeTests=true`. This installs `com.pocketvr.qrcheck` beside the real app. Run `am instrument -w -r com.pocketvr.qrcheck.test/com.pocketvr.test.NativeInstrumentation`.
- Close SteamVR before replacing a loaded driver DLL.
- Keep the discovery race guards, identity pin checks and same-SurfaceView headset transition when refactoring. Each fixed a real bug.
- Do not flip sensor signs based on how pixels move on a flat screen; the mapping is verified against Android's own remapping.
