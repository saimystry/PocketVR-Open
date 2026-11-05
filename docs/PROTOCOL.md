# PocketVR wire protocol v1.0

Negotiated in-session format extension: matching hello/ready messages advertise
`format_change:1`. Authenticated `stream.prepare` carries a request ID, exact
width/height/FPS and PC render-time boundary. Phone answers `stream.prepared` only
after codec readiness; `stream.commit` / `stream.committed` enable video. Failure
uses `stream.abort` with the previous format and newer boundary, acknowledged by
`stream.restored`; `stream.rejected` triggers rollback. No wire video-header,
session-key, packet-counter or projection change is made. Old packets below the
boundary are discarded. See DYNAMIC_FORMAT.md for ordering, limits and timeouts.

All integers and IEEE-754 floats are little endian, with no struct padding. Major/minor are exactly 1/0 until negotiated extensions are implemented. Reject reserved fields, flags and types that are not defined here. Ports default to TCP 9943 and UDP 9944. The entire UDP datagram, including authentication tag, is at most 1200 bytes.

## Authenticated UDP envelope

The implemented binary layout is pinned to **1.0**. Newer minor versions are
rejected until their layout is explicitly negotiated/implemented; capability
extensions on encrypted control do not silently change this layout. An unused
range-negotiation helper has been removed to avoid implying otherwise.

| Offset | Bytes | Field |
|---|---|---|
| 0 | 4 | ASCII PVR1 |
| 4 | 1 | Major (1) |
| 5 | 1 | Minor (0) |
| 6 | 1 | Type: VIDEO=1, FEC=2, POSE=3, INPUT=4, VSYNC=5, STATS=6, HAPTIC=7, PING=8, PONG=9, KEYFRAME=10 |
| 7 | 1 | Flags: 1 = encrypted; other values rejected |
| 8 | 4 | Nonzero session identifier |
| 12 | 2 | Device identifier (0 is HMD) |
| 14 | 2 | Reserved zero |
| 16 | 8 | Monotonic, nonzero directional packet counter |
| 24 | 8 | PC-clock send time in ns |
| 32 | variable | Ciphertext, at most 1152 bytes |
| end-16 | 16 | ChaCha20-Poly1305-IETF tag |

The nonce is session-u32 followed by counter-u64, both little endian. The full cleartext header is AEAD associated data. Counter space is shared by all message types/devices in a direction. Each direction has a different key. Do not reset a cipher counter or reuse an instance with a new peer. An exhausted counter terminates the session. Fresh X25519 key pairs and a transcript containing both nonces produce new directional keys every session. A 1024-packet replay window is advanced only after authentication succeeds. PacketCipher and ReplayWindow have a single owning network thread.

X25519 exchange primitives do not constitute pairing. Complete the signed commit/reveal and mutual confirmation in [AUTHENTICATION.md](AUTHENTICATION.md) before exposing control or media services. The shared core starts no production listener itself.

## Pose (65 plaintext bytes)

Sequence u32, sensor timestamp u64 in PC ns, position xyz float32, orientation xyzw float32, body-frame angular velocity xyz float32 (radians/sec), linear acceleration xyz float32 (m/s²), tracking state u8 (0 lost, 1 valid 3DoF, 2 externally positioned). Quaternions actively rotate eye/body coordinates to the right-handed world (+X right, +Y up, -Z forward). Reject non-finite floats, nonunit quaternions outside 1% squared-norm tolerance, angular speeds over 100 rad/sec and acceleration magnitudes over 1000 m/s².

## Video/FEC plaintext

The metadata is 78 bytes: frame_id u64, render_ns u64, frame_bytes u32, projection_id u32, packet_index u16, packet_count u16, slice u16, fec_group u16, shard_bytes u16, k u8, m u8, shard_index u8, flags u8 (bit 0 IDR), followed by render pose (position xyz, quaternion xyzw, angular velocity xyz: 40 bytes). The remaining 1024 bytes are one padded shard. Packet count refers to data shards only; frame_bytes removes padding at final assembly. Every packet carries identical frame pose/projection metadata. Conflicting metadata is rejected.

