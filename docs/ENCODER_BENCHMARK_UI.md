# Desktop encoder benchmark

Choose **Benchmark** in the desktop control app, or run the sibling
`pocketvr_benchmark.exe`. SteamVR and the phone are not required. Opening the
window does not run a benchmark; choose **Run comparison** explicitly.

Stop phone streaming first. The window refuses to start if the driver reports an
active phone session. Keep streaming stopped throughout the comparison, and
avoid other GPU-heavy work. It does not stop a game or change driver settings.

Choose:

- H.264 or HEVC.
- Stereo resolution: 2160x1080, 1728x864 or 1280x720.
- Configured stream rate: 60, 45 or 30 FPS.
- Target bitrate: 10-150 Mbps.
- Samples per preset: 30-600 frames (default 120).
- Compare Fast/Balanced/Quality sequentially, or run just one preset.
- Optional sampled luma quality, which adds PC software decoding and is slower.

All selected presets receive the same dimensions, codec, rate, bitrate, frame
count and quality-sampling choice. The comparison requires a hardware encoder
and rejects software/WARP fallback, even at lower resolutions. Unsupported
codecs/settings produce a failed row with details; they never become successful
hardware results. Other requested presets can still complete.

The table shows the actual selected encoder, hardware status, encode p50/p95,
target/encoded bitrate and optional sampled luma PSNR. Details identify the actual
GPU. Backend fallback from AMF to hardware Media Foundation is visible. On the
build PC this would identify the build PC GPU, not the RX 590 at home.

Interpretation:

- Encode p50/p95 are the encoder's recorded sample timings. The benchmark uses
  sequential synthetic frames; samples include startup behavior and no warm-up
  exclusion is applied. This is not a running VR game's pipeline measurement.
- **Encoded Mbps** is encoded payload bytes scaled to the configured frame rate.
  It is not measured network throughput and excludes encryption/FEC overhead.
  Synthetic scene complexity can produce an encoded rate below the target.
- Serial wall FPS includes CPU image generation and optional PC decoding. It is
  displayed separately in details and is not sustained streaming/display FPS.
- Optional quality is sampled software-decoded luma PSNR, capped at 100 dB. It
  is not a physical headset quality verdict or a chromatic/lens test. Optional
  decode timing belongs to the PC's software decoder, not the Pixel. A missing
  Windows software codec can make this optional test fail; retry without it.
- The reference target of encode p95 below 8 ms applies to the required
  2160x1080 target configuration. It is a goal to measure, not a guarantee.
- No phone decode, network latency, timewarp or motion-to-photon timing is
  measured here. RX 590 acceptance must be run on that actual GPU.

The window remains responsive while a separate benchmark process runs. Each
preset has a five-minute command deadline and a bounded captured-output buffer.
**Cancel** terminates only the current benchmark process, skips remaining presets
and retains completed rows. Closing the window also cancels its worker.

After completion, failure or cancellation, **Save report** saves available rows
and their requested settings, capture UTC, actual backend/GPU, measurements and
errors as local JSON. It preserves failed/cancelled status instead of inventing
metrics for them. Nothing is uploaded or automatically applied to settings.

Build the updated CLI and window together. The window locates
`pocketvr_encoder_bench.exe` beside itself or in the sibling `tools` directory,
matching the normal `pc/control` and `pc/tools` build layout. The frontend target
depends on the CLI target. CLI `--machine-readable` adds one versioned compact
result line; human-readable CLI output and `--output` remain supported.
`--require-hardware` explicitly rejects diagnostic software/WARP requests.

Validation separates implementation checks from performance: synthetic parser
tests reject mismatched/fallback/invalid reports, while a two-frame 256x128
WARP/software fixture verifies the real CLI report/optional-quality path and
hardware-only rejection. Those fixture timings are not RX 590 performance data.
No live VR performance acceptance is claimed.
Benchmark UI build and test logs use `benchmark-ui-*.log`.

2026-11-03 verification: Windows Debug build, installed dependency/SDK license
audit, hidden benchmark/control startup and 115 CTest checks pass. One existing
diagnostics hard-link check is skipped on the network share (116 total).
The actual CLI two-frame software fixture passes; no target-GPU timings were
recorded. Detailed results are in `<build root>`.
