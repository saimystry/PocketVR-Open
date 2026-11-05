# Design decisions

## Acknowledged in-session video format changes (2026-11-04)

Negotiate exact phone-advertised format steps under the startup ceiling. Pause
only video while replacing codecs and use prepare/commit acknowledgment plus a
PC render-time fence to reject delayed old-format UDP/TCP payloads. Retain sockets,
keys, poses, preferences and frame identities. Roll back through the same gated
path, disabling further automatic attempts after a failure. Scale encoder input
on the GPU rather than changing SteamVR's configured eye projections/render target.
Require persistent fresh pressure, settling and measured recovery headroom; the
thermal guard remains an independent stricter input-cadence/pause limit. See
`DYNAMIC_FORMAT.md` for protocol, policy, verification and hardware limits.
Probe the specified 0.85/0.7/0.55 size steps plus existing compatibility sizes;
only actual advertised support enters the ladder. Keep SteamVR's display-frequency
property at the physical panel refresh rate while encoded video cadence changes,
because the phone continues rendering/timewarping at that rate. Reporting 30/45 Hz
while emitting physical-display vsync events at 60 Hz would give inconsistent
timing; production vsync/PLL integration remains its own milestone. Recovery uses
fifteen clean seconds rather than the minimum ten to reduce codec restart churn.

## Bounded encoded diagnostics (2026-11-03 nightly continuation)

Dump Annex-B access units through preallocated bounded slots to an independent
file worker. Gate on a parameter-set-bearing keyframe, cap duration/bytes, stop
on gaps rather than append broken reference chains, and isolate all capture
errors from the live stream. Filenames are exclusive and output stays local.

## Per-user installation and scoped firewall (2026-11-03)

Install an external SteamVR driver at a permanent per-user PocketVR root instead
of writing inside the Steam installation. Register driver/dashboard as the actual
SteamVR user. Elevate only the firewall helper, carrying that user's SID explicitly
so another administrator's consent cannot change registration ownership. Use
runtime-path/SID-derived rule names, exact program and ports, Private profile,
LocalSubnet and blocked edge traversal; reject collisions/altered rules. Never
change a network category or remove foreign driver paths/rules. Detect running
processes and ask the user to close them, rather than terminating applications.

Validate Release inventory, DLL imports, hashes and exact legal assets before
packaging; Windows VC runtime remains an external prerequisite. Pin Inno compiler/
stub hashes and capture its permissive Inno/Pascal Script/public-domain LZMA
notices. Keep optional compiler extraction libraries out of the shipped package.
Rollback only newly created driver/rules/state; preserve upgrades, and retain files
for repair on timeout/unknown rollback. Uninstall stops on failed cleanup and
preserves external settings/pairing/logs. Host adapters plus compiled no-SteamVR
refusal do not establish positive runtime/firewall/uninstall acceptance.

## Desktop encoder comparison (2026-11-03)
Use a separate native benchmark window and sequential child CLI runs, keeping
comparison work outside the streaming process. Validate a versioned machine
result against the exact request and require hardware, exposing AMF/MF hardware
backend and GPU rather than silently timing software fallback. Preserve partial
results on cancellation/failure and never auto-apply settings. Label encoded
payload rate, serial wall FPS and optional PC software-decoded luma PSNR
separately from phone/VR metrics. Default comparisons use the same settings for
all three presets; actual RX 590 tuning remains a hardware acceptance task.

## Desktop adb setup (2026-11-03)
Use a standalone native USB wizard launched from desktop control, so adb setup
and authorization guidance work before SteamVR. Discover adb from explicit/SDK/
PATH locations and keep device operations on a cancellable worker. Launch adb
directly with bounded output and restricted inherited handles; never invoke a
shell or kill the shared server. Validate both tuples before mutation, create
with `--no-rebind`, verify results and roll back only confirmed new rules still
pointing at their expected destination. Surface partial/unknown outcomes because
adb offers no atomic two-rule transaction or conditional remove. Persist only a
browsed adb path; phone selection/pairing are not changed automatically. USB
tethering detection remains separate.