Shards are 1272 bytes and datagrams at most 1400. `hello.protocol` is 2; a phone and PC must match, and the PC tells a mismatched phone to update. Data shards are sent one from each group in turn, followed by parity in the same order. Reassembly does not depend on order.

Groups contain up to 16 data shards and 0–8 parity shards. All but the final group have 16 data shards. Data shard packet_index = group*16 + shard_index; a parity packet uses packet_index = group*16. Reassembly limits the frame size, slot count, lifetime and shard indices before any indexing. Production FEC policy limits overhead to 25%; algorithm tests exercise up to eight erasures. XOR supports exactly one parity shard. RS uses GF(256), primitive polynomial 0x11d, a Vandermonde matrix transformed to systematic form, and explicit zero padding. FEC protects plaintext shards inside independently authenticated UDP packets; corrupted or unauthenticated packets are erasures.

A completed frame view belongs to the reassembler's preallocated pool and is valid until that slot is reused. A consumer must finish use/copy before the next push can reuse it. Pending frames expire after the configured deadline (default two 60 Hz intervals). Frames more than one display interval behind the newest completed render timestamp are rejected. Loss/eviction sets an IDR request flag for the session controller.

The render quaternion is expressed in the phone sensor tracking reference used
by incoming poses. The driver removes its desktop recenter yaw from the submitted
OpenVR orientation before sending it, using the yaw captured with that layer.
The original OpenVR matrix remains available in `compositor.status`. This keeps
late sampling from comparing recentered PC coordinates with phone coordinates.

## Reliable control framing

TCP records have a u32 body length followed by exactly that many bytes (1–65536). Before authentication the bodies are fixed-size handshake messages; afterward they are authenticated encrypted JSON records, as specified in [AUTHENTICATION.md](AUTHENTICATION.md). The framer supports arbitrary fragmentation/coalescing. TCP_NODELAY is enabled and I/O has bounded deadlines. A partial send timeout closes the connection rather than corrupting the next record. The USB media adapter carries the same <=1200-byte media/pose packets inside these records, using reusable buffers.

Pre-authentication commit/reveal/signature records reveal public device identity
metadata, pin-claim bits and record sizes to an observer on the local network.
They carry no identity seeds or session keys. Authentication/integrity does not
promise anonymity, padded traffic sizes or full handshake metadata privacy.

v1 transport supports private RFC1918, link-local and loopback IPv4 literals.
IPv6 and CGNAT address space are not currently supported. Listener bind failures
are retried by the network manager with its one-second backoff, preserving
exclusive Windows port ownership rather than allowing another process to share
the endpoint.

## Manual viewer calibration extension

The authenticated PC `ready` message advertises `viewer_calibration: 1` and includes `projection` with `id`, `left`, `right`, `screen_left`, and `screen_right`. Each bounds array is positive tangent extents `[left, right, up, down]`; screen bounds precede distortion, while left/right describe the visible distorted projection. Current builds also expose `render_left` and `render_right`, expanded by eight degrees per edge (bounded below tangent 20) for late-rotation guard pixels. The PC raw render projection and phone sampler use these expanded bounds; the fixed optical aperture and HUD use visible bounds. Update both endpoints together for this projection change. An older peer may omit this extension. A phone must not send extension messages unless it is advertised.

The phone sends `{"type":"viewer.set","request_id":1,"values":{...}}`, using a positive signed-64-bit-range request identifier. The complete `values` object contains `screen_to_lens_m`, `inter_lens_distance_m`, `screen_width_m`, `screen_height_m`, `k1`, `k2`, `ipd_mm`, and `fov_scale`. The shared settings schema defines the numeric ranges; physical geometry is checked too. No extra or missing fields are accepted. A valid request sets the manual profile, clears the separate distortion tweak, and persists the entire transaction before publishing it.

Success is `{"type":"viewer.result","request_id":1,"ok":true,"settings":{...},"projection":{...}}`. Failure preserves the previous settings/projection and returns the same type/identifier with `ok:false` and `error`. Invalid calibration does not end an otherwise valid session. The phone ignores stale acknowledgments after a newer edit and resends its latest saved calibration after reconnecting. The PC requests an IDR after applying a calibration; subsequent frames carry its projection identifier. Runtime SteamVR projection transitions and actual phone rendering still require hardware acceptance.

