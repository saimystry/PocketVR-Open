# Live phone diagnostics

The phone sends a bounded diagnostic snapshot about once a second to its authenticated PC over the existing encrypted control connection. No analytics service or additional network destination is used. The PC advertises support; older drivers continue working without receiving these reports.

The connection screen now shows decode p50/p95 and the sample count, plus the number of reports sent. Decode measurements run from MediaCodec input submission to release of a fresh output. They exclude reassembly, the input queue before submission, GPU rendering and scan-out. The rolling window holds the most recent 120 successfully presented outputs, not the most recent 120 attempts or a fixed number of seconds. Dropped/stale outputs are counted separately. A stalled stream can retain old percentiles; these numbers alone must not drive adaptation. The codec worker uses a nonblocking measurement lock and can omit a timing sample during a concurrent snapshot.

On the PC, run the built `pc/tools/pocketvr_diagnostics.exe` once for a JSON snapshot, or use `pocketvr_diagnostics.exe --watch` for one JSON line per second. Ctrl+C stops watching. The tool reads only the current-user local driver pipe. It reports a clear error if SteamVR/the driver is absent; watch mode retries. The desktop GUI remains unfinished.

The `network.status` RPC response includes `session.phone_stats`:

- `available`: a valid report was received within three seconds, measured by the PC clock.
- `age_ms`: time since receipt, or null before the first report.
- `seq`: the most recent accepted positive sequence number.
- `rejected`: malformed/repeated/out-of-order reports rejected in this session.
- `values`: the accepted metrics, or null when absent/stale.

Disconnect clears the entire session snapshot. Reconnect starts a new sequence and empty snapshot. Invalid reports leave the last good values and their reception time unchanged. Control framing/authentication failures retain the existing fail-closed behavior.

Reported values:

| Fields | Meaning |
|---|---|
| `clock_ready`, `rtt_ms` | Phone clock-sync state and estimated UDP round-trip time; not one-way latency |
| `media_packets`, `media_bytes` | Cumulative authenticated received UDP packets/whole datagram bytes, including FEC and clock replies |
| `accepted_frames`, `sink_rejected_frames` | Completed frames accepted/rejected by the decoder sink, including old completed frames rejected by the phone link |
| `reassembly_dropped_frames` | Incomplete/evicted/expired frame slots counted by reassembly; completely unseen frames cannot be counted |
| `fec_recovered_groups` | Groups requiring FEC recovery, not a count of frames or missing packets |
| `decoder_submitted`, `decoder_decoded`, `decoder_dropped` | Decoder-stage cumulative counters; these overlap other pipeline stages and must not be summed into an overall loss percentage |
| `decode_samples`, `decode_p50_ms`, `decode_p95_ms` | Rolling nearest-rank percentiles; zero values with zero samples mean unavailable |
| `display_fps`, `video_fps` | Sampled completed EGL swaps versus newly latched video textures |
| `render_cpu_ms` | Latest CPU draw-through-swap duration; not GPU execution time |
| `repeated_frames`, `missed_vsync` | Renderer counters described in `PHONE_RENDERER.md` |

Snapshots combine independently sampled counters and may straddle a frame transition. No packet-loss percentage, end-to-end latency, motion-to-photon latency, thermal guard or automatic bitrate/FEC/resolution change is claimed.

Verification: 74 Windows CTest checks and license audit pass. Tests exercise bounded percentile eviction, invalid measurements, report schema/replay handling, PC-clock expiry, recovery, and authenticated production-server report delivery while preserving video streaming and clearing state on disconnect. Pixel 4a instrumentation executes the shared tests and verifies real hardware-decoder percentile output alongside the existing GPU/calibration checks. These are synthetic/device diagnostics checks, not full SteamVR gameplay acceptance.
# Transport diagnostics update

USB uses the same authenticated media counters as UDP; sealed packet bytes
exclude TCP's four-byte record prefix and TCP/IP overhead. Clock RTT is for the
active media transport. PC endpoint diagnostics now expose actual queried UDP
receive/send socket buffer sizes and whether buffer tuning calls succeeded;
requested buffer sizes are not assumed to have been granted by the OS.

