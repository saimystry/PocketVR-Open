# Phone renderer and headset stats

2026-11-03 repair: remove the driver's recenter yaw from the exact submitted
render orientation before sending it to the phone. The yaw is captured at layer
submission, so a later recenter cannot relabel a queued layer. Reprojection
rejects a relative rotation above 30 degrees and stops rotating a texture held
longer than 250 ms. In those cases the existing image is shown without late
rotation rather than sampling rays through/behind the camera into a black wedge.
Normal small late rotations and lens correction stay enabled. This bounds a
reference discontinuity; it does not reconstruct pixels outside the render guard.

The original fault was captured as mostly black, severely stretched stereo
wedges. With the repair installed and reprojection enabled, the actual Pixel
fullscreen capture shows the matching upright UP pattern in both eyes. Windows
checks and actual phone native/GPU tests pass; a continuous live run delivered
1,236 decoded frames, 1,215 latched textures and 8,106 poses. Full gameplay,
thermal and optical acceptance remain separate.

A second continuous hardware run includes desktop recenter with a nonzero
origin yaw (-0.2914 radians): 1,222 decoded / 1,216 latched frames and 8,125
poses, with instrumentation result -1. The captured post-recenter OpenVR pose,
after origin removal, matches the sensor orientation; the stereo screenshot
remains upright. One intervening run lost its USB media connection; the clean
retry passes, so extended USB/reconnect stability is still separate acceptance.

Choose **Enter headset view** for immersive landscape rendering. Android Back returns to setup. **Lens calibration grid** works offline with saved viewer geometry. No measured Shinecon profile is supplied.

The PC and phone use the same eight-degree render guard on each projection
edge. Physical lens/screen geometry and the visible aperture remain unchanged;
extra rendered pixels provide room for small late rotations. In black-edge mode
the optical aperture is clipped before rotation, so its outline stays fixed on
the screen. When late rotation goes beyond the guard, black-aperture mode now
extends the source eye's edge pixels instead of introducing a second moving
black rectangle. This can stretch edge content; it cannot reconstruct unseen
scene detail. The guard reduces pixel density at unchanged stream resolution. Update
the PC driver and APK together when using this projection change.

The original live diagnostic now uses matching red-over-green images with an
upward arrow and UP lettering in both eyes, making orientation directly comparable.
The decoder fixture retains different eye colours for automated eye isolation.

**Headset stats · phone preference** offers **Use PC setting**, **Off**, **Minimal**, and **Full**. The default follows `ui.stats_overlay` from the driver. A phone override is saved locally, survives app/surface recreation and reconnects, and does not alter the PC setting. Text is duplicated in both eyes, lens corrected and head locked. Minimal shows two rows; Full adds two more:

| Label | Meaning |
|---|---|
| DRAW FPS | Completed EGL draws/swaps per sampled interval; includes repeated video |
| VIDEO FPS | Newly latched, metadata-matched decoded textures per sampled interval |
| CPU DRAW-SWAP | Previous callback's CPU elapsed time including swap/backpressure; not GPU time |
| REPEAT | Cumulative draws reusing the same video frame |
| MISS | Estimated missed 60 Hz vsync intervals; not a scan-out measurement |
| SENSOR AGE | Age of the latest local pose when sampled, or `--` without a usable timestamp; not motion-to-photon latency |

Counters refresh about twice per second. Initial FPS is zero until the first sample window completes. Repeats and misses reset when the renderer surface is recreated. Full end-to-end latency, bitrate/loss, encode/decode distributions and thermal telemetry remain separate work.

The renderer owns GLES3/EGL on a dedicated native Choreographer thread. MediaCodec produces an external SurfaceTexture. A bounded timestamp/session table matches the actual latched texture to its render pose and projection ID; skipped decoded outputs cannot borrow another frame's pose. Stale-session or mismatched-projection frames are withheld. Held frames are sampled again using the newest predicted local orientation. For active local-to-world quaternions, sampling uses `inverse(q_render) * q_now`; the inverse convention maps old geometry forward. Prediction and pose freshness are bounded. PC recenter/tracker-origin alignment still needs end-to-end verification.

Mesh storage, indices and the 256x32 first-party bitmap text buffer are preallocated. Stats formatting and texture uploads run at the sample cadence or on mode changes, without heap allocation in the GL callback. No external font dependency is shipped.

Verification uses a first-party 256x128 H.264 fixture encoded by the PC Media Foundation software encoder, decoded by the Pixel's advertised hardware decoder and rendered into an ImageReader. Pixel assertions cover stereo color/orientation, held-frame repeats, an offline grid, synthetic-yaw timewarp, both-eye stats visibility, Minimal/Full/Off/PC-default switching and invalid-mode rejection. Saved PNGs contain only synthetic test output. This is not full-resolution SteamVR streaming, physical optics, sustained 60 fps or gameplay acceptance.
