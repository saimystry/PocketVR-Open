# External controller tracking bridge

The current-user-only local RPC accepts `bridge.pose` device 0 (HMD, existing),
1 (left controller), or 2 (right). Controller poses are independent of the phone
HMD. A real external tracker/provider must supply them; this API does not create
positional tracking from Xbox input or the phone IMU.

Example controller request:

```json
{"method":"bridge.pose","device":1,"position":[-0.2,1.3,-0.4],"orientation":[0,0,0,1],"linear_velocity":[0,0,0],"angular_velocity":[0,0,0],"space":"standing"}
```

Orientation is x/y/z/w, linear velocity is metres/second in the supplied position
space, and angular velocity is body-local radians/second. All vectors must be
finite; positions are bounded to 1,000 metres, speeds/angular speeds to 100, and
quaternions must be unit length within validation tolerance. Controller position
is required. Omitted velocities are zero, never estimated from head motion.
Optional timestamp_pc_ns uses the PC monotonic/QPC clock; default is receipt time.
Older/equal controller timestamps are rejected. Maximum accepted age is one second
and future allowance 100 ms; valid tracking requires age at most 250 ms.

`space: tracking` (default) applies PocketVR's recenter yaw to positions,
orientations and world velocity. Origin is the PocketVR tracking/floor frame;
positions are not offsets added to the head. `space: standing` means already
aligned PocketVR/OpenVR driver-world coordinates and applies no extra recenter.
It does not automatically convert another driver's seated/chaperone universe.
Providers must calibrate that alignment; bridge.status exposes recenter yaw and
eye height. Release ownership before changing coordinate space.

Additional prediction is capped at 80 ms and the returned poseTimeOffset describes
the pose actually returned. Body angular velocity is transformed to driver-world
axes, following the [OpenVR pose contract](https://github.com/ValveSoftware/openvr/blob/master/docs/Driver_API_Documentation.md#poses).
Position, velocity and coordinate-space metadata are published coherently.

Read `bridge.status` for both ownership/freshness/pose records. Send
`{"method":"bridge.release","device":1}` to explicitly restore synthetic input
for that hand. A stopped/crashed tracker leaves its hand invalid after 250 ms and
disconnected after two seconds; it never silently falls back to a synthetic pose.
Fresh tracking after loss needs neutral hand input before buttons rearm. Ownership
changes release input leases. Existing mouse/Xbox/phone mappings still supply
buttons; tracked pose alone does not press anything.

```powershell
python pc/tools/examples/tracker_bridge.py --device 1 --space standing --seconds 5
python pc/tools/examples/tracker_bridge.py --status
python pc/tools/examples/tracker_bridge.py --device 1 --release
```

The synthetic example releases controllers on normal exit. Device 0 remains the
HMD example and must not compete with live phone tracking. Controllers 1/2 can
run alongside the phone. `pocketvr_stereo_scene --inspect` now includes only the
PocketVR controllers' matrices/validity/velocity under each OpenVR origin.

Verification: 165 Windows checks/audit and actual Pixel suites pass. Tests cover
separate hands, source-space transforms, coherent velocity publication, bounded
prediction, stale/disconnected poses, validation/replay, explicit release and
neutral rearming. Actual SteamVR inspection confirmed both injected independent
poses while phone tracking ran, then invalid poses after updates stopped, followed
by explicit release. Surrounding strict tether test: one connection, 2,515 decoded
frames / 8,664 poses. Real external-tracker calibration/accuracy/gameplay remains
acceptance work; synthetic injection is not physical tracked-controller acceptance.
