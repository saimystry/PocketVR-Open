# QR setup

The desktop **Connection QR** button and phone **Scan PC connection QR** replace
manual LAN address entry. They do not replace authenticated pairing. Start the
driver, choose its private LAN interface in the QR window, scan on the phone,
review the address, then tap Connect. Open pairing on the PC and compare/confirm
the six-digit code on both devices as usual. Media ports are negotiated by the
existing authenticated protocol. USB/adb uses its separate setup flow.

Connection payload: `pocketvr://connect?v=1&ip=192.168.42.7&port=12345`.
Only canonical RFC1918 IPv4 addresses and valid control ports are accepted.
Unknown/duplicate query parameters, other versions and fragments fail. No
identity seed, trust pin, password or pairing code is encoded. Scanning never
connects automatically or changes trusted peers. The displayed endpoints are a
snapshot; reopen the window after changing networks or ports. The current
driver must be running to supply its actual listeners; the build PC has no
SteamVR and can only exercise synthetic fixtures here.

**Scan Cardboard viewer QR** reads full `http[s]://[www.]google.com/cardboard/cfg?p=…`
codes offline. The URL-safe base64 protobuf is bounded and validated using the
pinned upstream DeviceParams schema. Short/redirect links are rejected without
opening a browser or making a web request. Paste the full decoded QR text on the
scanner screen if camera access is unavailable. Camera permission is requested
only on entering that screen; scanning stops an existing stream and does not
resume it automatically. A camera-release gate prevents Connect until the setup
camera has closed. No camera or QR decoding runs in video/IMU callbacks. This
does not add ARCore or positional tracking.

The import review shows the vendor/model and lens measurements, and asks for
active screen width/height and tray-edge bezel in millimeters. Existing screen
measurements are offered as starting values, not certified measurements of the
phone. Confirm them physically. Your IPD remains separate and is preserved.
Review Cancel leaves saved calibration unchanged; Save goes through the same
native validation, persistence and PC projection synchronization as manual
calibration. Profiles with impossible geometry never replace saved values.

Supported optical data: lens-to-screen distance, lens-center spacing, zero/one/
two nonnegative radial coefficients within existing bounds, asymmetric left-eye
FOV limits (mirrored horizontally for the right eye), and bottom/center/top tray
alignment. Longer/negative distortion polynomials are explicitly unsupported,
rather than truncated. For bottom alignment, lens height above active-screen
bottom is `tray_to_lens - bottom_bezel`. For top alignment it is
`screen_height - tray_to_lens + top_bezel`; this physically mirrored relation is
intentional. Center alignment ignores tray/bezel values. Native FOV is limited
by both the visible screen and the profile's aperture; the existing render guard
continues outside that visible aperture for rotational reprojection.

Canonical saved geometry retains the existing `manual_viewer` preference key and
wire format with five additional lens fields: `vertical_offset_m` (above screen
center), and `fov_left_deg`, `fov_right_deg`, `fov_bottom_deg`, `fov_top_deg`.
Older saved profiles/settings migrate to centered lenses and 89-degree limits;
other missing/unknown fields still fail. Rebuild matching PC and phone versions
together. The manual editor exposes these fields; **Generic starting values**
clears imported optical limits when saved. The QR vendor/model label itself is
informational, not a calibrated preset or a stored trust assertion.

Dependencies: unmodified pinned MIT Nayuki C++ generator on Windows, pure Java
Apache-2.0 ZXing core 3.5.3 on Android, and the existing Apache-2.0 Cardboard
DeviceParams schema. The full Cardboard/Unity runtime is not shipped. PocketVR's
bounded independent wire reader and existing fixed-screen lens mesh keep the
setup dependency out of the real-time path. Vendored source hashes, Maven
JAR/POM/source hashes, strict Gradle verification and packaged notices are audited.

Verification includes LAN-only payload checks, malformed/oversize/duplicate
codes, truncated protobuf fields, unsupported polynomial rejection, physical
top/bottom symmetry, old-profile migration, and desktop-matrix decoding by
ZXing on Android. The matrix fixture in Android test assets is exported by
`SetupQr.DesktopMatrixForIndependentAndroidDecoder` when the explicit
`POCKETVR_QR_FIXTURE` environment variable is set. It contains a synthetic private
address only. Run QR-only device checks with
`adb shell am instrument -w -r -e qr_only true com.pocketvr.test/com.pocketvr.test.NativeInstrumentation`.
Physical camera scanning, measured viewer optics and live SteamVR acceptance
must be recorded separately from parser/build tests.

## Testing beside an installed app

An app signed with a different key cannot be updated in place, and uninstalling it
erases its data. For isolated device verification, build with
`-PqrVerification=true -PnativeTests=true`. This affects Debug only: the app is
`com.pocketvr.qrcheck`, labeled **PocketVR QR check**, and its test package is
`com.pocketvr.qrcheck.test`. Default Debug/Release identity stays `com.pocketvr`.
Use `com.pocketvr.qrcheck.test/com.pocketvr.test.NativeInstrumentation` in the
QR-only command above for that build. Shared preferences/Keystore identities
are isolated by Android UID. This avoids transferring signing keys or erasing
home pairing/calibration. Return to the default build flags for the regular app.

## Verified checkpoint, 2026-11-03

Windows Debug: 119 passing CTests, one pre-existing diagnostics hard-link check
skipped on SMB (120 total), installed dependency/SDK audit passed, hidden QR
window startup passed and all 43 packaged legal files match their source.
Android Debug/instrumentation builds and runtime/notices/16 KB alignment audits
pass. Actual Pixel isolated-package QR-only instrumentation reports `qr_checks=1`,
`native_failures=0`, result `-1`; full regression also reports passing native
handle, secure store, renderer and viewer checks with zero native failures.
The original home app was not replaced or reset. The new isolated app is left
installed under its distinct label. Synthetic RGB and NV21 decoding of the
actual desktop matrix pass; camera capture/physical scanning and measured
optics remain unverified. The automated saved-viewer screenshot is black and
must not be used as visual acceptance evidence. No Steam/SteamVR or gameplay
performance run occurred on this build PC.

Evidence is: `P:/pocketvr-builds/qr-ctest-final.log`,
`qr-setup-final-build.log`, `qr-package-build.log`, `qr-android-isolated-final.log`,
`qr-pixel-isolated-checks.log` and `qr-pixel-full-regression.log`. These machine-
local logs/build outputs and phone screenshots are not repository content.
