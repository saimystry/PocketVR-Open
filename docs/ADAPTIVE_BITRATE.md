# Automatic session bitrate

`video.adaptive` now controls production-session bitrate adjustment. It defaults to true. The initial bitrate is `video.bitrate_mbps`, clamped to `video.min_bitrate_mbps` / `video.max_bitrate_mbps` when adaptive mode is enabled. Defaults are 60 Mbps initially, with a 15–120 Mbps adaptive range. Decimal Mbps settings are preserved rather than truncated to whole Mbps.

Only fresh, authenticated phone reports with ready clock synchronization can drive the controller. The first report establishes a baseline. Subsequent reports use counter deltas:

- Reassembly failures plus decoder-sink rejections above 2% of observed accepted/failed frames, any new decoder drops, or any new missed display cadence request a 20% bitrate reduction. Cuts are at least 500 ms apart and stop at the minimum.
- After two seconds of clean reports with both accepted and decoded video progress, bitrate recovers at up to 5% per second, capped at the maximum. A long report interval cannot produce a large catch-up increase.
- Idle video, missing/stale reports, unsynchronized clocks and counter resets hold bitrate. Resuming establishes a new baseline. Repeated reports cannot trigger repeated decisions.

These counters indicate frame/display pressure. They are not measured packet loss, and completely unseen frames cannot be counted. The controller does not use stale decoder percentiles as proof of current overload. It is the first production adaptation stage; FEC, resolution, frame-rate, thermal guard, GPU render guard and PLL adjustment remain unfinished.

Setting `video.adaptive` to false applies the selected fixed `video.bitrate_mbps` and disables feedback-driven changes. Live changes to these settings/minimum/maximum through the driver-owned settings RPC take effect without persisting transient adaptive decisions. Other settings broadcasts do not reset the adaptation history. A new connection starts from configured quality rather than inheriting the previous connection's reductions. The phone connection screen shows the encoder setting and pending/rejected updates. The future desktop settings GUI is still required.

The encoder worker applies updates; OpenVR callbacks never invoke encoder configuration. Requested targets persist through encoder resets. A backend rejection blocks further live bitrate attempts for that session, reports the rejection and leaves video running at its last encoder setting. Reconnect retries with a fresh backend. Media Foundation now checks the runtime property's result instead of silently claiming success. Encoder-accepted settings do not establish measured wire bitrate or latency improvement.

`network.status.session.adaptation` and `pocketvr_diagnostics.exe --watch` expose enabled mode, requested/encoder-accepted bitrates, bounds, rejection state, adjustment count, current reason and last change reason. The encrypted `quality` control update mirrors this status to the phone. Accepted bitrate is zero while the encoder is unavailable. Logs record each adaptive target change with its reason.

Verification covers bounded clean recovery, pressure cuts/floor, cooldown, duplicate/missing/idle/reset feedback, fixed quality, real authenticated production-server reports driving a Media Foundation software encoder from 60 to 48 Mbps and back to fixed 60 Mbps while video remains decodable, retained targets through an injected reset, and a rejecting backend that keeps streaming without resetting. Full RX590/Pixel end-to-end congestion and gameplay tuning remains pending.
