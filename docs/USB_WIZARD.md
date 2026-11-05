# Desktop USB setup wizard

2026-11-03 automatic reconnect repair: the home `Start PocketVR.cmd` launcher
now runs the helper in `--maintain` mode after the driver's USB listeners are
ready. It selects exactly one authorized cable device and retains that selection
until it exits. Every two seconds it checks that phone's mappings, creating only
missing exact TCP control/media tuples via the existing bounded/no-rebind setup.
Unauthorized, wireless/emulator and ambiguous multiple-phone entries are not
configured. Existing conflicting or unrelated rules are preserved. Restart the
launcher to choose a different phone. Maintenance exits after the driver remains
unavailable, creates no login task/service, and never installs an APK or changes
pairing. **Remove PocketVR rules** stops maintenance before removing mappings.
The helper also accepts `--stop-maintain`; starting the launcher enables it again.
`--maintain --once` checks/restores once and prints JSON; `--probe` remains
strictly read-only. The selected driver's live loopback ports are authoritative.

Validation: native reconnect tests cover rule loss, idempotence, ambiguity,
authorization, sticky device selection and conflicts. Actual Pixel mappings
were removed deliberately and automatically restored before a passing live USB
fullscreen test. This models lost forwarding, not every USB cable/driver failure.

Build the Windows targets, then choose **USB setup** in `pocketvr_control.exe`.
The sibling `pocketvr_usb.exe` also runs independently, including while SteamVR
is stopped. Keep the control and USB executables together in the build's
`pc/control` directory. This feature needs no Android APK change.

1. The wizard looks for `adb.exe` in `ANDROID_SDK_ROOT`, `ANDROID_HOME`, the
   normal per-user Android SDK location and absolute PATH directories. It does
   not search arbitrary drives or the working directory. If needed, choose
   **Browse** and select Android platform-tools' `adb.exe`. A browsed path is
   saved in `%LOCALAPPDATA%\PocketVR\usb-setup.json`; a path typed directly is
   used for the current window. Missing platform-tools produces an installation/
   browsing instruction; nothing is downloaded automatically.
2. Connect the phone with a USB data cable. Enable Developer options and USB
   debugging, unlock the phone, and accept **Allow USB debugging**. Refresh the
   list after approving. Unauthorized, offline, recovery and missing-device
   states show separate guidance. Install the appropriate Windows USB driver if
   the phone never appears. The helper does not change USB debugging settings.
3. Select the phone. One cable candidate is selected automatically; multiple
   phones require an explicit selection. Recognized wireless adb and emulator
   entries are displayed but cannot be configured by this cable wizard. Windows
   adb does not always expose a USB device path; candidate classification uses
   serial/transport naming and is not a physical-cable attestation.
4. Check the PC control/media ports. When launched from a connected desktop
   app, the wizard receives its current driver ports; otherwise defaults are
   9943/9944. Ports must match the driver and phone and must differ. The wizard
   does not silently change either endpoint's settings.
5. Choose **Set up USB**. The helper creates and verifies the two matching
   `adb reverse` TCP rules. Existing matching rules are reused. A conflicting
   destination blocks the operation before either configured port is changed.
6. On the phone enable **USB (adb reverse)** and connect to `127.0.0.1` with the
   matching control port. Start SteamVR with the driver at the PC where it is
   installed, and pair normally. Forwarding readiness alone does not prove the
   driver is listening or that video/tracking works.

**Refresh** rechecks devices and mappings without changing rules. Windows USB
device-change notifications also schedule a refresh; they never trigger setup
or removal. After unplug/replug, refresh and set up again if mappings disappeared.
Changing the selected phone also refreshes its authorization and forwarding state.

**Remove PocketVR rules** removes only the two matching source/destination tuples
for the selected phone and ports. It never issues `--remove-all`, clears pairing,
or affects unrelated rules. Conflicting destinations are preserved. Matching
tuples cannot identify which program originally created them; select the intended
phone and PocketVR ports before removing them. Rules and paired identities are
separate.

**Cancel** interrupts the current command and attempts bounded cleanup of newly
created rules whose creation succeeded. Setup uses `--no-rebind` and rechecks
before mutations, so conflicting/concurrent destinations are not overwritten.
Rollback preserves pre-existing rules and different replacement destinations.
An interrupted adb command can have an unknown outcome; refresh to inspect it.
Cleanup failure and partial removal are explicitly reported. adb has no atomic
two-rule transaction or conditional-remove primitive, so another program should
not edit these same two tuples during setup/removal.

The helper runs adb as an explicit executable with escaped arguments, no shell,
restricted inherited handles, a 64 KiB output cap, cancellation and per-command
deadlines. Device discovery/forwarding runs on a dedicated desktop worker,
outside the driver/encoder callbacks. It terminates only its own timed-out adb
command, never kills/restarts the shared adb server or revokes authorization.
Closing the wizard cancels work; already configured rules remain installed.
The helper does not start SteamVR, install APKs or enable USB tethering.

For a read-only scripted probe with JSON output:

```powershell
& .\pocketvr_usb.exe --probe --adb 'C:\Android\platform-tools\adb.exe' | Out-String
```

This reads the device list and each authorized cable candidate's reverse table.
It does not create/remove rules, save preferences or launch the phone app. Treat
probe output as local diagnostics: it contains device serials and mapping names.

Automated coverage checks authorization/multiple-device parsing, conflicting and
unrelated rules, idempotence, rollback, cancellation, partial removal, SDK/path
discovery, paths with spaces, literal argument escaping, stderr/exit status,
subprocess deadlines and output caps. The actual connected Pixel 4a passed the
new read-only probe without changing its empty forwarding table. Live setup/
removal and SteamVR streaming through the wizard still need hardware acceptance;
this build PC has no Steam/SteamVR. USB tethering detection remains a separate
v1 feature. Builds and evidence stay under `<build root>` on the approved
build PC share.

2026-11-03 checkpoint: fresh Windows Debug build, installed dependency/SDK
license audit and 111 CTest checks pass. One existing diagnostics hard-link check
is skipped because the build PC share misreports link counts (112 checks total).
Both the wizard and desktop control pass hidden startup checks. Evidence:
`<build root>`, `usb-wizard-build.log`,
`usb-wizard-incremental.log` and the local-only `usb-wizard-probe.json`.

References: [Android adb setup](https://developer.android.com/tools/adb),
[AOSP adb command reference](https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/docs/user/adb.1.md).