## Diagnostics bundles (2026-11-03)
Replace the desktop's JSON-only save with a bounded, worker-built ZIP containing
a frozen status/network/settings snapshot and filtered tails of allowlisted text
logs. Use first-party ZIP STORE serialization rather than adding an archiver
dependency or shell command. Exclude identity stores/pairing data, disclose
filtering and truncation, and atomically replace the selected destination. ZIP
interoperability is checked independently using Python's standard reader. The
format follows [PKWARE APPNOTE](https://pkware.cachefly.net/webdocs/casestudies/APPNOTE.TXT).

## Native tracking calibration and coordinates
Use zero-batch NDK sensor delivery on a dedicated worker, request the configured 100–500 Hz rate with hardware min-delay limits, and map Android's natural sensor axes through the current display rotation. The game-rotation mode also converts ENU world axes to +Y-up/-Z-forward. Startup/recalibration requires two seconds of stillness with low gyro variance and stable gravity; afterwards freeze gyro bias. Continuous bias learning cannot distinguish a slow intentional yaw from bias without an independent reference. Tests preserve a 0.01 rad/s turn after calibration. Optional slow auto-centering starts only after prolonged stillness and slews yaw by at most 0.002 rad/s. Use sensor event CLOCK_BOOTTIME timestamps, reject stale/non-finite samples, subtract gravity for linear acceleration, and publish a shared bounded-read atomic pose snapshot. Actual device rates and tracking behavior remain pending hardware.

## Decoder preview checkpoint
At the 50% checkpoint, decode authenticated/reassembled access units into an Android SurfaceView as an explicitly labelled diagnostic preview. This does not implement headset optics or timewarp. The reusable native decoder takes an ANativeWindow, so the later renderer can provide a SurfaceTexture-backed window. Use the hardware codec name advertised by MediaCodecList, retry configuration without optional vendor hints, preallocate three bounded access-unit slots, reject stale queued input, and wait for an IDR after a reference-chain gap. One worker polls input/output without blocking waits and releases only the newest fresh available decoded output to the preview; it avoids concurrent flush/reset races. This intentionally differs from the proposed two codec threads while retaining separate network/sensor/UI workers. IDR requests are limited to once per two seconds. Pause and surface replacement release/reconnect the session. Performance is unmeasured until a phone is connected.

## Lens mesh mapping correction
For a fixed grid of screen-plane tan-angle rays, map screen to source texture with forward radial distortion. Cardboard's equivalent texture-grid construction instead applies inverse distortion to vertex positions. The former code applied inverse distortion to source rays on a fixed screen grid, which reversed the mapping. A regression reference at screen radius 0.5 with k1=0.34/k2=0.55 yields texture radius 0.5596875. See the primary [Cardboard distortion mesh reference](https://chromium.googlesource.com/external/github.com/googlevr/cardboard/+/refs/tags/upstream/v1.5.0/sdk/distortion_mesh.cc). Physical screen/lens geometry, projection negotiation and optical acceptance still need integration.

## Android validation and storage
Add a manual-only Android hosted build/audit job and an opt-in self-hosted ARM64-device instrumentation job. Compiling the test APK never counts as executing its tests. The portable Android build helper keeps dependency build trees, Gradle caches and APKs under the chosen external build root. The UDP loopback correctness tool allows a longer synthetic reassembly deadline to tolerate network-share paging during concurrent development builds; production reassembly retains its 33 ms default.

## Pairing approval and Android cancellation
Use signed ephemeral X25519 transcripts with Ed25519 identity pins, commit both hellos before reveal, and require mutual approval for new identities. The PC accepts the phone's typed code, then the phone verifies the PC's matching approval screen before saving its PC pin. Control records use independent directional keys and strict counters. NDK r26 lacks stop_token/jthread headers, so shared control sessions use a small cooperative Worker/StopToken wrapper with bounded 20 ms cancellation checks; keep the installed NDK rather than downloading a second large toolchain. Android timestamps use CLOCK_BOOTTIME to match sensor elapsedRealtimeNanos; Windows keeps its steady/QPC clock.

## V1 and hardware validation
V1 includes 3DoF IMU tracking, desktop setup, and a dashboard overlay. ARCore, streamed audio, and NVENC are deferred. The development PC has Intel HD Graphics 530; RX 590/Pixel 4a performance and gameplay acceptance remain pending. Synthetic results will be labelled separately.

## Storage and build tools
Keep source and local Git history here. External tools, dependency caches, and build trees use only `<build root>` (also mapped as P: to avoid spaces in native Android paths). Windows Build Tools/SDK are local. Do not publish or push. Development tools are not redistributed product dependencies.

## License corrections
PocketVR uses Apache-2.0. The pinned libsodium release uses ISC, a permissive license; OpenVR uses BSD-3-Clause. Actual upstream licenses take precedence over the prompt's informal MIT labels. The audit explicitly allows ISC, rejects copyleft and unknown license expressions, and checks transitive packages. Media Foundation software H.264 is the last-resort built-in encoder; OpenH264 is an explicit optional download, never an automatic startup action.

## Shared language and coordinates
Use C++20 on Windows and Android, supported by the installed NDK, so the same checked serialization and algorithms compile unchanged. World coordinates are right-handed, +X right, +Y up, -Z forward. Unit quaternions store (x,y,z,w) and actively rotate local rays into world space. A current-eye ray samples a frame using inverse(q_render)*q_now; inverse(q_now)*q_render maps the old frame's geometry into the current eye. Tests will exercise both directions.

## Cadence
The physical display and SteamVR HMD frequency stay at 60 Hz. Adaptive video delivery may become 45/30 fps; the phone still reprojects at every physical vsync. Reporting a fake 30 Hz physical display would distort prediction and timing.

## Control framing and authentication
Use bounded, length-prefixed UTF-8 JSON for control messages inside an authenticated encrypted session. UDP headers are authenticated associated data. Pairing must bind both public keys, both nonces and the displayed code to the confirmed transcript; an unauthenticated X25519 exchange alone does not prevent an active intermediary. No pairing or unauthenticated network listener is enabled before the security path is tested.

## Initial viewer and tracker bridge
Until a viewer QR/manual calibration is provided, use a clearly identified generic Cardboard v2 profile rather than treating an absent QR scan as real lens calibration. Generate settings defaults and the SteamVR mirror from one checked schema. The external tracker bridge uses a local named pipe restricted to the current user's SID and explicitly rejects remote clients; there is no unauthenticated network JSON tracker endpoint.

Home-PC viewer amendment (2026-11-02): expose persistent manual calibration controls in the Android app for lens-to-screen distance and physical lens-center spacing in millimeters, with validation and matching PC projection updates. Keep lens spacing separate from user IPD where geometry requires it. Shinecon SC-G13/G12/G10 are candidate viewers only; their optics have not been measured. The existing screen-to-lens schema field is a starting point, not a completed calibration UI or renderer integration.

Manual calibration feature (2026-11-02): the phone owns one saved manual profile and reapplies it on each authenticated connection. Shared C++ schema validation and geometry calculations are used by both Android and PC; the UI reads bounds/defaults from that catalog. A `viewer.set` control message replaces the complete calibration atomically, and a request-correlated `viewer.result` confirms or rejects it. Physical lens spacing determines mirrored off-axis projections from a centered landscape screen; user IPD independently determines eye-to-head transforms. Radial coefficients map physical tangent extents to render tangent extents; FOV scale acts on angles. Generic profiles retain the previous symmetric projection until manual calibration is saved. Projection snapshots are immutable and revisioned, and frames carry their projection ID. This feature establishes settings and PC projection; optical correction, vertical lens alignment, QR profiles, and the dedicated phone renderer remain separate work.

## Direct-mode sharing and color
Use documented legacy D3D11 shared swap handles (`MISC_SHARED`, texture flags zero), with ownership controlled by the compositor-provided keyed sync texture. This keeps the documented single-sync-texture protocol instead of inventing an extra per-source keyed-mutex handoff for NT swap handles. Both NT and legacy sync handles can be opened and are cached. Legacy resource handles are not closed with CloseHandle. Composition records a deferred command list so encoder use of the device's immediate context cannot interleave shader state. Blend premultiplied layers in linear light into an sRGB RTV, then perform one GPU-only copy into an encoder-compatible typed BGRA texture; the extra copy preserves correct color without CPU readback. GPU/SteamVR interoperability and RX 590 costs remain hardware acceptance checks.

## Software encoder exception
The requested software fallback and an unconditional ban on raw-frame CPU readback cannot both be satisfied: the built-in Media Foundation software H.264 encoder consumes CPU NV12 buffers. Keep AMF and D3D11-aware hardware MFT paths GPU-only; permit staging readback and BT.709 limited-range CPU conversion solely on the explicitly reported software fallback worker. It never blocks GetPose/SubmitLayer. Benchmarks and quality-analysis tools may also read back synthetic/reference images. Record actual encoder backend so software timings are never presented as RX 590 hardware measurements.

## Encoder ownership and recovery
Hold each of the three compositor output leases until its matching encoded frame is polled. The Present callback only pushes a fixed-size record into an SPSC queue; codec setup, conversion, submit/poll and output delivery run on a dedicated worker. Drop stale frames, request a new IDR after output loss, and destroy a stalled encoder before returning its texture leases. Retry initialization after one second. A session supplies the bounded whole-frame output sink when connected; do not create an unauthenticated media destination simply to activate encoding.


## Phone texture ownership and headset diagnostics (2026-11-02)
Use a dedicated EGL/GLES thread and detached SurfaceTexture producer. Publish decoded render metadata before releasing MediaCodec output with an exact surface timestamp token; match the latched texture by token and session instead of pairing it with the newest header. Reuse held frames with bounded local pose prediction. Shared EGL-display lifetime is reference counted. MainActivity reparents the same SurfaceView for immersive landscape mode, retaining one lifecycle owner rather than adding an activity handoff. QR/vertical alignment and PC recenter/tracker origin remain unfinished.

The optional HUD uses a first-party 5x7 bitmap alphabet and fixed storage, uploaded at a half-second sampling cadence. Draw it in undistorted eye space so it is stereo, head locked and lens corrected. Default to the driver-owned `ui.stats_overlay`; persist an explicit phone-only override independently and carry it across surfaces/reconnects. Report only observed local counters: draw/video FPS, CPU draw-through-swap time, repeated frames, estimated missed vsyncs and sensor age. End-to-end, GPU and motion-to-photon timings are not inferred from those values. Full latency/thermal/network telemetry is later integration work.


## Live diagnostic reports (2026-11-02)
Send phone diagnostics at 1 Hz over the existing encrypted reliable control channel only when the PC advertises support. Reliable low-rate JSON avoids another wire format and independent report replay state. Validate the complete bounded schema, positive monotonic sequence and percentile relationships; reject invalid reports without refreshing the last valid snapshot. Expire by PC-local reception time after three seconds and scope storage to the active connection. Keep decoder timing in a fixed 120-sample rolling window with nearest-rank percentiles and nonblocking codec-side updates. Expose reports through local RPC and a small read-only diagnostics CLI before the future desktop GUI. Keep FEC group recovery, reassembly loss, sink rejection and decoder drops distinct, since summing them or calling them packet-loss percentage would be inaccurate. The report is local device-to-PC diagnostics, never external analytics. Adaptation/PLL, latency-stage timestamps, temperature/battery reporting and scan-out measurement remain later work.


## First production adaptation stage (2026-11-02)
Integrate bitrate control before resolution/FPS/FEC/PLL changes. Use only fresh report counter deltas and ready clock state; observed failed-frame fraction is not called packet loss. Drops or missed cadence cut by 20%, while two seconds of clean accepted/decoded progress permit bounded 5%/second recovery. Reset baselines after stale feedback or counter resets, and preserve exact fixed quality when disabled. Do not treat the rolling decode percentile as a fresh per-interval overload measurement. Apply only on the encoder worker and distinguish requested targets from encoder-accepted settings; retain requests across resets and contain rejected live updates without dropping the session. Keep shared future full-adaptation algorithms separate until their required measurements and reconfiguration paths are integrated. Record adaptive changes as transient session state, never persistent user settings.


## Desktop setup, runtime calibration and live scene checks (2026-11-02)
Use standard Win32 controls with a background, bounded RPC command queue; desktop polling never blocks the window thread or changes driver ownership. Validate edits with the shared schema. Freeze export/selected revocation targets before modal dialogs to prevent polling from changing the operation. Export measurements/settings without pairing or identity records. Provide an explicit registration helper, preserving other drivers and files. Debug packages include exact reviewed license notices.

Keep successful gyro bias only for the app-process lifetime in natural sensor coordinates, transforming it on display rotation. Surface recreation/reconnect must not demand another still calibration; explicit recalibration clears the cache. Show hold-still text in the lens-corrected headset HUD even when no decoder image exists. Add an opt-in live Android test that performs normal pairing, tracking, fullscreen transition and actual decode/render checks. An original OpenVR color scene separates true stereo submission from runtime system content. Short successful hardware diagnostics are not gameplay or optical acceptance, and initial black-left system content remains a known acceptance item.


## AMF format fallback and backend visibility (2026-11-02)
Advertised AMF BGRA input is not proof that Init will accept it. Retry a failed BGRA Init with a fresh component/context and GPU-converted NV12, releasing the failed attempt before retrying. If it still fails, keep the existing hardware-MFT/software fallback ordering. Record backend identity and hardware status explicitly in pipeline/network/desktop snapshots instead of inferring them from timing. The target RX590 rejects both AMF formats with ACCESS_DENIED; the retry is graceful format negotiation, not a claim that this driver/runtime issue is fixed.

## Cable transport and stable optical aperture (2026-11-02)

Negotiate adb reverse TCP explicitly after the normal authenticated handshake;
bind its separate loopback-only media socket only after a valid encrypted ping.
Reuse packet validation and clock/pose logic, disable FEC, keep frame/socket
queues bounded and terminate partial-record send failures. Allow five seconds
for initial phone allocation and 10 ms for individual media writes to accommodate
adb scheduling. Join connection workers from the session owner before releasing
encoder callbacks. Keep preview surfaces outside dynamic scrolling layout.

Separate the fixed visible lens aperture from the eight-degree expanded render
projection used identically by PC and phone. Clip black optical edges before
rotation; guard pixels absorb small rotational changes without moving that
outline. This costs some pixel density at unchanged resolution and cannot fill
content after arbitrarily large rotations. Use an event to wake PC transmission
on work rather than a nominal 1 ms Windows sleep. Matching directional stereo
patterns and independent flipped-source/bounds tests make orientation explicit.
# Desktop mouse and wired gamepad input (2026-11-03)

Use an explicit foreground input window, defaulting to mouse gaze selection,
and an opt-in XInput/Windows joystick path for the user's disconnected wired
Xbox-style controller. Send bounded latest-state snapshots through the existing
current-user RPC, with a 250 ms lease and neutral-before-arm behavior. Publish
Vive-style inputs on two virtual controllers without putting Windows device
polling or RPC in VR/video callbacks. The right mouse ray follows head forward;
gamepad hands use approximate head-relative offsets. Physical mapping, SteamVR
pointer acceptance, haptics, editable mappings and second-phone input remain
distinct work; do not claim independently tracked hands.

# Setup-only QR and explicit optical import (2026-11-03)

Use a nonsecret, versioned RFC1918 connection URI, never a trust credential.
The existing signed handshake and mutual code confirmation remain mandatory.
Select actual driver listeners in the desktop QR window; keep USB setup separate.
Use pinned permissive Nayuki/ZXing code and the pinned Cardboard DeviceParams
schema with a bounded independent protobuf reader instead of importing the full
Cardboard/Unity runtime. This is a deliberate narrow format integration: it
keeps camera/library work out of the real-time path and rejects unsupported
polynomials rather than silently reducing them. Camera permission is setup-only;
no ARCore, audio or positional tracking is added. Async camera ownership gates
Connect while release completes.

Review active screen/bezel measurements before saving an imported viewer.
Preserve user IPD, extend canonical geometry with vertical lens offset and four
aperture limits, mirror only horizontal limits for the other eye, and synchronize
the same geometry to PC and phone. Old profiles migrate to centered geometry;
failed imports are transactional. Treat top tray alignment as the physical mirror
of bottom alignment (including bezel sign). Generic defaults clear imported
limits. Keep all root Gradle reports under the external build root when set.

# Separate dashboard process and worker (2026-11-03)

Use the pinned MIT ImGui D3D11/Win32 backends and BSD OpenVR API in a separate
application. Select SteamVR's reported adapter and render a persistent dashboard
texture at about 30 Hz only while visible; keep rendering and UI out of driver
pose/encoder callbacks. Translate bounded overlay mouse/scroll/focus events,
release input on hidden/quit/visibility loss, and use a numeric keypad for pairing.

Use a bounded, cancellable worker for existing current-user RPC, logs and ZIP
export. Preserve settings drafts, validate against the shared schema, require
explicit Apply and driver echo, expire stale actions and never retry uncertain
mutations. Missing latency/loss metrics remain unavailable. Reserved input and
thermal features are disabled. Reuse nonsecret LAN QR generation and diagnostic
privacy filters. Add overlay logs and prioritize current logs under a 6 MiB
aggregate budget, keeping the final archive below the existing 8 MiB limit.

Persistent manifest/autolaunch registration is an explicit command, separate
from normal launch; an installer will invoke it later. Supply live-driver desktop
preview and clearly labeled synthetic WARP capture tests for the build PC.
These checks do not establish SteamVR registration, headset usability or GPU
performance acceptance. No mobile/audio/ARCore/NVENC changes are made.

## 2026-11-03: conventional PC controller and mouse priority

For Android display rotation 90, sensor vectors transform as (-y,x,z), and for
270 as (y,-x,z). A world orientation needs the inverse display-basis rotation on
the right, so gyro fusion and the optional Android game-rotation path agree.
Verify the vector mapping against SensorManager.remapCoordinateSystem on the
actual phone, and test physical left/right yaw in both landscape orientations.
Reference: [Android sensor/display remapping example](https://google-developer-training.github.io/android-developer-advanced-course-practicals/unit-1-expand-the-user-experience/lesson-3-sensors/3-2-p-working-with-sensor-based-orientation/3-2-p-working-with-sensor-based-orientation.html).

USB forwarding disappears after cable/adb resets. Keep maintenance outside all
driver streaming callbacks, select one authorized cable device, retain its serial
in memory across reconnects, and use existing bounded/no-rebind rule handling.
Do not silently select another phone, overwrite conflicts, restart the adb server
or change trust. Stop maintenance before explicit rule removal. The home launcher
starts a single-instance helper; no Windows service or login task is installed.

The phone-view repair preserves small late rotational reprojection, normalizes
PC recenter coordinates before transporting each rendered pose, and rejects a
relative rotation above 30 degrees or a texture older than 250 ms. In an origin
discontinuity or stalled stream, retain an unrotated image instead of rotating
rays behind the camera. Use the recenter yaw captured with the submitted layer,
not the value at encoding time. Keep the actual OpenVR pose in diagnostics.

The user prefers an Xbox 360 controller and mouse; defer second-phone controls.
Combine focused mouse buttons with XInput states using maximum trigger/OR button
semantics, so releasing one source cannot cancel the other. An absent controller
falls back to local mouse input. Keep background gamepad control opt-in and gated
on a registered Ctrl+Alt+F8 pause shortcut; never capture background mouse input.
Escape pauses all translated input until explicit resume. Retain driver-side
250 ms lease expiry and neutral rearming across pause, reconnect and source
changes. Native gamepad titles can use their own XInput support without this
translation helper. No driver protocol or Android update is needed.

## 2026-11-03: phone-owned automatic transport selection

Prefer actual tether-subnet UDP discovery over loopback adb TCP, then Wi-Fi.
Keep a healthy route, bound discovery and route retries, retain the target PC's
identity pin when changing addresses, and wait for explicit first-use pairing.
Manual selection remains available; Android tethering must be enabled by its
owner. Rebuild private-interface listeners on stable address changes rather
than sharing mutable socket vectors with receive/accept workers. This can
interrupt a stream and is intentionally documented as reconnect, not migration.
No Windows network category or firewall policy is changed automatically.

## 2026-11-04: default controller bindings and reserved-setting honesty

The driver registers two virtual Vive-style controllers. The input profile
declared the sources the driver can produce but shipped no bindings, so
SteamVR had nothing to bind: a legacy application received no button press, no
controller pose and no haptic output, and the dashboard could not be opened
from a controller. The driver now ships a legacy binding covering the complete
legacy action set (system, grip, application menu, trigger click/pull/touch,
trackpad click/position/touch, both hand poses, both haptic outputs) and a
compositor binding for the dashboard (toggle dashboard, toggle room view,
system chord, screenshot chord, laser pointer click, long-press recenter).

The binding files are authored here. The action names, the file layout and the
`{driver}/input/...` resource-token convention follow the OpenVR SDK v2.15.6
samples and the input-profile format SteamVR consumes, so the legacy action set
matches the names an application reads. The profile resolves `{pocketvr}`
against the driver resource directory, exactly as the shipped Vive and Index
profiles resolve their own driver name. The binding-UI artwork is original; no
Valve artwork is copied.

SteamVR ignores an unknown or misspelled binding without an error, so a
controller can look configured and deliver nothing. The profile and every
binding are therefore validated: the profile identity, the referenced files,
the controller-type agreement, and every `/user/hand/...` path used by a
binding must resolve to a source the profile declares, and every capability the
profile advertises must be backed by an OpenVR input component the driver
actually creates. The reverse direction is checked too, so a component the
driver creates can never be left undeclared. The packaged resource tree is
compared against the source tree, because the driver copy used to run only
when the driver binary was rebuilt and could otherwise ship a stale profile.

Reserved settings are settings whose runtime control is not implemented.
`connection.mode` and `input.recenter_hotkey` stay reserved: the overlay and
the control application do not offer them, the phone does not expose them, and
the driver refuses a runtime edit with an explicit message rather than
accepting a value it cannot honour. A regression test pins both the gate and
the driver refusal, so the honest behaviour cannot decay into a control that
silently does nothing.

## No pairing code; USB helper starts with the driver (2026-11-05)

The owner removed the six-digit pairing step, the pairing window and the USB setup window because the product must be as simple as possible. A phone is now trusted on first connection and remembered. The signed handshake, per-session keys and encrypted media stay, but the code is confirmed internally, so it no longer stops another device on the same private network from connecting. The driver still listens only on private interfaces. The USB helper has no interface and is started by the driver, so the cable link works without the desktop app open.

## A connection replaces the session only after it is accepted (2026-11-05)

The first "newest connection wins" change interrupted the live session as soon as TCP accepted a socket, so a silent port scan could end a game session. The driver now has two control slots: one runs the session and the other lets a newcomer authenticate beside it. The newcomer takes over only after its handshake, hello and decoder capabilities are accepted. An unproven connection may displace only another unproven connection, so a silent stray cannot block a returning phone; the cost is that a stray arriving during a phone's handshake makes that phone retry. Any phone with the app can still take over, because there is no pairing code by owner decision.

## Eye size, sliders, manual resolution and antialiasing (2026-11-05)

The owner found the black border around each eye too large and asked for sliders. The generic profile maps a 90-degree view through the lens curve, so at 100% the picture reaches only about 74% of each eye's width and height. Eye size scales the mesh about each lens axis and leaves the rays alone, so correction is unchanged, though sizes far from 100% make the view slightly magnified or minified relative to head movement. The 135% default is calculated and confirmed on the Pixel, not copied: the picture reaches the edges of each eye, leaving only the corners, and up to 160% can fill those. A manual lens profile fills more at 100%, so it needs a smaller size. The headset view also now draws beside the camera cutout: before, it lost about 137 pixels on one side, which pushed both eyes off centre. The owner asked to match iVRy's view; iVRy was not opened or measured, because of the clean-room rule, so a measured match needs a number from the owner.

Eye size and lens values can change on either side. A value changed on the PC is saved on the phone too, because the phone resends its saved profile at each connection and would otherwise undo PC tuning. At connection the phone's eye size wins; after that the newest change wins.

Manual stream resolution is a ceiling chosen from the sizes the phone already advertises, applied at connection. If the phone cannot decode it the stream uses full size instead of refusing the connection. Antialiasing is supersampling through `video.render_scale`: a streaming driver cannot add anti-aliasing inside a game, which remains a per-game setting.

Changing a lens measurement selects the manual profile, because the generic profile ignores those values. Settings files from older versions are merged over the defaults. Without this, every new setting would make an existing file invalid and silently reset the owner's values.

## Any graphics card, and one click for new phones (2026-11-05)

The owner widened the target: PocketVR must work on any PC that can run SteamVR, including NVIDIA and newer hardware. This reverses "NVENC deferred". NVENC is loaded from the NVIDIA driver at runtime and uses a pinned MIT header, with no bundled binary, and every failure falls back to Media Foundation, which already reaches NVIDIA and Intel through their hardware transforms. AMF stays first on AMD. AV1 and 90/120 Hz were left out: the target phones have no AV1 decoder and 60 Hz is an owner decision.

The owner also approved one-click approval. It keeps the point of the earlier no-code decision, since the first phone and any phone on the USB cable still connect without a click, but a stranger on the same network can no longer take over a session. Only that case asks, and the PC refuses after 60 seconds.

A late frame is now decoded, not dropped. Every later frame depends on it, so dropping one cost a freeze until the next keyframe. Gyro drift is only learned from a window whose mean is under 0.004 rad/s, because the gyro alone cannot separate drift from a very slow deliberate turn, and the existing rule that such turns are preserved wins.

## Timing, bitrate pressure and wired links (2026-11-05)

The driver raises the Windows timer to 1 ms, raises its streaming threads and uses high-resolution timers for vsync and encoder polling. The encoder worker now wakes on an event for new frames. Its old wait could not be woken, so each frame waited for a timer. Encoders do not signal finished output, so it polls every 250 us only while a frame is in flight.

A single missed vsync or replaced frame no longer cuts bitrate. Recovery needs two clean seconds, so isolated phone hiccups drove bitrate to its floor without any link problem. Display failures now count only above 5% of frames; network failures keep their 2% limit. Both limits are starting points and have not been tuned on hardware.

On adb, a media record that cannot start within 10 ms is skipped and the session continues, because nothing reached the stream. Only a partly written record can close it, after 100 ms. The media send buffer is 256 KB, about one frame. A larger one would hide stalls by adding latency.

The phone reports `link: usb` for adb and USB tethering. Wired links skip send pacing, and automatic FEC starts at zero there and rises only if frames are lost. Sending unpaced over tethering is unmeasured on hardware.

## Faster wake-ups, keyframe requests and pacing (2026-11-05)

The driver's UDP receiver now blocks on its sockets, and the phone decoder uses MediaCodec callbacks. Both wake on data instead of sleeping and polling. One worker still makes every codec call, so callbacks only record events.

Keyframe requests also go out on the media path as soon as a frame is lost, because the control loop can wait 100 ms. That path is unreliable, so the control request stays as the backup and the PC treats requests within 250 ms as one.

Send pacing is now a rate based on the average frame at the current bitrate. Small frames finish early, and large frames still fit the spread window. An average frame still takes the whole window, so this does not reduce delay for typical constant-bitrate frames.

Latency CSV writes moved outside `latency_mutex_`, which the send thread takes for every frame. They stay on the control thread, where a slow disk cannot delay video.

Single-buffer display is an opt-in phone experiment using `EGL_KHR_mutable_render_buffer`, off by default. Nothing here has been run on a phone. It has no scan-line timing, so it can tear, and it falls back to normal drawing when the extension is missing.

## AMF picture quality options (2026-11-05)

AMF now requests VBAQ, which moves bits towards flat areas such as sky, and a maximum QP of 42 in place of the default 51. It also states BT.709 studio range, matching the Media Foundation path and the NV12 converter, so the phone gets the colour space from the stream instead of guessing. The VBV buffer now follows live bitrate changes at about 1.2 frames.

These options are requested, not required. If AMF refuses to initialize with them, it retries without them rather than falling back to a slower encoder, because Polaris already needs a workaround to start. The QP limit is a starting point: it can push hard scenes over their bit budget, so compare it on real hardware before keeping it. None of this has run on an AMD card.

## Larger packets and stronger FEC (2026-11-05)

Datagrams grew from 1200 to 1400 bytes and shards from 1024 to 1272. Per-packet overhead falls from about 12% to 10%, and a frame needs about 20% fewer packets. They stay under a 1500-byte link. A smaller link would fragment them, so check this on real tethering and Wi-Fi.

A shard size mismatch connects and then shows no picture, so `hello.protocol` became 2 and the PC tells an old app to update. The binary header version stays at 1.0, which keeps the header test vector valid. An old driver cannot explain the mismatch, so a new app says to update the PC.

Parity now rounds to the nearest shard instead of down, and enabled FEC always gives at least one shard. The default 10% therefore means two shards in 16, or 12.5%, instead of one, or 6.25%. It costs about 6% more data on Wi-Fi, and cuts modelled frame loss at 1% packet loss from about 9% to under 1%.

Shards now go out one from each group in turn, so a burst of losses spreads across groups. Data leaves first and parity is computed afterwards, so frames without loss do not wait for FEC. Coders are cached per thread and multiplication uses a lookup table.

The phone marks its UDP packets as expedited forwarding. This only helps poses and input going up. Video coming down would need a Windows QoS policy, which is not done.

## Sending under pressure, bad reports and phone approval (2026-11-05)

A full UDP send buffer now waits up to 2 ms and retries once. If a packet still cannot leave, it counts as lost and the frame continues while parity could repair it. Abandoning a frame forces a keyframe, the largest frame there is, which fills the buffer again. TCP still stops the frame, because a skipped record cannot be repaired.

Malformed phone reports are logged and ignored. Send and authentication failures still end the session.

`latency.csv` is now limited across sessions. A full file becomes `latency.previous.csv`, so the two together stay near 64 MB.

An Allow click now names the phone that was on screen, by full public key. Before, another phone could take the prompt just before the click and inherit it. Only the newest request is live, and an older one is refused and can retry.

The first phone is now trusted automatically only while the desktop app is open, which narrows the earlier owner decision. Anyone on the network could otherwise become the trusted phone while nobody was looking, for example after Forget or on a fresh install. With the app closed that phone waits for one click, as any new phone does. The SteamVR dashboard does not count as open, because it cannot show the prompt.

Any local program still counts as USB. Telling adb apart from another local process would add little, since a program already running as the user can reach the same files.

## Pairing code removed (2026-11-05)

The six-digit code was already confirmed automatically, so it protected nothing. It is gone from the handshake, replaced by a yes or no answer from the PC. The wire format is unchanged, because the code was never sent.

Signed identities and encryption stay. They are connection security rather than a usage restriction: PocketVR has no licence checks, and anyone can build it and sign their own app.

## Blinking stream, bitrate limit and recenter key (2026-11-05)

Found in a live RX 590 and Pixel 4a session. The picture blinked because automatic resolution treated any stage over one frame time as overload. The Pixel decoder takes 19-25 ms per frame yet delivers a steady 59.8 fps, because hardware stages overlap, so a healthy stream kept switching format, and every switch restarts the encoder and decoder. A switch now also needs frames to be failing to get through. Decoding is judged against frames that arrived, so a game running below 60 fps does not count.

The RX 590 refuses bitrates above its limit. Bitrate could climb past it once single missed vsyncs stopped cutting it, which blocked live updates and made the next encoder restart fall back to Media Foundation. Encoders now report a limit and the pipeline stays below it.

Ctrl+Alt+R now recenters from the PC keyboard, because a phone in a headset cannot be touched.

## Restart prompt, recenter offsets and phone direction (2026-11-05)

The PC app's Restart SteamVR now asks SteamVR to quit and starts it again itself. `RequestRestart` opens a prompt per request, so a button click led to a second question and repeat clicks stacked prompts. The SteamVR dashboard keeps that prompt, limited to one every 30 seconds, because it closes with SteamVR and cannot relaunch it.

Recenter on the phone now also tells the PC to recompute its offset. The two sides kept separate offsets that added together. Recenter still corrects yaw only, by design.

The phone now follows either landscape direction in VR. A locked direction left the picture and tracking upside down for a phone inserted the other way round. Tilting past the flip point mid-session resets tracking, which is acceptable for seated play but should be watched.

## OpenXR games and the tracking universe (2026-11-05)

War Thunder uses OpenXR and showed its whole world rotated 180 degrees, in its own PC window as well as on the phone. SteamVR's raw, seated and standing poses were upright and the stream was not at fault. The headset now reports tracking universe 2, and the owner confirmed the game then looked correct. Universe 2 already had room data on the test PC, left by ALVR. A PC without that data is untested, and SteamVR still shows its Room Setup notice.
