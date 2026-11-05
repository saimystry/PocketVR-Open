# Encoded frame-dump debugging

Enable `debug.dump_frames` in PC desktop/dashboard settings while streaming.
The owner-only local RPC accepts `settings.set` with that key and a boolean.
Query `debug.dump.status` for state, error, path, frames, bytes and limits.

Each activation records one Annex-B `.h264` or `.hevc` file under
`%LOCALAPPDATA%\PocketVR\captures`. It waits for a keyframe containing parameter
sets, requests an IDR on live settings changes, and fails after ten seconds
without a usable starting frame. Capture stops at five seconds or 64 MiB,
whichever comes first, without splitting an access unit. Toggle off/on to rearm.
An enabled setting at driver startup arms one capture for that driver lifetime.
Turn it off after diagnosis.

Files contain unencrypted game images, not network keys or packet metadata. They
are local and excluded from diagnostic ZIP export. Files are retained for user
inspection/removal; unique names are created exclusively and never overwritten.

The tap copies into eight preallocated 8 MiB slots using a nonblocking lock.
Settings setup allocates buffers; disabling frees them. A separate worker owns
file creation/writes. Overflow, tap contention or a codec change stops capture
and reports an error; dependent pictures are never appended after a skipped
access unit. A valid prefix may remain. File failures never reset the encoder
or disconnect streaming. This captures encoder output, not network loss or the
final decoded headset image.

Verification (2026-11-03): all 147 Windows checks and dependency audit pass.
Tests cover header/keyframe gating, whole-frame byte limits, duration expiry,
rearming, unique H.264/HEVC files, file-create failure, codec changes and actual
software-MFT file output replayed through an independent MF decoder. A real
authenticated Pixel tether session captured 363 access units / 36,725 bytes
within its bound while the live session remained connected. Sustained live
acceptance is recorded separately in the current handoff/status.

Follow-up: the tap now sees all encoder output before streaming freshness drops,
and a diagnostic exception cannot reset the pipeline. Capacity is eight slots
after real initial file-creation pressure exposed the smaller queue's limit.
All 149 checks pass. Final midstream run: 300 access units / 69,002 bytes with
tracking active; the surrounding strict tether test passes with 1,232 decoded
frames / 8,111 poses. Longer disk-stall and gameplay acceptance remain separate.
