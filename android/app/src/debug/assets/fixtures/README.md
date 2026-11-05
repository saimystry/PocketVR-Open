# First-party stereo decoder fixture

`stereo-256x128.h264` is a single PocketVR-generated IDR access unit with SPS/PPS, encoded by the Windows Media Foundation software fallback. Its left eye is red above green; its right eye is yellow above blue. It is test data covered by the repository's Apache-2.0 license, and is packaged only in the debug app. The Pixel's hardware decoder rejects the smaller 128x64 output format, so this fixture uses 256x128.

Generate it using the project's encoder benchmark:

```powershell
pocketvr_encoder_bench.exe --width 256 --height 128 --frames 1 --fps 60 --bitrate 2000000 --software --warp --stereo-test-pattern --dump stereo-256x128.h264
```

The Android test decodes this exact PC encoder output into a SurfaceTexture and checks four rendered output quadrants through an ImageReader. It does not exercise an Android encoder, download a codec, or claim target-resolution performance.
