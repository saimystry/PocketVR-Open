# SteamVR dashboard

`pocketvr_overlay.exe` is a separate OpenVR overlay application. The desktop
setup app's **Dashboard** button launches/shows it. It creates a PocketVR tab in the
SteamVR dashboard; open SteamVR's dashboard and select that tab. Starting the
process normally does not force the dashboard over a running game. The explicit
`--show` option, used by the desktop button, opens/selects its tab. If an instance
already owns the overlay key, the new process shows that instance and exits
without destroying its resources.

Build the Windows targets together. Development output is `pc/overlay` in the
external build tree; the staged package contains `pocketvr/overlay` with the
executable, matching `openvr_api.dll` and `app.vrmanifest`. Keep the package's
LICENSE/NOTICE and third-party notices. Do not copy only the executable.

## Starting and registration

Run these commands from the overlay's final installation directory:

```powershell
.\pocketvr_overlay.exe --register
.\pocketvr_overlay.exe
```

`--register` uses OpenVR's application registry to add the adjacent manifest and
explicitly enable application autolaunch. Subsequent SteamVR starts can launch
it automatically. Normal launches do not overwrite the user's autolaunch choice.
Register only after choosing a permanent folder: moving it breaks the registered
path. `--unregister` disables autolaunch and removes that manifest; it does not
terminate an already running instance. The [installer](INSTALLER.md) integrates
these steps. Registration has not been exercised on the build PC,
which has no SteamVR.

Installer commands add `--quiet` to registration/unregistration to return errors
without a modal dialog. Absent applications unregister idempotently; a conflicting
registered executable is rejected during installation and preserved during removal.
Failed fresh autolaunch setup removes its new
manifest; uncertain rollback returns code 2 so setup retains files for repair.

The executable also supports:

- `--show`: explicitly open/select the dashboard tab, reusing an existing instance.
- `--preview`: an interactive desktop version of the same panel, without
  initializing SteamVR. It reads the real driver if available, otherwise shows
  its offline state. Close the window to exit. This mode is not a VR performance
  or headset test.
- `--probe-runtime`: prints whether the OpenVR runtime is installed without
  starting a scene or registering the application.
- `--self-test DIRECTORY`: a test-only WARP renderer with explicitly labeled
  synthetic data and PNG captures. It never sends settings or pairing commands.
- `--preview-smoke-test`: a hidden, self-closing Win32/D3D11 startup check.

The build PC runtime probe reports `runtime_installed:false`. No Steam/SteamVR was
installed and no live VR/gameplay test was launched for this milestone.

## Pages

**Status** shows driver/session state, encoder backend and accepted bitrate
setting, transport, and fresh phone diagnostics. Four bounded histories show
sampled sent-byte rate, phone decode p95, phone video FPS and PC frame-drop
fraction. Sent-byte rate is a counter derivative, not the encoder bitrate
setting. PC frame drops are not network loss. Decode CPU/display diagnostics
are not end-to-end or scan-out latency. Missing data is labeled unavailable;
unsupported encode/one-way latency/network-loss measurements are not invented.
Histories reset after disconnects, counter regressions or a long-hidden panel.

**Video**, **Display / lens** and **Tracking** edit the same driver-owned settings
as the desktop app. Sliders/combos work without a VR keyboard. Each field has a
draft, explicit Apply and Reset; polling does not overwrite a draft. Validation
uses the shared settings catalog, and an applied value is confirmed by the
driver echo. Codec/preset/FPS/scale/FEC changes require a new phone connection;
runtime bitrate changes depend on encoder support. IPD, lens spacing and lens
distance remain distinct. Select manual calibration to apply physical lens
measurements; import QR profiles on the phone. A saved phone profile can override
PC calibration on connection. Recenter sends the existing driver recenter RPC.