## Phone diagnostics extension

An authenticated PC advertises `phone_stats: 1` in `ready`. A compatible phone sends `{"type":"phone.stats","seq":1,"values":{...}}` about once per second on the encrypted reliable control channel. Sequence numbers are positive integers up to INT64_MAX, strictly increasing within one session. Unknown/missing metric fields, wrong types, non-finite/out-of-range timings, invalid percentile order and replayed sequences are rejected without replacing or refreshing the last accepted snapshot. Reports receive no acknowledgment; the phone labels its counter as reports sent. The connection owns the snapshot and resets it on disconnect/reconnect. It expires after three seconds using PC-local reception time.

`pc/common/include/pocketvr/telemetry.hpp` defines the complete required fields and validation. Counters are nonnegative signed-64-bit-range integers, `decode_samples` is 0..120, FPS is finite 0..240, timing fields are finite 0..60000 ms, and `clock_ready` is boolean. Zero decode samples require zero percentiles; p50 cannot exceed p95. Diagnostics use the existing control channel to avoid unauthenticated or independently sequenced metric state; no new UDP wire layout is introduced. See `PHONE_DIAGNOSTICS.md` for exact measurement meanings and limitations. Full VSYNC/STATS-driven adaptation remains unfinished.

## Adaptive bitrate status extension

The PC advertises `adaptive_bitrate: 1` in `ready` and sends `{"type":"quality","values":{...}}` when production bitrate status changes. Values match `network.status.session.adaptation`: enabled mode, requested/encoder-accepted bitrate in bps, configured adaptive bounds, rejection flag, adjustment count, current reason and last-change reason. These are encoder configuration acknowledgments/status, not measured transport bitrate. Transient adjustments do not rewrite shared settings or projection IDs. No new media wire format is introduced. `video.adaptive=false` selects fixed bitrate; rejected backend updates are surfaced without terminating video. See `ADAPTIVE_BITRATE.md` for the deliberately limited controller scope.

## Clocks and reprojection

Clock exchange records phone send p0, PC receive c1, PC send c2, phone receive p3. Estimated network RTT is (p3-p0)-(c2-c1). Filter using minimum-RTT samples over a 128-observation window, fit a centered linear regression for skew using low-RTT samples, and anchor offset to the minimum-RTT observation. Half-path asymmetry remains an uncertainty and must be measured rather than reported as exact one-way latency.

For a current eye ray v, the sampling ray into a previously rendered eye is inverse(q_render)*q_now*v. The inverse transform inverse(q_now)*q_render moves rendered geometry into the current eye. CPU tests verify their inverse relationship and the world-ray identity. Physical display cadence remains 60 Hz even when video delivery is reduced.

## Explicit adb TCP media negotiation

After authentication, `hello.transport` may request `tcp` (default/legacy is
`udp`). TCP is accepted only for loopback control peers with an available
loopback TCP media listener. `ready.stream` reports `transport`, `tcp_port` and
the retained `udp_port`; the default TCP media port is 9944. The client must
reject a missing TCP acknowledgment when explicitly requesting cable mode.

`hello.link` is optional: `usb` for a cable route, including UDP over USB
tethering, or `network`. A missing value means `network`. It only tunes sending:
a wired link turns off send pacing and starts automatic FEC at zero.

KEYFRAME media packets ask the PC for a keyframe without waiting for the phone's
control loop. They may be lost, so the phone still sends the control `idr` message;
the PC treats requests within 250 ms as one.

The separate media socket carries the same authenticated packet envelope inside
u32-length-prefixed records. Its first record must authenticate as a session
PING before binding the media peer; unauthenticated sockets cannot bind it.
Binding has a five-second deadline, including phone startup allocations. The
same directional cipher/replay/clock/pose checks then apply. TCP uses no FEC.
Media EOF closes the session. Packet/byte counters exclude the four-byte TCP
record prefix and transport/IP overhead. See [USB.md](USB.md) for adb setup.
