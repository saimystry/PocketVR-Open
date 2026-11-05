# Architecture and validation boundaries

`pc/common` is the same C++20 protocol/math/timing/FEC/reassembly implementation cross-compiled for Android. UDP media packets are independently authenticated; dropped or unauthenticated packets enter FEC as erasures. Reassembly allocates bounded frame/parity pools at construction. Streaming crypto supports caller-owned output buffers, while vector-returning convenience APIs are used by tools.

The driver owns settings, poses, the selected D3D11 GPU and the local current-user-only RPC endpoint. Desktop and dashboard applications are planned clients of the driver; pre-SteamVR setup ownership still needs implementation. PC audio keeps using the existing Windows playback device. Runtime settings are stored in `%APPDATA%\PocketVR\pocketvr.json`; logs rotate under `%LOCALAPPDATA%\PocketVR\logs`. DPAPI protects the PC identity and paired records with the current user's credentials.

`config/settings.schema.json` is the checked source for the default settings catalog. `scripts/generate_settings.py` produces the default JSON, embedded C++ catalog and SteamVR defaults; CTest rejects stale generated files. Updates validate before committing values and atomically replacing the saved file. The generic Cardboard v2 profile is the initial fallback because no physical viewer has been selected. A real viewer should be calibrated using QR/manual parameters before visual acceptance.

Phone orientation is an active unit quaternion from local sensor/eye space to world. CPU timewarp tests distinguish rotating old geometry into the current eye from transforming a current ray into the old texture's camera. Physical phone/SteamVR display cadence stays 60 Hz; delivered video frames may be fewer, with phone reprojection required to remain at 60 Hz.

OpenVR interfaces are checked against SDK 2.15.6. The initial HMD reports a fictional undistorted display, supplies bounded predicted poses and flags stale tracking. The pose snapshot uses sequentially consistent atomic fields with a bounded read retry, so `GetPose` never acquires the writer mutex or performs I/O. Direct-mode ownership follows Valve's requirement to acquire/release the compositor's sync texture once per present and cache opened shared handles. See [Valve direct-mode documentation](https://github.com/ValveSoftware/openvr/wiki/Driver-direct-mode) and [Microsoft shared NT-handle documentation](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11device1-opensharedresource1).

## Measurements

See `spec.md` for what has and has not been measured on real hardware.