**Input** explains the current foreground mouse/XInput/joystick helper and
exposes the phone stats-overlay selector. Editable mappings, phone controllers,
haptics and other reserved runtime settings remain disabled and are not claimed
as implemented. Keep the desktop input helper foreground when using mouse gaze
selection; actual SteamVR pointer acceptance remains a home-PC check.

**Pairing** provides open/close controls, a six-digit keypad, code confirmation,
paired-public-identity selection and a confirmation before forgetting a phone.
It retains mutual confirmation on the phone. The adjacent LAN QR uses actual
private driver listeners and the existing nonsecret connection format. It never
grants trust or bypasses pairing. USB/adb setup stays in desktop setup.

**About / logs** refreshes bounded, filtered tails and exports a local diagnostics
ZIP on the worker, including when the driver is unavailable. Exports go to
`%LOCALAPPDATA%/PocketVR/diagnostics`; folder buttons open desktop windows.
Nothing is uploaded. Read `DIAGNOSTICS_EXPORT.md` before sharing a bundle. The
collector includes current driver/control/input/overlay logs before old rotations,
with 512 KiB per-file and 6 MiB aggregate log limits within the existing 8 MiB ZIP
limit. This prevents full rotations from crowding out metadata or current logs.

## Rendering and failure behavior

The panel uses the already pinned MIT Dear ImGui with its D3D11 and Win32
backends, plus the pinned BSD OpenVR SDK. VR mode selects the GPU reported by
`IVRSystem::GetDXGIOutputInfo`, creates a persistent 1280 x 900 D3D11 render
texture and submits it through `IVROverlay`. The original thumbnail is generated
once. No screenshot/readback or UI work enters the driver encoder/pose callbacks.
PNG readback exists only in self-test mode. Visible UI draws at about 30 Hz;
hidden UI does no further drawing after its initial frame. This is a resource
budget, not a measured latency guarantee.

A separate cancellable worker polls the existing current-user-only named pipe
at roughly 2 Hz. Mutations have a 16-entry queue and expire after three seconds.
Offline/stale settings cannot be applied; failed mutations are never retried
automatically because a lost reply can follow a successful operation. Local
diagnostics remain available offline. Mouse coordinates are converted from
OpenVR's bottom-left origin; invalid positions cannot activate a stale hover.
Button releases/focus resets occur on focus loss, hiding and quit, with a
visibility transition fallback. Events and scroll deltas are bounded.

The main/thumbnail overlays and texture references are cleared before the D3D
resources and OpenVR runtime are released. There is no automatic scene launch,
forced dashboard opening, driver installation or firewall change in this step.

## Verification and remaining acceptance

Run CTest normally. New tests cover pointer conversion and malformed events,
focus/quit releases, bounded/reset histories, asynchronous real named-pipe
settings, rejected mutations without retries, cancellation, all-page WARP
rendering and actual hidden Win32/D3D11 preview startup. Diagnostics tests also
exercise full rotations including the overlay log within the archive budget.
Self-test PNGs are in the build tree's `test-artifacts/dashboard` and are not
repository files. Their numbers are synthetic, prominently labeled fixtures.

On the home SteamVR/RX 590 setup, still verify registration/autolaunch/unregister,
adapter selection and texture orientation, all tab/scroll/keypad interactions,
mouse/controller focus loss, settings/recenter effects, phone disconnect/reconnect
and dashboard GPU cost during gameplay. Host checks cannot establish
these runtime or headset acceptance results.

Verified 2026-11-03: the full 128-check Windows suite has 127 passes and one
pre-existing SMB diagnostics skip. The final launcher change passes the eight
applicable dashboard/control startup checks. Installed dependency/SDK license
audit and package comparisons pass. All page captures, including the manual
prediction row, lower lens controls and side-by-side keypad/QR, were inspected.
Build and evidence stay under the approved external build root; build logs are
`P:/pocketvr-builds/dashboard-final-build.log`, `dashboard-ctest-final.log` and
`dashboard-launcher-checks.log`. These results do not establish live SteamVR,
registration/autolaunch, controller-pointer or headset performance acceptance.
