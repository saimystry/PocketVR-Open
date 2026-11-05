# Local tracker bridge

The Windows driver accepts length-prefixed UTF-8 JSON on `\\.\pipe\PocketVR.v1`. This pipe rejects remote clients and grants access only to the current Windows user's SID. A tracker must run as that user. Do not expose or forward it over a network. This intentionally chooses the prompt's named-pipe option rather than an unauthenticated UDP JSON listener.

Each request and response is u32 little-endian byte length (1–65536), then JSON. A successful response has `"ok":true`; rejected input returns `"ok":false` with an error string. An invalid length closes the connection. Read/write operations time out after eight seconds; desktop integrations should use a worker thread.

```
{"method":"bridge.pose","device":0,
 "orientation":[0,0,0,1],"angular_velocity":[0,0,0],
 "position":[0,0,0],"positioned":false,
 "timestamp_pc_ns":123456789000}
```

Orientation is xyzw, active local-to-world, right-handed (+X right, +Y up, -Z forward). Angular velocity is body-local in rad/sec. Optional position is world meters and is used only with `positioned:true`. Timestamp is Windows QPC nanoseconds, matching Python `time.monotonic_ns()` on Windows; omit it to stamp reception locally. Samples over one second old or over 100 ms into the future are rejected. Quaternions must be unit length within tolerance, and all fields must be finite. Device 0 is the HMD; controller IDs are added with the controller milestone.

`{"method":"recenter"}` recenters yaw. `{"method":"status"}` reports driver state. `{"method":"settings.get"}` returns the settings values and schema. `{"method":"settings.set","key":"video.fps","value":45}` validates and atomically saves an update.

Run `python pc/tools/examples/tracker_bridge.py --seconds 10` to feed a synthetic yaw trace. It requires the driver to be running in SteamVR and is not a phone/encoder acceptance test. The example uses only the Python standard library. Proprietary tracker protocols and SDKs are not integrated; adapt their independently obtained poses to this API.
