# Manual viewer calibration

Open **Viewer calibration** in the Android app. Enter the physical measurements in millimeters, then choose **Save calibration**. A valid profile is saved on the phone, survives app restarts, and is sent over the authenticated control connection when a compatible PC driver is connected. The status says **Calibration applied on PC** only after that driver confirms the update. Reconnecting reapplies the phone's saved manual profile.

- **Lens to screen** is the distance from the screen to the lenses. Match the viewer's physical adjustment.
- **Lens center spacing** is the distance between the two lens centers. It determines where the optical centers sit over the display.
- **Your IPD** is the distance between your pupils. It controls the driver's eye-to-head transforms separately from physical lens spacing.
- **Active screen width/height** describe the illuminated display in landscape, without bezels. These can be measured directly; the defaults are generic starting values.
- **k1/k2** describe radial lens distortion. They are not known measurements for a Shinecon SC-G13/G12/G10.
- **Field of view scale** adjusts the angular extent derived from the geometry.

The app displays the supported numeric ranges. Non-finite values, missing fields, out-of-range values, lens centers outside the screen, and unsupported extreme fields of view are rejected. Invalid edits leave the saved profile and PC configuration intact. **Use generic values** fills the form with starting values without saving them; **Cancel** discards edits. Saving selects the manual profile and clears the separate legacy distortion tweak, since k1/k2 are explicitly supplied. The form scrolls above fixed Save/Cancel controls, and keeps the screen awake only while it is open.

The PC saves the complete accepted calibration atomically, reports separate mirrored left/right projection bounds, and updates IPD independently. Frames carry the updated projection identifier. Existing settings files receive defaults for only the three new physical geometry fields; unrelated settings are preserved. An older driver without this extension leaves the calibration saved on the phone and reports that a newer PC driver is needed.

The dedicated SurfaceTexture/EGL renderer now applies the saved lens geometry and late rotational reprojection. Enable **Lens calibration grid** for an offline tuning pattern, then use **Enter headset view** for immersive landscape display; Android Back returns to setup. Software cannot change physical lens focus. Vertical lens alignment, QR calibration and measured headset acceptance remain pending. See `PHONE_RENDERER.md`.

Verification: 70 Windows Debug CTest checks pass, including manual projection reference values, IPD separation, settings persistence/migration, rejected-edit rollback, and encrypted production-server calibration updates followed by video delivery with the new projection ID. Android app/test APKs build and pass artifact/license and 16 KB ZIP alignment checks. Native tests and dedicated Kotlin/JNI calibration persistence checks execute successfully on the Pixel 4a. Device tests use a temporary preferences file and do not replace the user's calibration.
