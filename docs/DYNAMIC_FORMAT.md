# Resolution and FPS adaptation

Matching PC and Android builds negotiate `format_change:1` during the existing
authenticated handshake. `video.adaptive` enables automatic format selection;
saved width/render scale, FPS and bitrate preferences remain unchanged. Legacy
peers retain the existing fixed format, bitrate adaptation and resource guard.

The startup format is the ceiling. The PC builds a ladder from exact advertised
hardware-decoder width/height/FPS tuples, preserving stereo aspect ratio and
requiring a width divisible by four and an even height. Android probes 2160x1080,
1836x918 (0.85), 1512x756 (0.7), 1188x594 (0.55), plus the existing 1920x960 and
1280x640 compatibility sizes. Supported sizes descend at the configured rate,
then 45/30 fps at the smallest supported size. Unsupported tuples are skipped.
This reduces compression/codec/network work while preserving motion cadence first.
Thermal pressure selects an advertised step at no more than 45/30 fps. Critical
heat remains a video pause under the independent resource guard.

Fresh clock-ready phone reports and advancing decoded counters are required.
Network reassembly pressure and decoder drops/missed display cadence are
distinguished from codec timing.
Processing/decoder/display pressure must persist for two seconds; network pressure
must persist for four seconds at the encoder-accepted bitrate floor. Format
recovery needs fifteen seconds of measured decode/encode spare capacity and no
reassembly drops. Missing measurements, repeated/replayed sequences, idle/reset
counters, stale reports and stale previously-hot thermal state cannot invent
recovery. Every completed change has a five-second settling hold. Bounded debug
recordings hold format selection while recording/waiting for a keyframe.

## In-session format transaction

1. PC gates video, stops/drains the old encoder and its texture leases, clears
   queued compressed frames, and starts a fresh encoder for the proposed format.
   It sends `stream.prepare` with a strictly increasing request ID, exact format
   and PC-clock render timestamp boundary. Poses, clock, input, haptics and control
   continue on the same authenticated sockets and retain their counters/keys.
2. Phone pauses decoder delivery, stops/joins the old codec before reusing the
   Surface, invalidates old renderer metadata and creates the new hardware codec.
   It resets partial reassembly and its rate while preserving cumulative network
   diagnostics. Old-format packets below the boundary are rejected before reassembly.
   `stream.prepared` is sent only after the new codec reports running.
3. After both encoders/decoders are ready, PC sends `stream.commit`. Phone enables
   its replacement decoder and acknowledges `stream.committed`. Only then does
   PC resume video, request parameter-set-bearing IDR and apply the negotiated
   rate together with any stricter resource-guard cap. Display/timewarp stays at
   the phone's display refresh rate. No full authenticated reconnect is required.
4. Rejection, encoder failure or a five-second protocol-stage timeout starts
   `stream.abort` with the previous format and a newer boundary. Both sides rebuild
   that format; phone acknowledges `stream.restored` after its codec is running.
   Video resumes only after both sides are ready. Further automatic format changes
   are disabled for this session. A failed rollback closes the session safely;
   it never streams an uncertain format. Stale/wrong-phase acknowledgments are ignored.

The stream codec and eye projection remain fixed. The GPU scales the complete
stereo compositor output into a bounded three-texture encoder pool, with no new
CPU readback in hardware paths. SteamVR's render-target recommendation stays at
the configured size; this does not reduce the game's own rendering workload.
Compositor frame identities, crypto packet counters and cumulative PC encoded/
drop/reset counters survive encoder replacement. Current codec latency summaries
start fresh. Atomic decoder routing keeps media callbacks bounded and releases
old codecs on the control path, outside the renderer/frame callback.

`network.status` -> `session.adaptation.format` and the phone's stream status show
current/requested format, in-progress state, completed changes and failure/holding
reason. Turning adaptive video off restores the startup format through the same
transaction when format adaptation has not been blocked by a failure. Changing
startup codec/FPS/render preferences themselves still follows existing setup rules.

## Verification and limits

Focused host checks cover capability validation, aspect/ceiling limits, sustained
pressure, slow recovery, stale/idle/replayed/reset feedback, reassembly rate reset,
and authenticated delayed old-format packets. Actual software-MFT/WARP fixtures
switch resolution and FPS over real encrypted UDP and TCP sessions, decode the
new IDR, check both-eye luminance, retain tracking/preferences, and exercise decoder
rejection and missing acknowledgment rollback. Android app/test APKs build and
pass runtime/notices and 16 KiB alignment audits. The full Windows Debug suite
passes (174 passed, one existing SMB hard-link skip, 175 total), including the
final decoder-pressure and stale-thermal refinements. Native build graphs are
checked against current sources. The phone setup status shows resolution/FPS,
an adjusting indicator and a holding notice after a failed change.

No phone is connected on the build PC. Actual Pixel codec replacement/SurfaceTexture
behavior, AMF resizing on RX 590, switch duration/appearance, thermal adaptation,
long sessions and impairment tuning still require home acceptance. Host fixtures
are not measured headset performance. Full display pacing/PLL, automatic codec
selection and complete stage latency analysis remain separate work.
